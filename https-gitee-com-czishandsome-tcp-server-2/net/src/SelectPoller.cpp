#include "net/SelectPoller.h"
#include "net/Channel.h"
#include <cstring>
#include <algorithm>

SelectPoller::SelectPoller(EventLoop* loop) : Poller(loop), maxFd_(0) {}

void SelectPoller::updateChannel(Channel* ch) {
    if (ch->isNoneEvent()) {
        if (channels_.count(ch->fd())) {
            fdSet_.erase(ch->fd());
            channels_.erase(ch->fd());
        }
    } else {
        fdSet_.insert(ch->fd());
        channels_[ch->fd()] = ch;
    }
    maxFd_ = fdSet_.empty() ? 0 : *fdSet_.rbegin();
}

void SelectPoller::removeChannel(Channel* ch) {
    fdSet_.erase(ch->fd());
    channels_.erase(ch->fd());
    maxFd_ = fdSet_.empty() ? 0 : *fdSet_.rbegin();
}

void SelectPoller::fillFdSet(fd_set* set) const {
    FD_ZERO(set);
    for (auto fd : fdSet_) FD_SET(fd, set);
}

void SelectPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    fd_set readfds, writefds;
    fillFdSet(&readfds);
    fillFdSet(&writefds);

    timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int n = select(static_cast<int>(maxFd_ + 1), &readfds, &writefds, nullptr,
                   timeoutMs >= 0 ? &tv : nullptr);

    if (n <= 0) return;

    for (auto& [fd, ch] : channels_) {
        int revents = 0;
        if (FD_ISSET(fd, &readfds))  revents |= Channel::kReadEvent;
        if (FD_ISSET(fd, &writefds)) revents |= Channel::kWriteEvent;
        if (revents) {
            ch->setRevents(revents);
            activeChannels->push_back(ch);
        }
    }
}
