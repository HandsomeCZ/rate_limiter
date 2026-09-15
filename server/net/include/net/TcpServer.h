#pragma once
#include "net/Socket.h"
#include "net/Callbacks.h"
#include "net/InetAddress.h"
#include "net/Acceptor.h"
#include "net/Any.h"

#include <map>
#include <string>
#include <atomic>
#include <functional>

class EventLoop;
class LoopThreadPool;
class TimerWheel;

class TcpServer {
public:
    TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& name);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start();
    void setThreadPool(LoopThreadPool* pool) { threadPool_ = pool; }
    void setThreadInitCallback(const std::function<void(EventLoop*)>& cb) { threadInitCallback_ = cb; }

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

    EventLoop* getLoop() const { return loop_; }

    // ============ 时间轮超时管理 ============
    void setTimerWheel(TimerWheel* wheel) { timerWheel_ = wheel; }
    TimerWheel* getTimerWheel() const { return timerWheel_; }
    void setConnectionTimeout(uint32_t sec) { connTimeoutSec_ = sec; }
    uint32_t connectionTimeout() const { return connTimeoutSec_; }

    // 注册连接超时定时器
    void enableConnectionTimeout(const TcpConnectionPtr& conn);
    // 刷新连接超时（收到消息时调用）
    void refreshConnectionTimeout(const TcpConnectionPtr& conn);
    // 取消连接超时
    void cancelConnectionTimeout(const TcpConnectionPtr& conn);

private:
    void newConnection(sockfd_t fd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    std::atomic<int> nextConnId_;
    std::map<std::string, TcpConnectionPtr> connections_;
    LoopThreadPool* threadPool_ = nullptr;
    std::function<void(EventLoop*)> threadInitCallback_;

    TimerWheel* timerWheel_ = nullptr;
    uint32_t connTimeoutSec_ = 60;  // 默认 60 秒空闲超时
};