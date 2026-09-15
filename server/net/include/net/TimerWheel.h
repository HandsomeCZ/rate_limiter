#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <atomic>
#include "net/Socket.h"
#ifdef _WIN32
#include <thread>
#endif

class EventLoop;
class Channel;

using TaskFunc = std::function<void()>;

// ---------------------------------------------------------------------------
// TimerTask — 时间轮任务
//
// 回调由时间轮在 OnTime 里【显式触发】，不再放在析构函数里触发。
// 原实现把回调放在 ~TimerTask() 里，带来三个问题：
//   1. 回调触发时机隐晦（依赖 shared_ptr 引用计数归零）
//   2. 析构时成员逆序销毁导致 _release() 访问已析构的 _timers（use-after-free）
//   3. 刷新靠「旧槽不清除、引用计数兜底」，脆弱
// 显式触发后，TimerTask 变成纯数据结构，析构无副作用。
// ---------------------------------------------------------------------------
struct TimerTask {
    uint64_t id;        // 唯一 ID（原子计数器分配，避免 std::hash<string> 碰撞）
    uint32_t delaySec;  // 超时秒数（refresh 重新计时用）
    uint64_t deadline;  // 绝对到期 tick（秒）
    TaskFunc cb;
    bool canceled = false;
};

class TimerWheel {
public:
    using TimerId = uint64_t;
    using PtrTask  = std::shared_ptr<TimerTask>;
    using WeakTask = std::weak_ptr<TimerTask>;

    explicit TimerWheel(EventLoop* loop, int capacity = 60);
    ~TimerWheel();

    // 注册定时器，返回唯一 ID；delaySec 秒后在 loop 线程触发 cb
    TimerId addTimer(uint32_t delaySec, TaskFunc cb);
    // 重新计时：从当前时刻再等 delaySec
    void refreshTimer(TimerId id);
    void cancelTimer(TimerId id);
    bool hasTimer(TimerId id) const;

private:
    void OnTime();
    void processSlot(int slot);
    sockfd_t ReadTimerfd();
    static sockfd_t CreateTimerfd();
    void addTimerInLoop(TimerId id, uint32_t delaySec, const TaskFunc& cb);
    void refreshTimerInLoop(TimerId id);
    void cancelTimerInLoop(TimerId id);

    EventLoop* _loop;
    int _capacity;
    // 绝对 tick（单调递增，不取模）。原实现用 (_tick+delay)%capacity 计算槽位，
    // delay >= capacity 时会环绕提前触发；改绝对 deadline 后支持任意 delay（多轮）。
    uint64_t _tick = 0;
    std::vector<std::vector<PtrTask>> _wheel;
    std::unordered_map<TimerId, WeakTask> _timers;
    std::atomic<TimerId> _nextId{1};  // 定时器 ID 原子分配器
    sockfd_t _timerfd;
    std::unique_ptr<Channel> _timer_channel;
    std::atomic<bool> _running{true};  // 跨平台：构造/析构在 Linux 下也引用
#ifdef _WIN32
    std::thread _timerThread;
    sockfd_t _writeFd;
#endif
};
