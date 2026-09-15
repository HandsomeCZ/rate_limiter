#include "net/TcpServer.h"
#include "net/LoopThreadPool.h"
#include "net/TcpConnection.h"
#include "net/EventLoop.h"
#include "net/TimerWheel.h"
#include <iostream>

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& name)
    : loop_(loop), name_(name), nextConnId_(1) {
    acceptor_.reset(new Acceptor(loop, listenAddr));
    acceptor_->setNewConnectionCallback(
        std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer() {}

void TcpServer::start() { acceptor_->listen(); }

void TcpServer::newConnection(sockfd_t fd, const InetAddress& peerAddr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s#%d", name_.c_str(), nextConnId_.fetch_add(1));
    std::string connName = buf;

    // Round-robin dispatch to worker EventLoop
    EventLoop* ioLoop = threadPool_ ? threadPool_->getNextLoop() : loop_;

    InetAddress localAddr(0); // placeholder
    TcpConnectionPtr conn = std::make_shared<TcpConnection>(ioLoop, connName, fd, localAddr, peerAddr);
    connections_[connName] = conn;
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));
    ioLoop->runInLoop([this, conn]() {
        conn->connectEstablished();
        // 连接建立后注册超时定时器
        enableConnectionTimeout(conn);
    });
}

void TcpServer::enableConnectionTimeout(const TcpConnectionPtr& conn) {
    if (!timerWheel_ || connTimeoutSec_ == 0) return;

    // addTimer 返回唯一 ID（原子计数器分配，替代原 std::hash<string> 的碰撞风险）
    std::weak_ptr<TcpConnection> weakConn = conn;
    uint64_t timerId = timerWheel_->addTimer(connTimeoutSec_, [weakConn]() {
        if (auto c = weakConn.lock()) {
            // 确保在正确的 EventLoop 线程关闭
            c->getLoop()->runInLoop([c]() {
                std::cout << "[TimerWheel] Connection " << c->name()
                          << " idle timeout, closing..." << std::endl;
                c->shutdown();
            });
        }
    });
    conn->setTimerId(timerId);
}

void TcpServer::refreshConnectionTimeout(const TcpConnectionPtr& conn) {
    if (!timerWheel_ || conn->timerId() == 0) return;
    timerWheel_->refreshTimer(conn->timerId());
}

void TcpServer::cancelConnectionTimeout(const TcpConnectionPtr& conn) {
    if (!timerWheel_ || conn->timerId() == 0) return;
    timerWheel_->cancelTimer(conn->timerId());
    conn->setTimerId(0);
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // 取消超时定时器
    cancelConnectionTimeout(conn);
    // Remove from connections_ map on base loop (thread-safe centralized map)
    loop_->runInLoop([this, conn]() {
        connections_.erase(conn->name());
    });
    // Destroy the connection on its own EventLoop
    conn->getLoop()->runInLoop([conn]() {
        conn->connectDestroyed();
    });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    connections_.erase(conn->name());
    conn->getLoop()->runInLoop([conn]() {
        conn->connectDestroyed();
    });
}