#pragma once
// ============================================================================
// metrics.h — 核心指标统计（单例）
//
// 职责：
//   1. 全局原子计数器（total / allow / limit / challenge / reject）
//   2. 规则命中计数（rule_hit_count[ruleId]）
//   3. 接收 RiskEvent 并更新所有相关指标
//
// 线程安全：
//   所有计数器都是 atomic，无锁
//   rule_hit_count 的 map 用 mutex 保护（写入频率低，锁争用可忽略）
//
// 为什么是单例？
//   - 指标是全局状态，Consumer / Exporter / Service 都需要访问
//   - 类似 ConfigManager，依赖注入会造成构造参数爆炸
// ============================================================================

#include "common/common.h"
#include "event/risk_event.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

class SlidingWindowAggregator;  // 前向声明

class Metrics {
public:
    static Metrics& instance();

    // ---- 记录事件（由 EventConsumer 调用） ----
    void record(const RiskEvent& event);

    // ---- 基础计数器（只读） ----
    uint64_t total()     const { return total_.load(); }
    uint64_t allowed()   const { return allow_.load(); }
    uint64_t limited()   const { return limit_.load(); }
    uint64_t challenged() const { return challenge_.load(); }
    uint64_t rejected()  const { return reject_.load(); }

    // ---- 规则命中 ----
    uint64_t ruleHitCount(uint64_t ruleId) const;

    // 获取所有规则命中统计的快照
    std::vector<std::pair<uint64_t, uint64_t>> ruleHitSnapshot() const;

    // ---- 滑动窗口 ----
    SlidingWindowAggregator& slidingWindow() { return *sliding_; }
    const SlidingWindowAggregator& slidingWindow() const { return *sliding_; }

    // ---- 重置（压测用） ----
    void reset();

private:
    Metrics();
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // 基础计数器
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> allow_{0};
    std::atomic<uint64_t> limit_{0};
    std::atomic<uint64_t> challenge_{0};
    std::atomic<uint64_t> reject_{0};

    // 规则命中计数（ruleId → count）
    mutable std::mutex ruleMutex_;
    std::unordered_map<uint64_t, std::atomic<uint64_t>*> ruleHits_;

    // 滑动窗口聚合器
    std::unique_ptr<SlidingWindowAggregator> sliding_;
};
