#include "net/TimerWheel.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

TimerWheel::TimerWheel(EventLoop* loop, int capacity)
    : _tick(0), _capacity(capacity), _loop(loop) {
    _wheel.resize(capacity);
    _timerfd = CreateTimerfd();
    _timer_channel = std::make_unique<Channel>(loop, _timerfd);
    _timer_channel->setReadCallback([this]() { OnTime(); });
    _timer_channel->enableReading();
}

TimerWheel::~TimerWheel() {
    _timer_channel->disableAll();
    ::close(_timerfd);
}

int TimerWheel::CreateTimerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) { perror("timerfd_create"); abort(); }
    struct itimerspec itime;
    std::memset(&itime, 0, sizeof(itime));
    itime.it_value.tv_sec = 1;
    itime.it_interval.tv_sec = 1;
    ::timerfd_settime(timerfd, 0, &itime, nullptr);
    return timerfd;
}

int TimerWheel::ReadTimerfd() {
    uint64_t times = 0;
    ::read(_timerfd, &times, sizeof(times));
    return static_cast<int>(times);
}

void TimerWheel::OnTime() {
    int times = ReadTimerfd();
    for (int i = 0; i < times; i++) RunTimerTask();
}

void TimerWheel::RunTimerTask() {
    _tick = (_tick + 1) % _capacity;
    _wheel[_tick].clear();
}

void TimerWheel::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc& cb) {
    _loop->runInLoop([this, id, delay, cb]() { TimerAddInLoop(id, delay, cb); });
}

void TimerWheel::TimerRefresh(uint64_t id) {
    _loop->runInLoop([this, id]() { TimerRefreshInLoop(id); });
}

void TimerWheel::TimerCancel(uint64_t id) {
    _loop->runInLoop([this, id]() { TimerCancelInLoop(id); });
}

bool TimerWheel::HasTimer(uint64_t id) const {
    return _timers.find(id) != _timers.end();
}

void TimerWheel::TimerAddInLoop(uint64_t id, uint32_t delay, const TaskFunc& cb) {
    auto pt = std::make_shared<TimerTask>(id, delay, cb);
    pt->SetRelease([this, id]() { _timers.erase(id); });
    int pos = (_tick + delay) % _capacity;
    _wheel[pos].push_back(pt);
    _timers[id] = WeakTask(pt);
}

void TimerWheel::TimerRefreshInLoop(uint64_t id) {
    auto it = _timers.find(id);
    if (it == _timers.end()) return;
    auto pt = it->second.lock();
    if (!pt) return;
    int pos = (_tick + pt->DelayTime()) % _capacity;
    _wheel[pos].push_back(pt);
}

void TimerWheel::TimerCancelInLoop(uint64_t id) {
    auto it = _timers.find(id);
    if (it == _timers.end()) return;
    auto pt = it->second.lock();
    if (pt) pt->Cancel();
    _timers.erase(it);
}
