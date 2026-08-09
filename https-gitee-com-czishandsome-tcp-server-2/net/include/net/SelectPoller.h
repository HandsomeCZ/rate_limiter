#pragma once
#include "net/Poller.h"
#include <set>

class SelectPoller : public Poller {
public:
    explicit SelectPoller(EventLoop* loop);
    ~SelectPoller() override = default;

    void poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* ch) override;
    void removeChannel(Channel* ch) override;

private:
    void fillFdSet(fd_set* set) const;
    std::set<sockfd_t> fdSet_;
    sockfd_t maxFd_;
};
