#pragma once
#include <functional>
#include <memory>

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, sockfd_t fd);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    sockfd_t fd() const { return fd_; }
    int events() const { return events_; }
    int revents() const { return revents_; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    bool isReading() const { return events_ & kReadEvent; }
    bool isWriting() const { return events_ & kWriteEvent; }

    void setRevents(int revt) { revents_ = revt; }
    void setReadCallback(EventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    void enableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();
    void remove();

    void handleEvent();

    EventLoop* ownerLoop() const { return loop_; }

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

private:
    void update();

    EventLoop* loop_;
    sockfd_t fd_;
    int events_;
    int revents_;
    bool addedToLoop_;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
