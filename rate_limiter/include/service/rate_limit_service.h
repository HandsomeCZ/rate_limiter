#pragma once
// ============================================================================
// RateLimitService — 核心业务编排层（升级版：限流 + 风控）
//
// 完整处理流程（process 方法）：
//   ┌──────────────────────────────────────────────────────────┐
//   │ Phase 0: FastPath — 黑白名单（0 网络开销，0 评分开销）   │
//   │ Phase 1: Feature Extraction — 提取风险信号               │
//   │ Phase 2: Rule Matching — 匹配风控规则                    │
//   │ Phase 3: Risk Scoring — 评分 + 分级决策                  │
//   │ Phase 4: Action Dispatch — 按级别分派动作                │
//   │                                                          │
//   │ ALLOW   → 直接放行                                       │
//   │ LIMIT   → 收紧配额 + RateLimiter 判定                    │
//   │ REJECT  → 直接拒绝                                       │
//   │                                                          │
//   │ 三条流水线完全解耦：                                     │
//   │   FeatureExtractor → "你是谁？"（信号）                   │
//   │   DecisionEngine   → "有多危险？"（评分）                 │
//   │   RateLimiter      → "还能来多少次？"（频率）             │
//   └──────────────────────────────────────────────────────────┘
// ============================================================================

#include "common/common.h"
#include "rule_engine/rule.h"
#include "decision_engine/decision_engine.h"
#include "decision_engine/feature.h"
#include "decision_engine/risk_rule.h"
#include <memory>

class LocalCache;
class RateLimiterFacade;
class RedisClient;
class Stats;
class FeatureExtractorPipeline;
class DecisionEngine;
class EventProducer;  // NEW

class RateLimitService {
public:
    RateLimitService(LocalCache* cache,
                     RateLimiterFacade* limiter,
                     RedisClient* redis,
                     FeatureExtractorPipeline* featurePipeline,
                     DecisionEngine* decisionEngine);
    ~RateLimitService() = default;

    // 核心入口：处理一次请求（新 pipeline）
    Decision process(const Request& req);

    // 仅限流模式（兼容旧接口，跳过风控评分）
    Decision processRateLimitOnly(const Request& req);

    // NEW: 注入事件生产者（可选，nullptr = 不上报）
    void setEventProducer(EventProducer* producer) { eventProducer_ = producer; }

    // NEW: 生成请求 ID（简单实现，生产环境用雪花算法/UUID）
    void setRequestIdGenerator(std::function<std::string()> gen) {
        requestIdGen_ = std::move(gen);
    }

    // 统计
    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

private:
    // NEW: 构造并发布风控事件（异步）
    void publishRiskEvent(const Request& req, Decision decision,
                          int riskScore, int limitQuota,
                          const std::vector<uint64_t>& ruleIds,
                          const std::string& reason,
                          uint64_t latencyUs);

    LocalCache*               cache_;
    RateLimiterFacade*        limiter_;
    RedisClient*              redis_;
    FeatureExtractorPipeline* featurePipeline_;
    DecisionEngine*           decisionEngine_;
    Stats                     stats_;

    // NEW: 数据回流
    EventProducer* eventProducer_ = nullptr;
    std::function<std::string()> requestIdGen_;
};
