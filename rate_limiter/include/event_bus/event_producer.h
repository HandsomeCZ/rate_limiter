#pragma once
// ============================================================================
// event_producer.h — 事件生产者
//
// 职责：
//   1. 在 Service::process() 决策完成后被调用
//   2. 将 RiskEvent 异步投递到队列（不阻塞主链路）
//   3. 统计投递/丢弃数量
//
// 设计要点：
//   - publish() 是 O(1) 非阻塞操作（tryPush）
//   - 队列满时静默丢弃（不抛异常、不阻塞）
//   - 不依赖 Redis / 网络 I/O
//   - 业务线程调用 publish() 延迟 < 1μs
// ============================================================================

#include "event_bus/event_queue.h"
#include <memory>
#include <atomic>
#include <string>

class EventProducer {
public:
    explicit EventProducer(IEventQueue* queue);

    // 非阻塞发布（主链路调用）
    // 返回 true 表示入队成功，false 表示被丢弃
    bool publish(RiskEvent&& event);

    // 统计
    uint64_t published() const { return published_.load(); }
    uint64_t dropped()   const { return dropped_.load(); }

private:
    IEventQueue* queue_;
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> dropped_{0};
};
