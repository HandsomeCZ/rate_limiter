#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

class EventLoop;
class Channel;

using TaskFunc    = std::function<void()>;
using ReleaseFunc = std::function<void()>;

class TimerTask {
public:
    TimerTask(uint64_t id, uint32_t delay, const TaskFunc& cb)
        : _id(id), _timeout(delay), _task_cb(cb), _canceled(false) {}
    ~TimerTask() { if (!_canceled) _task_cb(); _release(); }
    void Cancel() { _canceled = true; }
    void SetRelease(const ReleaseFunc& cb) { _release = cb; }
    uint32_t DelayTime() const { return _timeout; }
private:
    uint64_t _id; uint32_t _timeout; bool _canceled;
    TaskFunc _task_cb; ReleaseFunc _release;
};

class TimerWheel {
public:
    using PtrTask  = std::shared_ptr<TimerTask>;
    using WeakTask = std::weak_ptr<TimerTask>;
    explicit TimerWheel(EventLoop* loop, int capacity = 60);
    ~TimerWheel();
    void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc& cb);
    void TimerRefresh(uint64_t id);
    void TimerCancel(uint64_t id);
    bool HasTimer(uint64_t id) const;
private:
    void OnTime();
    void RunTimerTask();
    int  ReadTimerfd();
    static int CreateTimerfd();
    void TimerAddInLoop(uint64_t id, uint32_t delay, const TaskFunc& cb);
    void TimerRefreshInLoop(uint64_t id);
    void TimerCancelInLoop(uint64_t id);
    int _tick, _capacity;
    std::vector<std::vector<PtrTask>> _wheel;
    std::unordered_map<uint64_t, WeakTask> _timers;
    EventLoop* _loop; int _timerfd;
    std::unique_ptr<Channel> _timer_channel;
};
