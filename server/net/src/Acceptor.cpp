#include "net/Acceptor.h"
#include <iostream>
#include <cerrno>
#include "net/EventLoop.h"

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr)
    : loop_(loop),
      acceptSocket_(Socket::createNonBlocking()),
      acceptChannel_(loop, acceptSocket_.fd()) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.bind(listenAddr);
    acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}

void Acceptor::listen() {
    acceptSocket_.listen();
    acceptChannel_.enableReading();
}

void Acceptor::handleRead() {
    // 循环 accept，直到没有待接受连接（EAGAIN/EWOULDBLOCK）。
    // 原实现每个可读事件只 accept 一个连接，高并发建连（如突发连接风暴）时会成为瓶颈。
    while (true) {
        InetAddress peerAddr;
        sockfd_t connfd = acceptSocket_.accept(&peerAddr);
        if (connfd == INVALID_SOCK) {
#ifdef _WIN32
            int err = WSAGetLastError();
            const bool noMore = (err == WSAEWOULDBLOCK);
            const bool retry  = (err == WSAECONNABORTED || err == WSAEINTR);
#else
            int err = errno;
            const bool noMore = (err == EAGAIN || err == EWOULDBLOCK);
            const bool retry  = (err == ECONNABORTED || err == EPROTO || err == EINTR);
#endif
            if (noMore) break;     // 已无待接受连接
            if (retry)  continue;  // 该连接在 accept 前已断开，试下一个
            // 其它错误（如 EMFILE 文件描述符耗尽）：跳出，避免忙轮询
            std::cerr << "[Acceptor] accept error, errno=" << err << std::endl;
            break;
        }
        if (newConnCallback_) newConnCallback_(connfd, peerAddr);
    }
}
