// ============================================================================
// Http.hpp -- HTTP layer (refactored: uses modular net/ instead of server.hpp)
// ============================================================================

#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <sys/stat.h>
#include <chrono>
#include <unordered_map>
#include <cassert>

// Modular net/ headers (replaces monolithic server.hpp)
#include "net/TcpServer.h"
#include "net/TcpConnection.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Any.h"

// Forward declarations
class LoopThreadPool;

// Rate limiter integration
#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"

#define DEFAULT_TIMEOUT 10

// ============ HTTP Status Codes ============
static std::unordered_map<int, std::string> _statu_msg = {
    {100,"Continue"},{101,"Switching Protocol"},{200,"OK"},{201,"Created"},
    {204,"No Content"},{206,"Partial Content"},{301,"Moved Permanently"},
    {302,"Found"},{304,"Not Modified"},{400,"Bad Request"},{401,"Unauthorized"},
    {403,"Forbidden"},{404,"Not Found"},{405,"Method Not Allowed"},
    {408,"Request Timeout"},{413,"Payload Too Large"},{414,"URI Too Long"},
    {429,"Too Many Requests"},{500,"Internal Server Error"},
    {502,"Bad Gateway"},{503,"Service Unavailable"}
};

// ============ MIME Types ============
static std::unordered_map<std::string, std::string> _mime_msg = {
    {".html","text/html"},{".htm","text/html"},{".css","text/css"},
    {".js","text/javascript"},{".json","application/json"},
    {".png","image/png"},{".jpg","image/jpeg"},{".jpeg","image/jpeg"},
    {".gif","image/gif"},{".ico","image/vnd.microsoft.icon"},
    {".svg","image/svg+xml"},{".txt","text/plain"},{".xml","application/xml"},
    {".pdf","application/pdf"},{".zip","application/zip"},
    {".mp3","audio/mpeg"},{".mp4","video/mp4"},{".webm","video/webm"}
};

// ============ Utility ============
class Util {
public:
    static bool IsDirectory(const std::string& path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    static std::string GetMime(const std::string& path) {
        size_t pos = path.rfind('.');
        if (pos == std::string::npos) return "application/octet-stream";
        auto it = _mime_msg.find(path.substr(pos));
        return it != _mime_msg.end() ? it->second : "application/octet-stream";
    }
    static std::string UrlDecode(const std::string& str) {
        std::string result;
        for (size_t i = 0; i < str.size(); i++) {
            if (str[i] == '%' && i + 2 < str.size()) {
                int val;
                sscanf(str.substr(i+1,2).c_str(), "%x", &val);
                result += static_cast<char>(val);
                i += 2;
            } else if (str[i] == '+') {
                result += ' ';
            } else {
                result += str[i];
            }
        }
        return result;
    }
};

// ============ HTTP Context (per-connection state) ============
enum RecvStatu { RECV_HTTP_ERROR, RECV_HTTP_LINE, RECV_HTTP_HEAD, RECV_HTTP_BODY, RECV_HTTP_OVER };

class HttpRequest {
public:
    std::string _method;
    std::string _path;
    std::string _version;
    std::string _body;
    std::unordered_map<std::string, std::string> _headers;
    std::unordered_map<std::string, std::string> _params;

    std::string GetHeader(const std::string& key) const {
        auto it = _headers.find(key);
        return it != _headers.end() ? it->second : "";
    }
    std::string GetParam(const std::string& key) const {
        auto it = _params.find(key);
        return it != _params.end() ? it->second : "";
    }
    void SetParam(const std::string& key, const std::string& val) { _params[key] = val; }
};

class HttpResponse {
public:
    int _statu;
    std::string _body;
    std::unordered_map<std::string, std::string> _headers;
    bool _close;

    HttpResponse(int statu = 200) : _statu(statu), _close(true) {
        _headers["Content-Type"] = "text/html";
    }
    void SetContent(const std::string& body, const std::string& type = "text/html") {
        _body = body;
        _headers["Content-Type"] = type;
    }
    void SetHeader(const std::string& key, const std::string& val) { _headers[key] = val; }
    void SetClose(bool close) { _close = close; }
    bool Close() const { return _close; }
};

class HttpContext {
public:
    HttpContext() : _resp_statu(200), _recv_statu(RECV_HTTP_LINE), _body_len(0) {}

    void RecvHttpRequest(Buffer* buf);
    HttpRequest& Request() { return _request; }
    int RespStatu() const { return _resp_statu; }
    RecvStatu RecvStatuVal() const { return _recv_statu; }
    void ReSet() {
        _request = HttpRequest();
        _resp_statu = 200;
        _recv_statu = RECV_HTTP_LINE;
        _body_len = 0;
    }

private:
    bool ParseHttpRequestLine(const std::string& line);
    bool ParseHttpHeadLine(const std::string& line);

    HttpRequest _request;
    int _resp_statu;
    RecvStatu _recv_statu;
    size_t _body_len;
};

// ============ HttpContext Implementation ============
void HttpContext::RecvHttpRequest(Buffer* buf) {
    // Request line
    if (_recv_statu == RECV_HTTP_LINE) {
        const char* crlf = nullptr;
        for (size_t i = 0; i + 1 < buf->ReadAbleSize(); i++) {
            if (buf->peek()[i] == '\r' && buf->peek()[i+1] == '\n') {
                crlf = buf->peek() + i; break;
            }
        }
        if (!crlf) return;
        std::string line(buf->peek(), crlf - buf->peek());
        buf->MoveReadOffset(line.size() + 2);
        if (!ParseHttpRequestLine(line)) {
            _resp_statu = 400; _recv_statu = RECV_HTTP_ERROR; return;
        }
        _recv_statu = RECV_HTTP_HEAD;
    }
    // Headers
    if (_recv_statu == RECV_HTTP_HEAD) {
        const char* crlf = nullptr;
        for (size_t i = 0; i + 1 < buf->ReadAbleSize(); i++) {
            if (buf->peek()[i] == '\r' && buf->peek()[i+1] == '\n') {
                crlf = buf->peek() + i; break;
            }
        }
        if (!crlf) return;
        std::string line(buf->peek(), crlf - buf->peek());
        buf->MoveReadOffset(line.size() + 2);
        if (line.empty()) {
            // Empty line => headers done
            auto it = _request._headers.find("Content-Length");
            if (it != _request._headers.end()) {
                _body_len = std::stoul(it->second);
                _recv_statu = RECV_HTTP_BODY;
            } else {
                _recv_statu = RECV_HTTP_OVER;
            }
        } else {
            if (!ParseHttpHeadLine(line)) {
                _resp_statu = 400; _recv_statu = RECV_HTTP_ERROR;
            }
        }
    }
    // Body
    if (_recv_statu == RECV_HTTP_BODY) {
        if (buf->ReadAbleSize() >= _body_len) {
            _request._body.assign(buf->peek(), _body_len);
            buf->MoveReadOffset(_body_len);
            _recv_statu = RECV_HTTP_OVER;
        }
    }
}

bool HttpContext::ParseHttpRequestLine(const std::string& line) {
    std::istringstream iss(line);
    if (!(iss >> _request._method >> _request._path >> _request._version)) return false;
    // Parse query string
    size_t qpos = _request._path.find('?');
    if (qpos != std::string::npos) {
        std::string qs = _request._path.substr(qpos + 1);
        _request._path = _request._path.substr(0, qpos);
        size_t pos = 0;
        while (pos < qs.size()) {
            size_t eq = qs.find('=', pos);
            size_t amp = qs.find('&', pos);
            if (amp == std::string::npos) amp = qs.size();
            if (eq < amp)
                _request._params[qs.substr(pos, eq-pos)] = Util::UrlDecode(qs.substr(eq+1, amp-eq-1));
            pos = amp + 1;
        }
    }
    return true;
}

bool HttpContext::ParseHttpHeadLine(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    val.erase(0, val.find_first_not_of(" \t"));
    _request._headers[key] = val;
    return true;
}

// ============ HttpServer ============
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;

    HttpServer(int port, int timeout = DEFAULT_TIMEOUT)
        : _loop(), _server(&_loop, InetAddress(static_cast<uint16_t>(port)), "HttpServer"),
          _basedir("."), _rate_limiter(nullptr) {
        _server.setConnectionCallback(
            std::bind(&HttpServer::OnConnected, this, std::placeholders::_1));
        _server.setMessageCallback(
            std::bind(&HttpServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
    }

    void SetBaseDir(const std::string& path) {
        assert(Util::IsDirectory(path)); _basedir = path;
    }
    void SetRateLimiter(RateLimitService* rl) { _rate_limiter = rl; }
    void setThreadPool(LoopThreadPool* pool) { _server.setThreadPool(pool); }
    void SetThreadCount(int count) { _threadCount = count; }
    EventLoop* getLoop() { return &_loop; }

    void Get(const std::string& pattern, const Handler& handler) {
        _get_route.push_back({std::regex(pattern), handler});
    }
    void Post(const std::string& pattern, const Handler& handler) {
        _post_route.push_back({std::regex(pattern), handler});
    }
    void Put(const std::string& pattern, const Handler& handler) {
        _put_route.push_back({std::regex(pattern), handler});
    }
    void Delete(const std::string& pattern, const Handler& handler) {
        _delete_route.push_back({std::regex(pattern), handler});
    }

    void Listen() {
        _server.start();
        _loop.loop();
    }

private:
    // Get client IP from fd
    static std::string GetClientIp(sockfd_t fd) {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd, (sockaddr*)&addr, &len) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            return ip;
        }
        return "unknown";
    }

    // Static file handler
    bool IsFileHandler(HttpRequest& req, HttpResponse* rsp) {
        std::string path = _basedir + req._path;
        if (req._path.back() == '/') path += "index.html";
        if (!Util::IsDirectory(path)) {
            std::ifstream file(path, std::ios::binary);
            if (file.is_open()) {
                std::string body((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
                rsp->SetContent(body, Util::GetMime(path));
                return true;
            }
        }
        return false;
    }

    void ErrorHandler(const HttpRequest& req, HttpResponse* rsp) {
        std::string body = "<html><body><h1>" + std::to_string(rsp->_statu) + " "
                         + (_statu_msg.count(rsp->_statu) ? _statu_msg[rsp->_statu] : "Unknown")
                         + "</h1></body></html>";
        rsp->SetContent(body);
    }

    void WriteResponse(const TcpConnectionPtr& conn, const HttpRequest& req, HttpResponse& rsp) {
        if (_statu_msg.count(rsp->_statu))
            rsp.SetHeader("Server", "cpp-http/1.0");
        if (rsp.Close())
            rsp.SetHeader("Connection", "close");

        std::ostringstream oss;
        oss << req._version << " " << rsp._statu << " "
            << (_statu_msg.count(rsp->_statu) ? _statu_msg[rsp->_statu] : "Unknown") << "\r\n";
        for (auto& [k, v] : rsp._headers)
            oss << k << ": " << v << "\r\n";
        oss << "Content-Length: " << rsp._body.size() << "\r\n";
        oss << "\r\n" << rsp._body;

        conn->send(oss.str());
    }

    bool Dispatcher(HttpRequest& req, HttpResponse* rsp,
                    const std::vector<std::pair<std::regex, Handler>>& routes) {
        for (auto& [pattern, handler] : routes) {
            if (std::regex_match(req._path, pattern)) {
                handler(req, rsp);
                return true;
            }
        }
        return false;
    }

    void Route(HttpRequest& req, HttpResponse* rsp) {
        if (req._method == "GET" && IsFileHandler(req, rsp)) return;
        if (req._method == "GET" || req._method == "HEAD") {
            if (Dispatcher(req, rsp, _get_route)) return;
        } else if (req._method == "POST") {
            if (Dispatcher(req, rsp, _post_route)) return;
        } else if (req._method == "PUT") {
            if (Dispatcher(req, rsp, _put_route)) return;
        } else if (req._method == "DELETE") {
            if (Dispatcher(req, rsp, _delete_route)) return;
        }
        rsp->_statu = 405;
    }

    // ============ Rate Limit Check ============
    bool CheckRateLimit(const TcpConnectionPtr& conn, HttpRequest& req, HttpResponse* rsp) {
        if (!_rate_limiter) return false;

        std::string clientIp = GetClientIp(conn->fd());
        std::string userId = req.GetHeader("X-User-Id");
        if (userId.empty()) userId = req.GetParam("user_id");
        if (userId.empty()) userId = clientIp;

        Request rlReq;
        rlReq.userId = userId;
        rlReq.ip = clientIp;
        rlReq.api = req._path;
        rlReq.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        Decision d = _rate_limiter->process(rlReq);

        if (d == Decision::REJECT) {
            rsp->_statu = 429;
            ErrorHandler(req, rsp);
            return true;
        }
        if (d == Decision::CHALLENGE) {
            rsp->_statu = 403;
            rsp->SetContent("{\"error\":\"challenge_required\"}", "application/json");
            return true;
        }
        return false;
    }

    void OnConnected(const TcpConnectionPtr& conn) {
        conn->setContext(HttpContext());
    }

    void OnMessage(const TcpConnectionPtr& conn, Buffer* buffer) {
        while (buffer->ReadAbleSize() > 0) {
            HttpContext* context = conn->getContext()->get<HttpContext>();
            context->RecvHttpRequest(buffer);
            HttpRequest& req = context->Request();
            HttpResponse rsp(context->RespStatu());

            if (context->RespStatu() >= 400) {
                ErrorHandler(req, &rsp);
                WriteResponse(conn, req, rsp);
                context->ReSet();
                buffer->MoveReadOffset(buffer->ReadAbleSize());
                conn->shutdown();
                return;
            }
            if (context->RecvStatuVal() != RECV_HTTP_OVER) return;

            // ============ Rate Limit Intercept ============
            if (CheckRateLimit(conn, req, &rsp)) {
                WriteResponse(conn, req, rsp);
                context->ReSet();
                buffer->MoveReadOffset(buffer->ReadAbleSize());
                if (rsp.Close()) conn->shutdown();
                return;
            }

            Route(req, &rsp);
            WriteResponse(conn, req, rsp);
            context->ReSet();
            if (rsp.Close()) conn->shutdown();
        }
    }

private:
    EventLoop _loop;
    TcpServer _server;
    std::string _basedir;
    int _threadCount = 1;
    RateLimitService* _rate_limiter;

    std::vector<std::pair<std::regex, Handler>> _get_route;
    std::vector<std::pair<std::regex, Handler>> _post_route;
    std::vector<std::pair<std::regex, Handler>> _put_route;
    std::vector<std::pair<std::regex, Handler>> _delete_route;
};
