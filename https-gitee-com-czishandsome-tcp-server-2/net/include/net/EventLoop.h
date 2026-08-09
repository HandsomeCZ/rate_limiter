#pragma once
#include <memory>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

class Channel;
class Poller;

class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void quit();
    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }

    void updateChannel(Channel* ch);
    void removeChannel(Channel* ch);

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);
    void wakeup();

private:
    void handleRead();  // wakeup fd callback
    void doPendingFunctors();

    std::atomic<bool> quit_;
    std::atomic<bool> looping_;
    std::thread::id threadId_;
    std::unique_ptr<Poller> poller_;
    std::vector<Channel*> activeChannels_;

    // Wakeup mechanism
    sockfd_t wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;

    // Pending functors
    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
    std::atomic<bool> callingPendingFunctors_;
};
