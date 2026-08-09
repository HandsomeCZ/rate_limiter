// Service层实现：4阶段风控+限流流水线编排（FastPath → Feature → Score → Dispatch）
#include "service/rate_limit_service.h"
#include "local_cache/local_cache.h"
#include "rate_limiter/rate_limiter.h"
#include "redis_client/redis_client.h"
#include "decision_engine/feature_extractor.h"
#include "event/risk_event.h"
#include "event_bus/event_producer.h"
#include <chrono>

RateLimitService::RateLimitService(LocalCache* cache,
                                   RateLimiterFacade* limiter,
                                   RedisClient* redis,
                                   FeatureExtractorPipeline* featurePipeline,
                                   DecisionEngine* decisionEngine)
    : cache_(cache), limiter_(limiter), redis_(redis),
      featurePipeline_(featurePipeline),
      decisionEngine_(decisionEngine) {}

// ============================================================================
// publishRiskEvent — 构造事件 + 异步投递（非阻塞）
//
// 为什么放在 Service 内部？
//   - Service 掌握完整的决策上下文（score / ruleIds / decision）
//   - 避免把 EventProducer 暴露给外部调用方
//
// 为什么 eventProducer_ 为 nullptr 时静默跳过？
//   - 数据回流是可选的（压测 / 调试时不需要）
// ============================================================================
void RateLimitService::publishRiskEvent(
    const Request& req, Decision decision,
    int riskScore, int limitQuota,
    const std::vector<uint64_t>& ruleIds,
    const std::string& reason,
    uint64_t latencyUs) {

    if (!eventProducer_) return;

    // 生成 requestId（优先用外部注入的生成器）
    std::string reqId;
    if (requestIdGen_) {
        reqId = requestIdGen_();
    } else {
        // 默认：hash(userId + ip + timestamp)
        reqId = std::to_string(
            std::hash<std::string>{}(req.userId + req.ip +
                                     std::to_string(req.timestampMs)));
    }

    auto event = RiskEvent::make(reqId, req, decision,
                                  riskScore, limitQuota,
                                  ruleIds, reason, latencyUs);
    eventProducer_->publish(std::move(event));
}

// ============================================================================
// process() — 完整风控 + 限流流水线
// ============================================================================
Decision RateLimitService::process(const Request& req) {
    auto t0 = std::chrono::steady_clock::now();

    // ═══════════════════════════════════════════════════════════════════
    // Phase 0: FastPath — 黑白名单
    // ═══════════════════════════════════════════════════════════════════
    if (cache_->isWhitelisted(req)) {
        stats_.recordAllow();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        publishRiskEvent(req, Decision::ALLOW, 0, 100, {},
                         "whitelist", us);
        return Decision::ALLOW;
    }
    if (cache_->isBlacklisted(req)) {
        stats_.recordReject();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        publishRiskEvent(req, Decision::REJECT, 100, 0, {},
                         "blacklist", us);
        return Decision::REJECT;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase 1: Feature Extraction
    // ═══════════════════════════════════════════════════════════════════
    std::vector<Feature> features;
    if (featurePipeline_) {
        features = featurePipeline_->extractAll(req);
    }

    auto limitRules = cache_->getRules(req);

    // ═══════════════════════════════════════════════════════════════════
    // Phase 2 + 3: Rule Matching + Risk Scoring
    // ═══════════════════════════════════════════════════════════════════
    DecisionResult riskResult;
    bool hasRiskDecision = false;

    if (decisionEngine_ && !features.empty()) {
        auto riskRules = cache_->getRiskRules(req);
        if (!riskRules.empty()) {
            riskResult = decisionEngine_->evaluate(riskRules, features);
            hasRiskDecision = true;
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase 4: Action Dispatch
    // ═══════════════════════════════════════════════════════════════════
    if (hasRiskDecision) {
        switch (riskResult.decision) {

            case Decision::ALLOW: {
                stats_.recordAllow();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                publishRiskEvent(req, Decision::ALLOW,
                                 riskResult.riskScore, riskResult.limitQuota,
                                 riskResult.triggeredRuleIds,
                                 riskResult.reason, us);
                return Decision::ALLOW;
            }

            case Decision::CHALLENGE: {
                stats_.recordAllow();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                publishRiskEvent(req, Decision::CHALLENGE,
                                 riskResult.riskScore, riskResult.limitQuota,
                                 riskResult.triggeredRuleIds,
                                 riskResult.reason, us);
                return Decision::CHALLENGE;
            }

            case Decision::LIMIT: {
                for (const auto& rule : limitRules) {
                    bool ok = limiter_->checkRuleWithQuota(
                        req, rule, stats_, riskResult.limitQuota);
                    if (!ok) {
                        stats_.recordReject();
                        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        publishRiskEvent(req, Decision::REJECT,
                                         riskResult.riskScore, riskResult.limitQuota,
                                         riskResult.triggeredRuleIds,
                                         "limit_exceeded", us);
                        return Decision::REJECT;
                    }
                }
                stats_.recordAllow();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                publishRiskEvent(req, Decision::LIMIT,
                                 riskResult.riskScore, riskResult.limitQuota,
                                 riskResult.triggeredRuleIds,
                                 riskResult.reason, us);
                return Decision::LIMIT;
            }

            case Decision::REJECT: {
                stats_.recordReject();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                publishRiskEvent(req, Decision::REJECT,
                                 riskResult.riskScore, riskResult.limitQuota,
                                 riskResult.triggeredRuleIds,
                                 riskResult.reason, us);
                return Decision::REJECT;
            }
        }
    }

    // ---- 降级：纯限流模式 ----
    return processRateLimitOnly(req);
}

// ============================================================================
// processRateLimitOnly()
// ============================================================================
Decision RateLimitService::processRateLimitOnly(const Request& req) {
    auto t0 = std::chrono::steady_clock::now();

    if (cache_->isWhitelisted(req)) {
        stats_.recordAllow();
        return Decision::ALLOW;
    }
    if (cache_->isBlacklisted(req)) {
        stats_.recordReject();
        return Decision::REJECT;
    }

    auto rules = cache_->getRules(req);
    if (rules.empty()) {
        stats_.recordAllow();
        return Decision::ALLOW;
    }

    for (const auto& rule : rules) {
        if (!limiter_->checkRule(req, rule, stats_)) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            publishRiskEvent(req, Decision::REJECT, 0, 0, {},
                             "rate_limit", us);
            return Decision::REJECT;
        }
    }

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    publishRiskEvent(req, Decision::ALLOW, 0, 100, {},
                     "rate_limit_pass", us);
    return Decision::ALLOW;
}
