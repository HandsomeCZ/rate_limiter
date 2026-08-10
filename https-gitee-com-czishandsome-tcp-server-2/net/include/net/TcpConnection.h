#pragma once
#include "net/Callbacks.h"
#include "net/Buffer.h"
#include "net/InetAddress.h"
#include "net/Socket.h"
#include "net/Any.h"
#include <memory>
#include <string>

class Channel;
class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum State { kConnecting, kConnected, kDisconnecting, kDisconnected };

    TcpConnection(EventLoop* loop, const std::string& name,
                  sockfd_t fd, const InetAddress& localAddr, const InetAddress& peerAddr);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& peerAddr() const { return peerAddr_; }
    sockfd_t fd() const { return socket_->fd(); }

    void connectEstablished();
    void connectDestroyed();

    void send(const std::string& message);
    void send(const void* data, size_t len);
    void shutdown();

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }

    Buffer* inputBuffer() { return &inputBuffer_; }
    Buffer* outputBuffer() { return &outputBuffer_; }

private:
    void handleRead();
    void handleWrite();
    void handleClose();
    void sendInLoop(const std::string& message);

    EventLoop* loop_;
    std::string name_;
    State state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    InetAddress localAddr_;
    InetAddress peerAddr_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;
    Any context_;  // type-erased user context (HttpContext, etc.)

public:
    template<typename T> void setContext(const T& ctx) { context_ = ctx; }
    Any* getContext() { return &context_; }

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
};
