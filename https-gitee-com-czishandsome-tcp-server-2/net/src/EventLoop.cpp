#include "net/EventLoop.h"
#include "net/Channel.h"
#include "net/EpollPoller.h"
#include "net/Socket.h"
#include <cassert>
#include <iostream>

#ifdef _WIN32
    #define close closesocket
#else
    #include <unistd.h>
    #include <sys/eventfd.h>
#endif

static sockfd_t createWakeupFd() {
#ifdef _WIN32
    // On Windows, use a loopback TCP socket pair as wakeup mechanism
    sockfd_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener != INVALID_SOCK);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listener, (sockaddr*)&addr, sizeof(addr));

    socklen_t len = sizeof(addr);
    ::getsockname(listener, (sockaddr*)&addr, &len);
    ::listen(listener, 1);

    sockfd_t connector = ::socket(AF_INET, SOCK_STREAM, 0);
    ::connect(connector, (sockaddr*)&addr, sizeof(addr));
    sockfd_t acceptor = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);
    return connector;  // we write to connector to wake up
#else
    return ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
}

EventLoop::EventLoop()
    : quit_(false), looping_(false),
      poller_(new EpollPoller(this)),
      callingPendingFunctors_(false) {
    wakeupFd_ = createWakeupFd();
    wakeupChannel_.reset(new Channel(this, wakeupFd_));
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    Socket::close(wakeupFd_);
}

void EventLoop::loop() {
    assert(!looping_);
    looping_ = true;
    quit_ = false;
    threadId_ = std::this_thread::get_id();

    while (!quit_) {
        activeChannels_.clear();
        poller_->poll(100, &activeChannels_);
        for (auto* ch : activeChannels_) ch->handleEvent();
        doPendingFunctors();
    }
    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) wakeup();
}

void EventLoop::updateChannel(Channel* ch) {
    assert(isInLoopThread());
    poller_->updateChannel(ch);
}

void EventLoop::removeChannel(Channel* ch) {
    assert(isInLoopThread());
    poller_->removeChannel(ch);
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) { cb(); }
    else { queueInLoop(std::move(cb)); }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    if (!isInLoopThread() || callingPendingFunctors_) wakeup();
}

void EventLoop::wakeup() {
    uint64_t one = 1;
#ifdef _WIN32
    ::send(wakeupFd_, (const char*)&one, sizeof(one), 0);
#else
    ::write(wakeupFd_, &one, sizeof(one));
#endif
}

void EventLoop::handleRead() {
    uint64_t buf;
#ifdef _WIN32
    ::recv(wakeupFd_, (char*)&buf, sizeof(buf), 0);
#else
    ::read(wakeupFd_, &buf, sizeof(buf));
#endif
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (auto& f : functors) f();
    callingPendingFunctors_ = false;
}
