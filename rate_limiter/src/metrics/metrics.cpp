// Metrics单例实现：atomic基础计数 + 规则命中(动态map+mutex) + 滑动窗口聚合
#include "metrics/metrics.h"
#include "metrics/aggregator.h"
#include <cstdio>

Metrics::Metrics() {
    sliding_ = std::make_unique<SlidingWindowAggregator>();
}

Metrics& Metrics::instance() {
    static Metrics inst;
    return inst;
}

void Metrics::record(const RiskEvent& event) {
    // ---- 1. 基础计数 ----
    total_++;

    switch (event.decision) {
        case Decision::ALLOW:     allow_++;     break;
        case Decision::LIMIT:     limit_++;     break;
        case Decision::CHALLENGE: challenge_++; break;
        case Decision::REJECT:    reject_++;    break;
    }

    // ---- 2. 规则命中计数 ----
    if (!event.triggeredRuleIds.empty()) {
        std::lock_guard<std::mutex> lock(ruleMutex_);
        for (auto ruleId : event.triggeredRuleIds) {
            ++ruleHits_[ruleId];  // operator[] 缺省插入 0 再自增
        }
    }

    // ---- 3. 滑动窗口 ----
    sliding_->recordRequest(event.decision == Decision::REJECT);
}

uint64_t Metrics::ruleHitCount(uint64_t ruleId) const {
    std::lock_guard<std::mutex> lock(ruleMutex_);
    auto it = ruleHits_.find(ruleId);
    return (it != ruleHits_.end()) ? it->second : 0;
}

std::vector<std::pair<uint64_t, uint64_t>> Metrics::ruleHitSnapshot() const {
    std::lock_guard<std::mutex> lock(ruleMutex_);
    std::vector<std::pair<uint64_t, uint64_t>> result;
    result.reserve(ruleHits_.size());
    for (auto& kv : ruleHits_) {
        result.emplace_back(kv.first, kv.second);
    }
    return result;
}

void Metrics::reset() {
    total_  = 0; allow_  = 0; limit_  = 0;
    challenge_ = 0; reject_ = 0;

    {
        std::lock_guard<std::mutex> lock(ruleMutex_);
        ruleHits_.clear();
    }

    sliding_->reset();
}
