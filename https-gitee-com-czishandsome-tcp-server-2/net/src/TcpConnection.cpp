#include "net/TcpConnection.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include <cassert>
#include <iostream>

TcpConnection::TcpConnection(EventLoop* loop, const std::string& name,
                             sockfd_t fd, const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop), name_(name), state_(kConnecting),
      socket_(new Socket(fd)), localAddr_(localAddr), peerAddr_(peerAddr) {
    channel_.reset(new Channel(loop, fd));
    channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
    channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
    socket_->setTcpNoDelay(true);
}

TcpConnection::~TcpConnection() {}

void TcpConnection::connectEstablished() {
    assert(state_ == kConnecting);
    state_ = kConnected;
    channel_->enableReading();
    if (connectionCallback_) connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed() {
    if (state_ == kConnected) {
        state_ = kDisconnected;
        channel_->disableAll();
        if (connectionCallback_) connectionCallback_(shared_from_this());
    }
    channel_->remove();
}

void TcpConnection::handleRead() {
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        if (messageCallback_) messageCallback_(shared_from_this(), &inputBuffer_);
    } else if (n == 0) {
        handleClose();
    }
}

void TcpConnection::handleWrite() {
    if (outputBuffer_.readableBytes() > 0) {
        int savedErrno = 0;
        ssize_t n = ::send(channel_->fd(), outputBuffer_.peek(),
                           static_cast<int>(outputBuffer_.readableBytes()), 0);
        if (n > 0) {
            outputBuffer_.retrieve(n);
        }
    }
    if (outputBuffer_.readableBytes() == 0) {
        channel_->disableWriting();
        if (writeCompleteCallback_) writeCompleteCallback_(shared_from_this());
    }
}

void TcpConnection::handleClose() {
    state_ = kDisconnected;
    channel_->disableAll();
    TcpConnectionPtr guard(shared_from_this());
    if (connectionCallback_) connectionCallback_(guard);
    if (closeCallback_) closeCallback_(guard);
}

void TcpConnection::send(const std::string& message) {
    if (state_ == kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop(message);
        } else {
            loop_->runInLoop(std::bind(&TcpConnection::sendInLoop, this, message));
        }
    }
}

void TcpConnection::send(const void* data, size_t len) {
    send(std::string(static_cast<const char*>(data), len));
}

void TcpConnection::sendInLoop(const std::string& message) {
    ssize_t nwrote = 0;
    if (!channel_->isNoneEvent() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::send(channel_->fd(), message.data(), static_cast<int>(message.size()), 0);
    }
    if (static_cast<size_t>(nwrote) < message.size()) {
        outputBuffer_.append(message.data() + nwrote, message.size() - nwrote);
        channel_->enableWriting();
    }
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        state_ = kDisconnecting;
        loop_->runInLoop([this]() {
#ifdef _WIN32
            ::shutdown(channel_->fd(), SD_SEND);
#else
            ::shutdown(channel_->fd(), SHUT_WR);
#endif
        });
    }
}
