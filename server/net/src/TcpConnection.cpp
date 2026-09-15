#include "net/TcpConnection.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/TimerWheel.h"
#include <cassert>
#include <iostream>
#include <fstream>

TcpConnection::TcpConnection(EventLoop* loop, const std::string& name,
                             sockfd_t fd, const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop), name_(name), state_(kConnecting),
      socket_(new Socket(fd)), localAddr_(localAddr), peerAddr_(peerAddr) {
    socket_->setNonBlocking();
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
    } else {
        // 真正的读错误（连接被重置等）应关闭连接；EAGAIN/EWOULDBLOCK 只是暂时无数据
#ifdef _WIN32
        if (savedErrno != WSAEWOULDBLOCK && savedErrno != WSAEINTR)
#else
        if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK && savedErrno != EINTR)
#endif
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
        } else if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
            {
                handleClose();
                return;
            }
        }
    }
    if (outputBuffer_.readableBytes() == 0) {
        channel_->disableWriting();
        if (writeCompleteCallback_) writeCompleteCallback_(shared_from_this());
        // 半关闭：等缓冲清空后再关写端，避免丢弃未发送数据
        if (state_ == kDisconnecting) shutdownInLoop();
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
            // 跨线程投递：用 weak_ptr 防止连接在 functor 执行前被销毁（原实现裸 this → 悬垂）
            std::weak_ptr<TcpConnection> weakThis = shared_from_this();
            loop_->runInLoop([weakThis, message]() {
                if (auto conn = weakThis.lock()) conn->sendInLoop(message);
            });
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
        if (nwrote < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
            {
                // 真正的发送错误（EPIPE/ECONNRESET）→ 连接已损坏，直接关闭
                handleClose();
                return;
            }
            nwrote = 0;  // 缓冲区满，稍后由 EPOLLOUT 触发 handleWrite
        }
    }
    if (static_cast<size_t>(nwrote) < message.size()) {
        outputBuffer_.append(message.data() + nwrote, message.size() - nwrote);
        channel_->enableWriting();
    }
}

void TcpConnection::shutdown() {
    std::weak_ptr<TcpConnection> weakThis = shared_from_this();
    loop_->runInLoop([weakThis]() {
        if (auto conn = weakThis.lock()) {
            if (conn->state_ == kConnected) {
                conn->state_ = kDisconnecting;
                conn->shutdownInLoop();
            }
        }
    });
}

void TcpConnection::shutdownInLoop() {
    // 若输出缓冲还有未发送数据，等 handleWrite 清空后再关写端，避免丢数据
    if (!channel_->isWriting()) {
#ifdef _WIN32
        ::shutdown(channel_->fd(), SD_SEND);
#else
        ::shutdown(channel_->fd(), SHUT_WR);
#endif
    }
}

void TcpConnection::refreshTimeout() {
    // 由外部（TcpServer/HttpServer）保证在正确的 EventLoop 线程调用
    if (timerId_ != 0) {
        // 这里需要 TcpServer 提供 TimerWheel 访问，实际刷新在 TcpServer 层做
        // 留空，由 TcpServer 在收到消息时统一处理
    }
}

void TcpConnection::cancelTimeout() {
    if (timerId_ != 0) {
        // 同上，由 TcpServer 层取消
        timerId_ = 0;
    }
}