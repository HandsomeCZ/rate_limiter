#pragma once
#include "net/Socket.h"
#include "net/InetAddress.h"
#include "net/Channel.h"
#include <functional>

class EventLoop;

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(sockfd_t, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr);

    void listen();
    void setNewConnectionCallback(const NewConnectionCallback& cb) { newConnCallback_ = cb; }

private:
    void handleRead();

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnCallback_;
};
