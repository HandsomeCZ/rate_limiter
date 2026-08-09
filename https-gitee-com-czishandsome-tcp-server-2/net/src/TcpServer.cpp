#include "net/TcpServer.h"
#include "net/TcpConnection.h"
#include "net/EventLoop.h"

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

    InetAddress localAddr(0); // placeholder
    TcpConnectionPtr conn = std::make_shared<TcpConnection>(loop_, connName, fd, localAddr, peerAddr);
    connections_[connName] = conn;
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));
    conn->connectEstablished();
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    connections_.erase(conn->name());
    conn->connectDestroyed();
}
