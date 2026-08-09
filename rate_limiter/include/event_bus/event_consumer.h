#pragma once
// ============================================================================
// event_consumer.h — 事件消费者
//
// 职责：
//   1. 独立后台线程，循环从队列取事件
//   2. 将事件送入 Metrics::record() 做统计
//   3. 支持优雅退出（stop + join）
//
// 设计要点：
//   - 单线程消费（避免 Metrics 写入竞争）
//   - 消费者失败不丢事件：重试或写死信队列（本版重试 1 次）
//   - 与 Metrics 解耦：Consumer 只负责搬运，Metrics 负责统计
// ============================================================================

#include "event_bus/event_queue.h"
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

class Metrics;  // 前向声明

class EventConsumer {
public:
    // @param queue   事件队列
    // @param metrics 指标统计（nullptr = 不统计）
    // @param onEvent 可选回调（用于写日志/Kafka，不阻塞消费循环）
    EventConsumer(IEventQueue* queue,
                  Metrics* metrics,
                  std::function<void(const RiskEvent&)> onEvent = nullptr);

    ~EventConsumer();

    // 启动后台消费线程
    void start();

    // 优雅退出（停止消费 + 等待线程结束）
    void stop();

    // 状态
    bool running() const { return running_.load(); }
    uint64_t consumed() const { return consumed_.load(); }

private:
    void consumeLoop();  // 后台线程主循环

    IEventQueue* queue_;
    Metrics* metrics_;
    std::function<void(const RiskEvent&)> onEvent_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> consumed_{0};
};
