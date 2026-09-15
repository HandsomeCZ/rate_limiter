#include "net/SelectPoller.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "net/Channel.h"
#include "net/EventLoop.h"
#include <cstring>

#ifdef _WIN32
#else
    #include <sys/select.h>
    #include <unistd.h>
#endif

SelectPoller::SelectPoller(EventLoop* loop)
    : Poller(loop) {
}

SelectPoller::~SelectPoller() {
}
//这个函数只是把fd放到poller和selectpoller记录的表中，还没有设置到poll中监听
void SelectPoller::updateChannel(Channel* ch) {
    sockfd_t fd = ch->fd();
    if (ch->isNoneEvent()) {
        channelStates_.erase(fd);
        channels_.erase(fd);
    } else {
        ChannelState state;
        state.reading = ch->isReading();
        state.writing = ch->isWriting();
        channelStates_[fd] = state;
        channels_[fd] = ch;
    }
}

void SelectPoller::removeChannel(Channel* ch) {
    sockfd_t fd = ch->fd();
    channelStates_.erase(fd);
    channels_.erase(fd);
}

void SelectPoller::poll(int timeoutMs, ChannelList* activeChannels) {
#ifdef _WIN32
    std::vector<WSAPOLLFD> fds;
    fds.reserve(channelStates_.size());
    for (const auto& [fd, state] : channelStates_) {
        WSAPOLLFD pfd;
        pfd.fd = fd;
        pfd.events = 0;
        pfd.revents = 0;
        if (state.reading)  pfd.events |= POLLIN;
        if (state.writing)  pfd.events |= POLLOUT;
        fds.push_back(pfd);
    }

    int n = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);

    if (n <= 0) return;

    for (size_t i = 0; i < fds.size() && n > 0; ++i) {
        if (fds[i].revents == 0) continue;
        sockfd_t fd = fds[i].fd;
        auto it = channels_.find(fd);
        if (it != channels_.end()) {
            int revents = 0;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) revents |= Channel::kReadEvent;
            if (fds[i].revents & POLLOUT) revents |= Channel::kWriteEvent;
            if (revents) {
                it->second->setRevents(revents);
                activeChannels->push_back(it->second);
                --n;
            }
        }
    }
#else
    fd_set readSet, writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);

    sockfd_t maxfd = INVALID_SOCK;

    for (const auto& [fd, state] : channelStates_) {
        if (state.reading) { FD_SET(fd, &readSet); }
        if (state.writing) { FD_SET(fd, &writeSet); }
        if (fd > maxfd) maxfd = fd;
    }

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int n = ::select(
        static_cast<int>(maxfd + 1),
        &readSet, &writeSet, nullptr,
        (timeoutMs >= 0) ? &tv : nullptr
    );

    if (n <= 0) return;

    for (const auto& [fd, ch] : channels_) {
        int revents = 0;
        if (FD_ISSET(fd, &readSet))  revents |= Channel::kReadEvent;
        if (FD_ISSET(fd, &writeSet)) revents |= Channel::kWriteEvent;
        if (revents) {
            ch->setRevents(revents);
            activeChannels->push_back(ch);
        }
    }
#endif
}
