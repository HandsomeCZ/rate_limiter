#include "net/EpollPoller.h"
#include "net/Channel.h"
#include <cstring>
#include <unistd.h>
#include <iostream>

EpollPoller::EpollPoller(EventLoop* loop)
    : Poller(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        perror("epoll_create1");
        abort();
    }
}

EpollPoller::~EpollPoller() {
    ::close(epollfd_);
}

void EpollPoller::updateChannel(Channel* ch) {
    int op = 0;
    if (channels_.count(ch->fd())) {
        if (ch->isNoneEvent()) {
            op = EPOLL_CTL_DEL;
            channels_.erase(ch->fd());
        } else {
            op = EPOLL_CTL_MOD;
        }
    } else {
        if (!ch->isNoneEvent()) {
            op = EPOLL_CTL_ADD;
            channels_[ch->fd()] = ch;
        }
    }
    if (op == 0) return;

    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = 0;
    if (ch->isReading()) ev.events |= EPOLLIN;
    if (ch->isWriting()) ev.events |= EPOLLOUT;
    ev.data.ptr = ch;  // Store Channel pointer, not fd!
    // This lets us retrieve the Channel directly from epoll_wait result.

    ::epoll_ctl(epollfd_, op, ch->fd(), &ev);
}

void EpollPoller::removeChannel(Channel* ch) {
    if (channels_.count(ch->fd())) {
        channels_.erase(ch->fd());
        ::epoll_ctl(epollfd_, EPOLL_CTL_DEL, ch->fd(), nullptr);
    }
}

void EpollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    // Expand event buffer if needed (avoid realloc on every call)
    if (static_cast<size_t>(channels_.size()) > events_.size()) {
        events_.resize(channels_.size() * 2);
    }

    int n = ::epoll_wait(epollfd_, events_.data(),
                         static_cast<int>(events_.size()), timeoutMs);
    if (n < 0) {
        if (errno != EINTR) perror("epoll_wait");
        return;
    }

    // O(n) where n = number of ACTIVE fds, not total fds!
    // This is the key difference from select.
    for (int i = 0; i < n; i++) {
        auto* ch = static_cast<Channel*>(events_[i].data.ptr);
        int revents = 0;
        if (events_[i].events & EPOLLIN)  revents |= Channel::kReadEvent;
        if (events_[i].events & EPOLLOUT) revents |= Channel::kWriteEvent;
        if (events_[i].events & (EPOLLERR | EPOLLHUP))
            revents |= Channel::kReadEvent;  // error treated as readable
        ch->setRevents(revents);
        activeChannels->push_back(ch);
    }
}
