// EventConsumer实现：后台线程循环消费(1s超时可中断) → Metrics::record + 退出前排空队列
#include "event_bus/event_consumer.h"
#include "metrics/metrics.h"
#include <cstdio>

EventConsumer::EventConsumer(IEventQueue* queue,
                             Metrics* metrics,
                             std::function<void(const RiskEvent&)> onEvent)
    : queue_(queue), metrics_(metrics), onEvent_(std::move(onEvent)) {}

EventConsumer::~EventConsumer() {
    stop();
}

void EventConsumer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { consumeLoop(); });
    printf("[EventConsumer] started\n");
}

void EventConsumer::stop() {
    running_.store(false);
    if (queue_) queue_->stop();  // 唤醒阻塞在 pop 的线程
    if (thread_.joinable()) thread_.join();
    printf("[EventConsumer] stopped (consumed=%llu)\n",
           (unsigned long long)consumed_.load());
}

void EventConsumer::consumeLoop() {
    RiskEvent event;
    while (running_.load(std::memory_order_acquire)) {
        // 阻塞等待事件（1 秒超时，避免 stop 时永久阻塞）
        if (!queue_->pop(event, 1000)) {
            continue;  // 超时或停止
        }

        // ---- 指标统计 ----
        if (metrics_) {
            metrics_->record(event);
        }

        // ---- 可选回调（写日志 / 转发 Kafka / 写本地文件） ----
        if (onEvent_) {
            onEvent_(event);
        }

        consumed_++;
    }

    // 退出前排空队列中剩余事件
    RiskEvent remaining;
    while (queue_->pop(remaining, 0)) {
        if (metrics_) metrics_->record(remaining);
        if (onEvent_) onEvent_(remaining);
        consumed_++;
    }
}
