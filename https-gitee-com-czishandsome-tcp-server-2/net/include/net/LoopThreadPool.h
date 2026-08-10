#pragma once
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class EventLoop;

class LoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    explicit LoopThread(const ThreadInitCallback& cb = ThreadInitCallback());
    ~LoopThread();
    EventLoop* startLoop();
private:
    void threadFunc();
    EventLoop* _loop;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cond;
    ThreadInitCallback _initCallback;
};

class LoopThreadPool {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    explicit LoopThreadPool(EventLoop* baseLoop);
    ~LoopThreadPool();
    void setThreadCount(int n) { _threadCount = n; }
    void start(const ThreadInitCallback& cb = ThreadInitCallback());
    EventLoop* getNextLoop();
    std::vector<EventLoop*> allLoops() const { return _loops; }
    bool started() const { return _started; }
private:
    EventLoop* _baseLoop;
    int _threadCount, _nextIdx;
    bool _started;
    std::vector<std::unique_ptr<LoopThread>> _threads;
    std::vector<EventLoop*> _loops;
};
