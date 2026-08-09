#pragma once
#include <vector>
#include <memory>
#include <map>

class Channel;
class EventLoop;

class Poller {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop) : ownerLoop_(loop) {}
    virtual ~Poller() = default;

    virtual void poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* ch) = 0;
    virtual void removeChannel(Channel* ch) = 0;

    EventLoop* ownerLoop() const { return ownerLoop_; }

protected:
    EventLoop* ownerLoop_;
    std::map<sockfd_t, Channel*> channels_;
};
