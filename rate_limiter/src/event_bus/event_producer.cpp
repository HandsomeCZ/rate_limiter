// EventProducer实现：非阻塞publish(tryPush) → 队列满则静默丢弃 + 计数器
#include "event_bus/event_producer.h"

EventProducer::EventProducer(IEventQueue* queue) : queue_(queue) {}

bool EventProducer::publish(RiskEvent&& event) {
    if (!queue_) {
        dropped_++;
        return false;
    }

    bool ok = queue_->tryPush(std::move(event));
    if (ok) {
        published_++;
    } else {
        dropped_++;
    }
    return ok;
}
