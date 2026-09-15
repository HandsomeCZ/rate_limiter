#include "net/EventLoop.h"
#include "net/Channel.h"
#ifdef _WIN32
#include "net/SelectPoller.h"
#else
#include "net/EpollPoller.h"
#endif
#include "net/Socket.h"
#include <cassert>
#include <iostream>

#ifdef _WIN32
    #include <cstring>
    #include <ws2tcpip.h>
    using socklen_t = int;

    static void initWinsock() {
        static bool initialized = false;
        if (!initialized) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                std::cerr << "WSAStartup failed!" << std::endl;
                abort();
            }
            initialized = true;
        }
    }
#else
    #include <unistd.h>
    #include <sys/eventfd.h>
#endif

static std::pair<sockfd_t, sockfd_t> createWakeupPair() {
#ifdef _WIN32
    initWinsock();
    // Create a self-connected TCP socket pair: listener -> accept
    // readEnd = accepted socket (becomes readable when data is sent to writeEnd)
    // writeEnd = connecting socket (used to send wakeup byte)
    sockfd_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener != INVALID_SOCK);

    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listener, (sockaddr*)&addr, sizeof(addr));

    socklen_t len = sizeof(addr);
    ::getsockname(listener, (sockaddr*)&addr, &len);
    ::listen(listener, 1);

    sockfd_t writeEnd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(writeEnd != INVALID_SOCK);
    ::connect(writeEnd, (sockaddr*)&addr, sizeof(addr));

    sockfd_t readEnd = ::accept(listener, nullptr, nullptr);
    assert(readEnd != INVALID_SOCK);
    ::closesocket(listener);

    // Set readEnd to non-blocking for use with poll/select
    u_long mode = 1;
    ::ioctlsocket(readEnd, FIONBIO, &mode);

    return {readEnd, writeEnd};
#else
    sockfd_t efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return {efd, efd};  // writeEnd same as readEnd on Linux (eventfd is bidirectional)
#endif
}

EventLoop::EventLoop()
    : quit_(false), looping_(false),
      threadId_(std::this_thread::get_id()),
      poller_(new
#ifdef _WIN32
    SelectPoller
#else
    EpollPoller
#endif
    (this)),
      callingPendingFunctors_(false) {
    auto [readEnd, writeEnd] = createWakeupPair();
    wakeupFd_ = readEnd;
    wakeupWriteFd_ = writeEnd;
    wakeupChannel_.reset(new Channel(this, wakeupFd_));
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    Socket::close(wakeupFd_);
#ifdef _WIN32
    Socket::close(wakeupWriteFd_);
#endif
}

void EventLoop::loop() {
    assert(!looping_);
    looping_ = true;
    quit_ = false;
    // 注意：不再重写 threadId_。构造函数已在正确的线程里初始化 threadId_，
    // loop() 与构造函数运行在同一线程，重写既冗余又与其他线程的 isInLoopThread() 读构成数据竞争。

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
    ::send(wakeupWriteFd_, (const char*)&one, sizeof(one), 0);
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
