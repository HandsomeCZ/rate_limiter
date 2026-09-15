#pragma once
#include "net/Poller.h"
#include <vector>
#include <sys/epoll.h>

// EpollPoller -- Linux epoll-based I/O multiplexing
// Supports unlimited fds, O(1) active fd retrieval.

class EpollPoller : public Poller {
public:
    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller() override;

    void poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* ch) override;
    void removeChannel(Channel* ch) override;

private:
    static const int kInitEventListSize = 16;
    int epollfd_;
    std::vector<epoll_event> events_;
};
