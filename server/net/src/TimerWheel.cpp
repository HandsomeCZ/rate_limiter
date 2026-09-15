#include "net/TimerWheel.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Socket.h"
#include <cstring>
#include <iostream>
#include <thread>
#include <algorithm>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    static void initWinsock() {
        static bool initialized = false;
        if (!initialized) {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
            initialized = true;
        }
    }
#else
    #include <sys/timerfd.h>
    #include <unistd.h>
#endif

TimerWheel::TimerWheel(EventLoop* loop, int capacity)
    : _loop(loop), _capacity(capacity) {
    _wheel.resize(capacity);

#ifdef _WIN32
    initWinsock();
    // Windows: use a loopback TCP socket pair for timer notifications
    // readEnd (_timerfd) = accepted socket (polled for readability)
    // writeEnd (_writeFd) = connecting socket (timer thread sends byte here)
    sockfd_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCK) {
        std::cerr << "TimerWheel: socket failed" << std::endl;
        abort();
    }
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listener, (sockaddr*)&addr, sizeof(addr));

    socklen_t len = sizeof(addr);
    ::getsockname(listener, (sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    ::listen(listener, 1);

    _writeFd = ::socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_port = htons(port);
    ::connect(_writeFd, (sockaddr*)&addr, sizeof(addr));
    _timerfd = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);

    // Start timer thread
    _timerThread = std::thread([this]() {
        while (_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!_running.load()) break;
            char c = 1;
            ::send(_writeFd, &c, 1, 0);
        }
    });
#else
    _timerfd = CreateTimerfd();
#endif

    _timer_channel = std::make_unique<Channel>(loop, _timerfd);
    _timer_channel->setReadCallback([this]() { OnTime(); });
    _timer_channel->enableReading();
}

TimerWheel::~TimerWheel() {
    _running = false;
#ifdef _WIN32
    if (_timerThread.joinable()) {
        _timerThread.join();
    }
#endif

    // 回调已改为显式触发（不再在 TimerTask 析构中执行），
    // 因此这里直接清空即可：无 use-after-free，也不会在析构期间误触发超时回调。
    _wheel.clear();
    _timers.clear();

    _timer_channel->disableAll();
#ifdef _WIN32
    ::closesocket(_timerfd);
    ::closesocket(_writeFd);
#else
    ::close(_timerfd);
#endif
}

#ifndef _WIN32
sockfd_t TimerWheel::CreateTimerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) { perror("timerfd_create"); abort(); }
    struct itimerspec itime;
    std::memset(&itime, 0, sizeof(itime));
    itime.it_value.tv_sec = 1;
    itime.it_interval.tv_sec = 1;
    ::timerfd_settime(timerfd, 0, &itime, nullptr);
    return timerfd;
}
#endif

sockfd_t TimerWheel::ReadTimerfd() {
#ifdef _WIN32
    char buf[64];
    int n = ::recv(_timerfd, buf, sizeof(buf), 0);
    return n > 0 ? static_cast<sockfd_t>(n) : static_cast<sockfd_t>(0);
#else
    uint64_t times = 0;
    ::read(_timerfd, &times, sizeof(times));
    return static_cast<sockfd_t>(times);
#endif
}

void TimerWheel::OnTime() {
    uint64_t times = ReadTimerfd();
    for (uint64_t i = 0; i < times; ++i) {
        ++_tick;  // 绝对 tick 单调递增
        processSlot(static_cast<int>(_tick % _capacity));
    }
}

void TimerWheel::processSlot(int slot) {
    auto& tasks = _wheel[slot];
    for (auto& task : tasks) {
        if (task->canceled) continue;
        // 用绝对 deadline 判断，只有真正到期才触发。
        // 支持 delay >= capacity 的多轮场景：未到期的任务保留在槽里，下一轮再检查。
        if (task->deadline <= _tick) {
            task->canceled = true;
            _timers.erase(task->id);
            task->cb();  // 显式触发回调
        }
    }
    // 惰性清理已触发/已取消的任务，只保留未到期的
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
        [](const PtrTask& t) { return t->canceled; }), tasks.end());
}

TimerWheel::TimerId TimerWheel::addTimer(uint32_t delaySec, TaskFunc cb) {
    TimerId id = _nextId.fetch_add(1, std::memory_order_relaxed);
    _loop->runInLoop([this, id, delaySec, cb = std::move(cb)]() {
        addTimerInLoop(id, delaySec, cb);
    });
    return id;
}

void TimerWheel::refreshTimer(TimerId id) {
    _loop->runInLoop([this, id]() { refreshTimerInLoop(id); });
}

void TimerWheel::cancelTimer(TimerId id) {
    _loop->runInLoop([this, id]() { cancelTimerInLoop(id); });
}

bool TimerWheel::hasTimer(TimerId id) const {
    return _timers.find(id) != _timers.end();
}

void TimerWheel::addTimerInLoop(TimerId id, uint32_t delaySec, const TaskFunc& cb) {
    auto task = std::make_shared<TimerTask>();
    task->id = id;
    task->delaySec = delaySec;
    task->deadline = _tick + delaySec;
    task->cb = cb;
    task->canceled = false;
    _wheel[task->deadline % _capacity].push_back(task);
    _timers[id] = task;
}

void TimerWheel::refreshTimerInLoop(TimerId id) {
    auto it = _timers.find(id);
    if (it == _timers.end()) return;
    auto old = it->second.lock();
    if (!old || old->canceled) return;

    // 作废旧任务（惰性删除，processSlot 时清理），用原 delaySec 重新计时
    old->canceled = true;
    auto task = std::make_shared<TimerTask>();
    task->id = id;
    task->delaySec = old->delaySec;
    task->deadline = _tick + old->delaySec;
    task->cb = old->cb;
    task->canceled = false;
    _wheel[task->deadline % _capacity].push_back(task);
    _timers[id] = task;
}

void TimerWheel::cancelTimerInLoop(TimerId id) {
    auto it = _timers.find(id);
    if (it == _timers.end()) return;
    auto task = it->second.lock();
    if (task) task->canceled = true;  // 惰性删除
    _timers.erase(it);
}
