#include "net/Channel.h"
#include "net/EventLoop.h"
#include <cassert>

const int Channel::kNoneEvent  = 0;
const int Channel::kReadEvent  = 1;
const int Channel::kWriteEvent = 2;

Channel::Channel(EventLoop* loop, sockfd_t fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0), addedToLoop_(false) {}

Channel::~Channel() {
    if (addedToLoop_) remove();
}

void Channel::enableReading() {
    events_ |= kReadEvent;
    update();
}

void Channel::enableWriting() {
    events_ |= kWriteEvent;
    update();
}

void Channel::disableWriting() {
    events_ &= ~kWriteEvent;
    update();
}

void Channel::disableAll() {
    events_ = kNoneEvent;
    update();
}

void Channel::remove() {
    if (addedToLoop_) {
        loop_->removeChannel(this);
        addedToLoop_ = false;
    }
}

void Channel::update() { loop_->updateChannel(this); }

void Channel::handleEvent() {
    if ((revents_ & kReadEvent) && readCallback_)  readCallback_();
    if ((revents_ & kWriteEvent) && writeCallback_) writeCallback_();
}
