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
            auto it = ruleHits_.find(ruleId);
            if (it == ruleHits_.end()) {
                // 首次命中该规则，动态创建计数器
                auto* ptr = new std::atomic<uint64_t>(1);
                ruleHits_[ruleId] = ptr;
            } else {
                it->second->fetch_add(1);
            }
        }
    }

    // ---- 3. 滑动窗口 ----
    sliding_->recordRequest(event.decision == Decision::REJECT);
}

uint64_t Metrics::ruleHitCount(uint64_t ruleId) const {
    std::lock_guard<std::mutex> lock(ruleMutex_);
    auto it = ruleHits_.find(ruleId);
    return (it != ruleHits_.end()) ? it->second->load() : 0;
}

std::vector<std::pair<uint64_t, uint64_t>> Metrics::ruleHitSnapshot() const {
    std::lock_guard<std::mutex> lock(ruleMutex_);
    std::vector<std::pair<uint64_t, uint64_t>> result;
    result.reserve(ruleHits_.size());
    for (auto& kv : ruleHits_) {
        result.emplace_back(kv.first, kv.second->load());
    }
    return result;
}

void Metrics::reset() {
    total_  = 0; allow_  = 0; limit_  = 0;
    challenge_ = 0; reject_ = 0;

    {
        std::lock_guard<std::mutex> lock(ruleMutex_);
        for (auto& kv : ruleHits_) delete kv.second;
        ruleHits_.clear();
    }

    sliding_->reset();
}
