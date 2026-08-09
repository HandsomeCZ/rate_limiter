#pragma once
// ============================================================================
// rate_limiter.h — 限流器接口 + 门面
//
// 设计：
//   为什么拆成 IRateLimiter → FixedWindow / SlidingWindow → RateLimiterFacade？
//     - 策略模式：算法可替换
//     - RateLimiterFacade 负责选算法 / 分片 / 降级，不自己计数
//     - 单一职责：每个 Limiter 只负责自己的算法逻辑
// ============================================================================

#include "common/common.h"
#include "rule_engine/rule.h"
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// 前向声明
// ---------------------------------------------------------------------------
class RedisClient;

// ---------------------------------------------------------------------------
// 限流器抽象接口
// ---------------------------------------------------------------------------
class IRateLimiter {
public:
    virtual ~IRateLimiter() = default;

    // 检查是否允许通过
    // @param key         Redis key
    // @param windowSec   窗口大小（秒）
    // @param maxReq      最大请求数
    // @return true=允许, false=拒绝
    virtual bool check(RedisClient* redis,
                       const std::string& key,
                       uint32_t windowSec,
                       uint32_t maxReq) = 0;

    virtual Algorithm algorithm() const = 0;
};

// ---------------------------------------------------------------------------
// 固定窗口限流器
//
// 原理：
//   INCR key → 如果返回值 > maxReq → REJECT
//   首次写入时 SETEX key windowSec
//
// 为什么用 INCR + EXPIRE 而不是 Lua 包装？
//   - 固定窗口精度要求低（边界突刺是已知缺陷）
//   - INCR 是 O(1) 操作，比 Lua 快
//   - 面试面试即可说明"更严格场景用滑动窗口"
//
// 为什么不用 Redis SET NX + INCR（更精确的原子方案）？
//   - 那个方案需要 2 次 Redis 往返，增加延迟
//   - INCR + 首次 EXPIRE 在 99% 场景下正确
// ---------------------------------------------------------------------------
class FixedWindowLimiter : public IRateLimiter {
public:
    bool check(RedisClient* redis, const std::string& key,
               uint32_t windowSec, uint32_t maxReq) override;
    Algorithm algorithm() const override { return Algorithm::FIXED_WINDOW; }
};

// ---------------------------------------------------------------------------
// 滑动窗口限流器（基于 Redis ZSet + Lua）
//
// Lua 脚本做的事情（一条脚本完成全部，保证原子性）：
//   1. ZREMRANGEBYSCORE key 0 (now - window)   — 清理窗口外
//   2. ZADD key now member                     — 记录本次请求
//   3. ZCARD key                               — 窗口内计数
//   4. 判断 count <= maxReq → 放行/拒绝
//   5. 拒绝则回滚 ZREM
//
// 为什么必须用 Lua？
//   - 5 步操作必须原子执行，否则会有 race condition
//   - 减少网络往返（5 步 → 1 次）
//
// 为什么是 ZSet 不是 List？
//   - ZSet 按 score 范围删除 O(log N)，List 需要遍历 O(N)
//
// member 为什么是 (nowMs + random)？
//   - ZSet member 必须唯一，同一毫秒可能多个请求
// ---------------------------------------------------------------------------
class SlidingWindowLimiter : public IRateLimiter {
public:
    bool check(RedisClient* redis, const std::string& key,
               uint32_t windowSec, uint32_t maxReq) override;
    Algorithm algorithm() const override { return Algorithm::SLIDING_WINDOW; }
};

// ---------------------------------------------------------------------------
// RateLimiterFacade — 限流器门面
//
// 职责：
//   1. 根据 Rule.algorithm 选择 FixedWindow 或 SlidingWindow
//   2. 构建 Redis Key
//   3. 热点 key 分片
//   4. 降级处理（fail-open）
//   5. 线程安全
//
// 热点 key 分片策略：
//   为什么分片？
//     - 热点用户/API 会导致单个 Redis key 成为瓶颈
//     - 分片后请求分散到多个 key，各分片独立计数
//   怎么做？
//     - 写：随机选一个分片 → 检查该分片是否满 → 写入
//     - 读（本实现简化为写时检查）：只检查单个分片
//     - 每个分片配额 = 总配额 / 分片数
//   为什么不用"读所有分片求和"？
//     - 增加 Redis 调用次数，延迟翻倍
//     - 分片策略本身就有误差容忍（±1/分片数）
// ---------------------------------------------------------------------------
class RateLimiterFacade {
public:
    explicit RateLimiterFacade(RedisClient* redis, uint32_t shardCount = 16);
    ~RateLimiterFacade();

    // 核心入口：检查单条规则
    // @return true=允许, false=拒绝
    bool checkRule(const Request& req, const Rule& rule, Stats& stats);

    // NEW: 带自定义配额的限流检查（风控 LIMIT 级别使用）
    // @param customQuota 替代 rule.maxReq 的自定义配额
    bool checkRuleWithQuota(const Request& req, const Rule& rule,
                            Stats& stats, uint32_t customQuota);

    // 获取/设置热点分片数
    uint32_t shardCount() const { return shardCount_; }
    void setShardCount(uint32_t n) { shardCount_ = n; }

private:
    // 构建 Redis key（分片版）
    std::string buildKey(const Request& req, const Rule& rule);

    RedisClient* redis_;
    uint32_t shardCount_;
    std::unique_ptr<FixedWindowLimiter>   fixedWindow_;
    std::unique_ptr<SlidingWindowLimiter> slidingWindow_;

    // 判断是否需要分片（简化：所有 key 都分片）
    bool needShard(const Rule& rule) const;
};
