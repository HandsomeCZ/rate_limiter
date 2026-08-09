// ============================================================================
// Http.hpp — HTTP 层 (基于原项目 + 限流风控集成)
// ============================================================================

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <regex>
#include <sys/stat.h>
#include "../server.hpp"
#include <chrono>

// ============ 限流风控集成 ============
#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"

#define DEFALT_TIMEOUT 10

std::unordered_map<int, std::string> _statu_msg = {
    {100,"Continue"},{101,"Switching Protocol"},{102,"Processing"},{103,"Early Hints"},
    {200,"OK"},{201,"Created"},{202,"Accepted"},{203,"Non-Authoritative Information"},
    {204,"No Content"},{205,"Reset Content"},{206,"Partial Content"},
    {207,"Multi-Status"},{208,"Already Reported"},{226,"IM Used"},
    {300,"Multiple Choice"},{301,"Moved Permanently"},{302,"Found"},
    {303,"See Other"},{304,"Not Modified"},{305,"Use Proxy"},{306,"unused"},
    {307,"Temporary Redirect"},{308,"Permanent Redirect"},
    {400,"Bad Request"},{401,"Unauthorized"},{402,"Payment Required"},
    {403,"Forbidden"},{404,"Not Found"},{405,"Method Not Allowed"},
    {406,"Not Acceptable"},{407,"Proxy Authentication Required"},{408,"Request Timeout"},
    {409,"Conflict"},{410,"Gone"},{411,"Length Required"},{412,"Precondition Failed"},
    {413,"Payload Too Large"},{414,"URI Too Long"},{415,"Unsupported Media Type"},
    {416,"Range Not Satisfiable"},{417,"Expectation Failed"},{418,"I'm a teapot"},
    {421,"Misdirected Request"},{422,"Unprocessable Entity"},{423,"Locked"},
    {424,"Failed Dependency"},{425,"Too Early"},{426,"Upgrade Required"},
    {428,"Precondition Required"},{429,"Too Many Requests"},
    {431,"Request Header Fields Too Large"},{451,"Unavailable For Legal Reasons"},
    {501,"Not Implemented"},{502,"Bad Gateway"},{503,"Service Unavailable"},
    {504,"Gateway Timeout"},{505,"HTTP Version Not Supported"},
    {506,"Variant Also Negotiates"},{507,"Insufficient Storage"},{508,"Loop Detected"},
    {510,"Not Extended"},{511,"Network Authentication Required"}
};

std::unordered_map<std::string, std::string> _mime_msg = {
    {".aac","audio/aac"},{".abw","application/x-abiword"},{".arc","application/x-freearc"},
    {".avi","video/x-msvideo"},{".azw","application/vnd.amazon.ebook"},
    {".bin","application/octet-stream"},{".bmp","image/bmp"},
    {".bz","application/x-bzip"},{".bz2","application/x-bzip2"},
    {".csh","application/x-csh"},{".css","text/css"},{".csv","text/csv"},
    {".doc","application/msword"},
    {".docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".eot","application/vnd.ms-fontobject"},{".epub","application/epub+zip"},
    {".gif","image/gif"},{".htm","text/html"},{".html","text/html"},
    {".ico","image/vnd.microsoft.icon"},{".ics","text/calendar"},
    {".jar","application/java-archive"},{".jpeg","image/jpeg"},{".jpg","image/jpeg"},
    {".js","text/javascript"},{".json","application/json"},
    {".jsonld","application/ld+json"},{".mid","audio/midi"},{".midi","audio/x-midi"},
    {".mjs","text/javascript"},{".mp3","audio/mpeg"},{".mpeg","video/mpeg"},
    {".mpkg","application/vnd.apple.installer+xml"},
    {".odp","application/vnd.oasis.opendocument.presentation"},
    {".ods","application/vnd.oasis.opendocument.spreadsheet"},
    {".odt","application/vnd.oasis.opendocument.text"},{".oga","audio/ogg"},
    {".ogv","video/ogg"},{".ogx","application/ogg"},
    {".otf","font/otf"},{".png","image/png"},{".pdf","application/pdf"},
    {".ppt","application/vnd.ms-powerpoint"},
    {".pptx","application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".rar","application/x-rar-compressed"},{".rtf","application/rtf"},
    {".sh","application/x-sh"},{".svg","image/svg+xml"},
    {".swf","application/x-shockwave-flash"},{".tar","application/x-tar"},
    {".tif","image/tiff"},{".tiff","image/tiff"},{".ttf","font/ttf"},
    {".txt","text/plain"},{".vsd","application/vnd.visio"},
    {".wav","audio/wav"},{".weba","audio/webm"},{".webm","video/webm"},
    {".webp","image/webp"},{".woff","font/woff"},{".woff2","font/woff2"},
    {".xhtml","application/xhtml+xml"},{".xls","application/vnd.ms-excel"},
    {".xlsx","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xml","application/xml"},{".xul","application/vnd.mozilla.xul+xml"},
    {".zip","application/zip"},{".3gp","video/3gpp"},{".3g2","video/3gpp2"},
    {".7z","application/x-7z-compressed"}
};

class Util {
public:
    static size_t Split(const std::string& src, const std::string& sep,
                        std::vector<std::string>* out) {
        size_t start = 0;
        while (start < src.size()) {
            size_t pos = src.find(sep, start);
            if (pos == std::string::npos) { out->push_back(src.substr(start)); break; }
            if (pos > start) out->push_back(src.substr(start, pos - start));
            start = pos + sep.size();
        }
        return out->size();
    }
    static bool IsDirectory(const std::string& path) {
        struct stat st;
        return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    }
    static bool IsRegularFile(const std::string& path) {
        struct stat st;
        return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    }
    static std::string Extension(const std::string& path) {
        size_t pos = path.rfind('.');
        return pos == std::string::npos ? "" : path.substr(pos);
    }
    static std::string UrlDecode(const std::string& src) {
        std::string result;
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '%' && i + 2 < src.size()) {
                int val = 0;
                for (int j = 1; j <= 2; ++j) {
                    char c = src[i + j]; val <<= 4;
                    if (c >= '0' && c <= '9') val += c - '0';
                    else if (c >= 'a' && c <= 'f') val += c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') val += c - 'A' + 10;
                }
                result += static_cast<char>(val); i += 2;
            } else if (src[i] == '+') { result += ' '; }
            else { result += src[i]; }
        }
        return result;
    }
    // 去掉尾部的 \r 和 \n
    static std::string TrimCRLF(const std::string& s) {
        std::string r = s;
        while (!r.empty() && (r.back() == '\r' || r.back() == '\n')) r.pop_back();
        return r;
    }
};

// ============ 从连接 fd 提取客户端 IP ============
static std::string GetClientIp(int sockfd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr*)&addr, &len) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return ip;
    }
    return "unknown";
}

class HttpRequest {
public:
    std::string _method, _path, _version, _body;
    std::unordered_map<std::string, std::string> _headers, _params;
    bool _has_body = false;

    void SetMethod(const std::string& m) { _method = m; }
    void SetPath(const std::string& p) { _path = p; }
    void SetVersion(const std::string& v) { _version = v; }
    void SetBody(const std::string& b) { _body = b; _has_body = true; }
    void AddHeader(const std::string& k, const std::string& v) { _headers[k] = v; }
    void AddParam(const std::string& k, const std::string& v) { _params[k] = v; }

    std::string GetHeader(const std::string& k) const {
        auto it = _headers.find(k); return it != _headers.end() ? it->second : "";
    }
    std::string GetParam(const std::string& k) const {
        auto it = _params.find(k); return it != _params.end() ? it->second : "";
    }
    bool HasHeader(const std::string& k) const { return _headers.count(k) > 0; }
    void Clear() {
        _method.clear(); _path.clear(); _version.clear(); _body.clear();
        _headers.clear(); _params.clear(); _has_body = false;
    }
};

class HttpResponse {
public:
    int _statu;
    std::string _body;
    std::unordered_map<std::string, std::string> _headers;
    bool _close = true;

    HttpResponse(int statu = 200) : _statu(statu) {
        _headers["Content-Type"] = "text/html";
        _headers["Connection"] = "close";
    }
    void SetContent(const std::string& body, const std::string& type = "text/html") {
        _body = body; _headers["Content-Type"] = type;
    }
    void SetHeader(const std::string& k, const std::string& v) { _headers[k] = v; }
    bool Close() const { return _close; }
    void SetClose(bool c) { _close = c; }
};

enum RecvStatu { RECV_HTTP_ERROR, RECV_HTTP_LINE, RECV_HTTP_HEAD, RECV_HTTP_BODY, RECV_HTTP_OVER };

class HttpContext {
private:
    HttpRequest _request;
    int _resp_statu;
    RecvStatu _recv_statu;
    size_t _content_length;

    bool ParseHttpLine(const std::string& line) {
        std::vector<std::string> parts;
        if (Util::Split(line, " ", &parts) != 3) return false;
        _request.SetMethod(parts[0]);
        std::string path = parts[1];
        size_t qpos = path.find('?');
        if (qpos != std::string::npos) {
            std::string query = path.substr(qpos + 1);
            path = path.substr(0, qpos);
            std::vector<std::string> pairs;
            Util::Split(query, "&", &pairs);
            for (auto& p : pairs) {
                size_t eq = p.find('=');
                if (eq != std::string::npos)
                    _request.AddParam(Util::UrlDecode(p.substr(0, eq)), Util::UrlDecode(p.substr(eq+1)));
                else
                    _request.AddParam(Util::UrlDecode(p), "");
            }
        }
        _request.SetPath(Util::UrlDecode(path));
        _request.SetVersion(parts[2]);
        return true;
    }

    bool ParseHttpHead(const std::string& line) {
        if (line.empty()) {
            if (_request.HasHeader("Content-Length"))
                _content_length = std::stoul(_request.GetHeader("Content-Length"));
            _recv_statu = (_content_length > 0) ? RECV_HTTP_BODY : RECV_HTTP_OVER;
            return true;
        }
        size_t pos = line.find(": ");
        if (pos == std::string::npos) return false;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 2);
        _request.AddHeader(key, val);
        if (key == "Content-Length") _content_length = std::stoul(val);
        if (key == "Connection" && val == "keep-alive") _resp_statu = 200;
        return true;
    }

public:
    HttpContext() : _resp_statu(200), _recv_statu(RECV_HTTP_LINE), _content_length(0) {}

    void RecvHttpRequest(Buffer* buf) {
        switch (_recv_statu) {
        case RECV_HTTP_LINE: {
            std::string line = buf->GetLineAndPop();
            if (line.empty()) return;
            if (!ParseHttpLine(Util::TrimCRLF(line))) {
                _resp_statu = 400; _recv_statu = RECV_HTTP_ERROR; return;
            }
            _recv_statu = RECV_HTTP_HEAD;
            RecvHttpRequest(buf);
            break;
        }
        case RECV_HTTP_HEAD: {
            std::string line = buf->GetLineAndPop();
            if (line.empty()) return;
            std::string trimmed = Util::TrimCRLF(line);
            if (!ParseHttpHead(trimmed)) {
                _resp_statu = 400; _recv_statu = RECV_HTTP_ERROR; return;
            }
            if (_recv_statu == RECV_HTTP_HEAD) RecvHttpRequest(buf);
            else if (_recv_statu == RECV_HTTP_BODY) RecvHttpRequest(buf);
            break;
        }
        case RECV_HTTP_BODY: {
            if (buf->ReadAbleSize() < _content_length) return;
            _request.SetBody(std::string(buf->ReadPosition(), _content_length));
            buf->MoveReadOffset(_content_length);
            _recv_statu = RECV_HTTP_OVER;
            break;
        }
        default: break;
        }
    }

    HttpRequest& Request() { return _request; }
    int RespStatu() const { return _resp_statu; }
    RecvStatu RecvStatuVal() const { return _recv_statu; }
    void ReSet() {
        _resp_statu = 200; _recv_statu = RECV_HTTP_LINE;
        _content_length = 0; _request.Clear();
    }
};

using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;
using RouteTable = std::vector<std::pair<std::regex, Handler>>;

class HttpServer {
private:
    TcpServer _server;
    std::string _basedir;
    RouteTable _get_route, _post_route, _put_route, _delete_route;

    // ============ 限流风控 ============
    RateLimitService* _rate_limiter = nullptr;

    void WriteReponse(const PtrConnection& conn, const HttpRequest& req, HttpResponse& rsp) {
        std::string resp;
        resp += req._version + " " + std::to_string(rsp._statu) + " " +
                (_statu_msg.count(rsp._statu) ? _statu_msg[rsp._statu] : "Unknown") + "\r\n";
        rsp._headers["Content-Length"] = std::to_string(rsp._body.size());
        if (rsp.Close()) rsp._headers["Connection"] = "close";
        else rsp._headers["Connection"] = "keep-alive";
        for (auto& [k, v] : rsp._headers) resp += k + ": " + v + "\r\n";
        resp += "\r\n" + rsp._body;
        conn->Send(resp.data(), resp.size());
    }

    void ErrorHandler(const HttpRequest& req, HttpResponse* rsp) {
        std::string body = "<html><body><h1>" + std::to_string(rsp->_statu) + " " +
                           (_statu_msg.count(rsp->_statu) ? _statu_msg[rsp->_statu] : "Error") +
                           "</h1></body></html>";
        rsp->SetContent(body);
    }

    bool IsFileHandler(const HttpRequest& req) {
        if (_basedir.empty()) return false;
        std::string path = _basedir + req._path;
        if (req._path == "/") path += "index.html";
        return Util::IsRegularFile(path);
    }

    void FileHandler(const HttpRequest& req, HttpResponse* rsp) {
        std::string path = _basedir + req._path;
        if (req._path == "/") path += "index.html";
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { rsp->_statu = 404; ErrorHandler(req, rsp); return; }
        std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string ext = Util::Extension(path);
        std::string mime = _mime_msg.count(ext) ? _mime_msg[ext] : "application/octet-stream";
        rsp->SetContent(body, mime);
        rsp->SetClose(false);
    }

    bool Dispatcher(const HttpRequest& req, HttpResponse* rsp, const RouteTable& routes) {
        for (auto& [pattern, handler] : routes) {
            if (std::regex_match(req._path, pattern)) { handler(req, rsp); return true; }
        }
        return false;
    }

    void Route(HttpRequest& req, HttpResponse* rsp) {
        if (IsFileHandler(req)) { FileHandler(req, rsp); return; }
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

    // ============ ★ 限流检查 ★ ============
    bool CheckRateLimit(const PtrConnection& conn, HttpRequest& req, HttpResponse* rsp) {
        if (!_rate_limiter) return false;

        std::string clientIp = GetClientIp(conn->Fd());
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

    void OnConnected(const PtrConnection& conn) {
        conn->SetContext(HttpContext());
        DBG_LOG("NEW CONNECTION %p", conn.get());
    }

    void OnMessage(const PtrConnection& conn, Buffer* buffer) {
        while (buffer->ReadAbleSize() > 0) {
            HttpContext* context = conn->GetContext()->get<HttpContext>();
            context->RecvHttpRequest(buffer);
            HttpRequest& req = context->Request();
            HttpResponse rsp(context->RespStatu());

            if (context->RespStatu() >= 400) {
                ErrorHandler(req, &rsp);
                WriteReponse(conn, req, rsp);
                context->ReSet();
                buffer->MoveReadOffset(buffer->ReadAbleSize());
                conn->Shutdown();
                return;
            }
            if (context->RecvStatuVal() != RECV_HTTP_OVER) return;

            // ============ ★ 限流风控拦截点 ★ ============
            if (CheckRateLimit(conn, req, &rsp)) {
                WriteReponse(conn, req, rsp);
                context->ReSet();
                buffer->MoveReadOffset(buffer->ReadAbleSize());
                if (rsp.Close()) conn->Shutdown();
                return;
            }

            Route(req, &rsp);
            WriteReponse(conn, req, rsp);
            context->ReSet();
            if (rsp.Close()) conn->Shutdown();
        }
    }

public:
    HttpServer(int port, int timeout = DEFALT_TIMEOUT) : _server(port) {
        _server.EnableInactiveRelease(timeout);
        _server.SetConnectedCallback(std::bind(&HttpServer::OnConnected, this, std::placeholders::_1));
        _server.SetMessageCallback(std::bind(&HttpServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
    }
    void SetBaseDir(const std::string& path) {
        assert(Util::IsDirectory(path)); _basedir = path;
    }

    // ============ ★ 限流器注入 ★ ============
    void SetRateLimiter(RateLimitService* rl) { _rate_limiter = rl; }

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
    void SetThreadCount(int count) { _server.SetThreadCount(count); }
    void Listen() { _server.Start(); }
};

