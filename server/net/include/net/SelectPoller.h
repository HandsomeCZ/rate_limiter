#pragma once
#include "net/Poller.h"
#include <vector>
#include <map>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <sys/select.h>
#endif

class SelectPoller : public Poller {
public:
    explicit SelectPoller(EventLoop* loop);
    ~SelectPoller() override;

    void poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* ch) override;
    void removeChannel(Channel* ch) override;

private:
    struct ChannelState {
        bool reading = false;
        bool writing = false;
    };
    std::map<sockfd_t, ChannelState> channelStates_;
};
