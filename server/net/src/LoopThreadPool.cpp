#include "net/LoopThreadPool.h"
#include "net/EventLoop.h"
#include <cassert>

LoopThread::LoopThread(const ThreadInitCallback& cb)
    : _loop(nullptr), _initCallback(cb) {}

LoopThread::~LoopThread() {
    if (_loop) _loop->quit();
    if (_thread.joinable()) _thread.join();
}

EventLoop* LoopThread::startLoop() {
    _thread = std::thread(&LoopThread::threadFunc, this);
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cond.wait(lock, [this]() { return _loop != nullptr; });
    }
    return _loop;
}

void LoopThread::threadFunc() {
    EventLoop loop;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _loop = &loop;
    }
    _cond.notify_one();
    if (_initCallback) _initCallback(&loop);
    loop.loop();
}

LoopThreadPool::LoopThreadPool(EventLoop* baseLoop)
    : _baseLoop(baseLoop), _threadCount(0), _nextIdx(0), _started(false) {}

LoopThreadPool::~LoopThreadPool() {}

void LoopThreadPool::start(const ThreadInitCallback& cb) {
    assert(!_started);
    _started = true;
    _threads.resize(_threadCount);
    _loops.resize(_threadCount);
    for (int i = 0; i < _threadCount; i++) {
        _threads[i] = std::make_unique<LoopThread>(cb);
        _loops[i] = _threads[i]->startLoop();
    }
}

EventLoop* LoopThreadPool::getNextLoop() {
    if (_loops.empty()) return _baseLoop;
    EventLoop* loop = _loops[_nextIdx];
    _nextIdx = (_nextIdx + 1) % static_cast<int>(_loops.size());
    return loop;
}
