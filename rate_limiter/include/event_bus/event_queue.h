#pragma once
// ============================================================================
// event_queue.h — 事件队列抽象
//
// IEventQueue     : 抽象接口（未来替换为 Kafka/Pulsar 的适配器）
// BoundedEventQueue : 内存有界队列（mutex + condition_variable）
//
// 设计决策：
//   为什么有界（bounded）而不是无界（unbounded）？
//     - 无界队列在消费者慢时会导致 OOM
//     - 有界队列 + 丢弃策略 = 背压保护
//   丢弃策略：tryPush 失败时直接丢弃（不阻塞生产者）
//     - 指标数据允许少量丢失（99.9% 准确率可接受）
//     - 如果阻塞生产者，会影响主链路延迟（不可接受）
// ============================================================================

#include "event/risk_event.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

// ---------------------------------------------------------------------------
// IEventQueue — 事件队列抽象接口
//
// 为什么抽接口？
//   - 内存队列只适合单机，分布式需要 Kafka/Pulsar
//   - 抽象后：换 Kafka 只需写 KafkaEventQueue 实现 IEventQueue
//   - EventProducer / EventConsumer 只依赖接口，不依赖具体队列
// ---------------------------------------------------------------------------
class IEventQueue {
public:
    virtual ~IEventQueue() = default;

    // 非阻塞入队。成功返回 true，队列满返回 false（事件被丢弃）
    virtual bool tryPush(RiskEvent&& event) = 0;

    // 阻塞出队（消费者使用），超时返回 false
    virtual bool pop(RiskEvent& out, int timeoutMs) = 0;

    // 队列状态
    virtual size_t size() const = 0;
    virtual size_t capacity() const = 0;

    // 统计
    virtual uint64_t totalPushed()  const = 0;
    virtual uint64_t totalDropped() const = 0;
    virtual uint64_t totalPopped()  const = 0;

    // 优雅退出：唤醒阻塞的消费者
    virtual void stop() = 0;
};

// ---------------------------------------------------------------------------
// BoundedEventQueue — 有界内存队列
//
// 并发模型：多生产者 + 单消费者（MSPC）
//   - tryPush：mutex + 非阻塞（满则返回 false）
//   - pop：mutex + condition_variable（空则等待）
// ---------------------------------------------------------------------------
class BoundedEventQueue : public IEventQueue {
public:
    explicit BoundedEventQueue(size_t cap = 65536)
        : capacity_(cap) {}

    bool tryPush(RiskEvent&& event) override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (q_.size() >= capacity_) {
            dropped_++;
            return false;
        }
        q_.push(std::move(event));
        pushed_++;
        cv_.notify_one();
        return true;
    }

    bool pop(RiskEvent& out, int timeoutMs) override {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this] { return !q_.empty() || stopped_; })) {
            return false;
        }
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        popped_++;
        return true;
    }

    // 优雅退出：唤醒消费者
    void stop() {
        std::lock_guard<std::mutex> lock(mtx_);
        stopped_ = true;
        cv_.notify_all();
    }

    size_t size()     const override { std::lock_guard<std::mutex> lock(mtx_); return q_.size(); }
    size_t capacity() const override { return capacity_; }

    uint64_t totalPushed()  const override { return pushed_.load(); }
    uint64_t totalDropped() const override { return dropped_.load(); }
    uint64_t totalPopped()  const override { return popped_.load(); }

private:
    std::queue<RiskEvent> q_;
    size_t capacity_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_ = false;

    std::atomic<uint64_t> pushed_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> popped_{0};
};
