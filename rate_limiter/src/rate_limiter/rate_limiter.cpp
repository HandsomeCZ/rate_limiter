// 限流器实现：FixedWindow(INCR+EXPIRE) / SlidingWindow(ZSet+Lua) / Facade(分片+降级fail-open)
#include "rate_limiter/rate_limiter.h"
#include "redis_client/redis_client.h"
#include <random>

// ============================================================================
// FixedWindowLimiter
// ============================================================================
bool FixedWindowLimiter::check(RedisClient* redis, const std::string& key,
                                uint32_t windowSec, uint32_t maxReq) {
    int64_t count = redis->fixedWindowIncr(key, windowSec);
    if (count < 0) return true;  // Redis 故障 → fail-open
    return count <= static_cast<int64_t>(maxReq);
}

// ============================================================================
// SlidingWindowLimiter
// ============================================================================
bool SlidingWindowLimiter::check(RedisClient* redis, const std::string& key,
                                  uint32_t windowSec, uint32_t maxReq) {
    return redis->slidingWindowCheck(key, windowSec, maxReq);
}

// ============================================================================
// RateLimiterFacade
// ============================================================================

RateLimiterFacade::RateLimiterFacade(RedisClient* redis, uint32_t shardCount)
    : redis_(redis)
    , shardCount_(shardCount)
    , fixedWindow_(std::make_unique<FixedWindowLimiter>())
    , slidingWindow_(std::make_unique<SlidingWindowLimiter>())
{}

RateLimiterFacade::~RateLimiterFacade() = default;

std::string RateLimiterFacade::buildKey(const Request& req, const Rule& rule) {
    // 根据 rule 的匹配字段选择 id
    std::string id;
    switch (rule.limitType) {
        case LimitType::USER:
            id = req.userId;
            break;
        case LimitType::IP:
            id = req.ip;
            break;
        case LimitType::API:
            id = req.api;
            break;
        case LimitType::COMBO:
            // 组合维度：user+api 或 ip+api
            if (!rule.matchUserId.empty() && !rule.matchApi.empty())
                id = req.userId + "+" + req.api;
            else if (!rule.matchIp.empty() && !rule.matchApi.empty())
                id = req.ip + "+" + req.api;
            else
                id = req.userId + "+" + req.api;  // 默认 user+api
            break;
    }
    return KeyBuilder::build(rule.limitType, id, req.api);
}

bool RateLimiterFacade::needShard(const Rule& /*rule*/) const {
    // 简化：所有规则都分片（热点 key 保护）
    return shardCount_ > 1;
}

bool RateLimiterFacade::checkRule(const Request& req, const Rule& rule,
                                   Stats& stats) {
    std::string baseKey = buildKey(req, rule);

    // 判断 Redis 是否可用
    if (!redis_->isAvailable()) {
        stats.recordDegraded();
        return true;  // fail-open
    }

    bool allowed = false;

    try {
        if (needShard(rule)) {
            // 分片：随机选一个分片写入 + 检查
            static thread_local std::mt19937 rng(std::random_device{}());
            uint32_t shardIdx = rng() % shardCount_;
            allowed = redis_->slidingWindowShardCheck(
                baseKey, rule.windowSec, rule.maxReq,
                shardIdx, shardCount_);
        } else {
            // 不分片：标准滑动窗口或固定窗口
            if (rule.algorithm == Algorithm::FIXED_WINDOW) {
                allowed = fixedWindow_->check(redis_, baseKey,
                                              rule.windowSec, rule.maxReq);
            } else {
                allowed = slidingWindow_->check(redis_, baseKey,
                                                rule.windowSec, rule.maxReq);
            }
        }
    } catch (...) {
        // 异常兜底
        stats.recordDegraded();
        return true;
    }

    return allowed;
}

// NEW: 带自定义配额的限流检查
bool RateLimiterFacade::checkRuleWithQuota(const Request& req, const Rule& rule,
                                            Stats& stats, uint32_t customQuota) {
    std::string baseKey = buildKey(req, rule);

    if (!redis_->isAvailable()) {
        stats.recordDegraded();
        return true;
    }

    bool allowed = false;

    try {
        if (needShard(rule)) {
            static thread_local std::mt19937 rng(std::random_device{}());
            uint32_t shardIdx = rng() % shardCount_;
            allowed = redis_->slidingWindowShardCheck(
                baseKey, rule.windowSec, customQuota,
                shardIdx, shardCount_);
        } else {
            if (rule.algorithm == Algorithm::FIXED_WINDOW) {
                allowed = fixedWindow_->check(redis_, baseKey,
                                              rule.windowSec, customQuota);
            } else {
                allowed = slidingWindow_->check(redis_, baseKey,
                                                rule.windowSec, customQuota);
            }
        }
    } catch (...) {
        stats.recordDegraded();
        return true;
    }

    return allowed;
}
