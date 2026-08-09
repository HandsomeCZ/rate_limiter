#include "net/Acceptor.h"
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
    InetAddress peerAddr;
    sockfd_t connfd = acceptSocket_.accept(&peerAddr);
    if (connfd != INVALID_SOCK) {
        if (newConnCallback_) newConnCallback_(connfd, peerAddr);
    }
}
