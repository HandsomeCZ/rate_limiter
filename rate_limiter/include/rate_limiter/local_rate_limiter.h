#pragma once
// ============================================================================
// local_rate_limiter.h — 本地滑动窗口限流器
//
// 设计目标：
//   1. 本地计数，零网络开销
//   2. 滑动窗口算法，保证精度
//   3. 定时同步到 Redis（最终一致性）
//   4. 支持多规则、多维度（user/ip/api/combo）
//
// 性能预期：
//   - 本地计数延迟 < 1μs
//   - 10w+ QPS 无压力
//   - Redis 同步延迟 100ms（可配置）
//
// 并发模型（分片锁）：
//   - 单一全局 mutex 在 10w QPS 下是所有线程的争用热点
//   - 拆成 kShardCount 个分片，按 key 哈希路由，锁粒度降到 1/kShardCount
//   - 每个分片独立 unordered_map + mutex，互不干扰
// ============================================================================

#include "common/common.h"
#include "rule_engine/rule.h"
#include <unordered_map>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <array>
#include <vector>

class RedisClient;

// ---------------------------------------------------------------------------
// LocalSlidingWindow — 单窗口滑动计数器
//
// 原理：
//   - 用 deque 存储每个请求的时间戳（毫秒）
//   - 检查时先清理过期数据（now - windowSec）
//   - 判断 count <= maxReq
//   - 允许则 push_back(now)
// ---------------------------------------------------------------------------
struct LocalSlidingWindow {
    std::deque<uint64_t> timestamps_;
    uint32_t windowSec_;
    uint32_t maxReq_;
    // 自上次同步以来新增的请求数（用于增量同步，避免全量重复计数）
    uint64_t pendingSync_ = 0;

    LocalSlidingWindow(uint32_t windowSec = 1, uint32_t maxReq = 100)
        : windowSec_(windowSec), maxReq_(maxReq) {}

    // 检查并记录（线程不安全，需外部加锁）
    bool checkAndRecord(uint64_t nowMs) {
        // 1. 清理过期数据
        uint64_t windowStart = nowMs - windowSec_ * 1000ULL;
        while (!timestamps_.empty() && timestamps_.front() < windowStart) {
            timestamps_.pop_front();
        }

        // 2. 检查是否超限
        if (timestamps_.size() >= maxReq_) {
            return false;  // 拒绝
        }

        // 3. 记录本次请求
        timestamps_.push_back(nowMs);
        pendingSync_++;
        return true;  // 允许
    }

    // 获取当前计数
    size_t count(uint64_t nowMs) {
        uint64_t windowStart = nowMs - windowSec_ * 1000ULL;
        while (!timestamps_.empty() && timestamps_.front() < windowStart) {
            timestamps_.pop_front();
        }
        return timestamps_.size();
    }

    // 取走并清零待同步增量（同步到 Redis 用）
    uint64_t takePending() {
        uint64_t p = pendingSync_;
        pendingSync_ = 0;
        return p;
    }
};

// ---------------------------------------------------------------------------
// LocalRateLimiter — 本地限流器（支持多规则，分片锁）
// ---------------------------------------------------------------------------
class LocalRateLimiter {
public:
    explicit LocalRateLimiter(RedisClient* redis = nullptr,
                              int syncIntervalMs = 100);
    ~LocalRateLimiter();

    // 核心入口：检查请求是否允许
    bool check(const Request& req, const Rule& rule);

    // 带自定义配额的检查
    bool checkWithQuota(const Request& req, const Rule& rule, uint32_t customQuota);

    // 带 Stats 统计的检查（适配 Service 层接口）
    bool checkRule(const Request& req, const Rule& rule, Stats& stats);

    // 带自定义配额 + Stats 统计的检查
    bool checkRuleWithQuota(const Request& req, const Rule& rule,
                            Stats& stats, uint32_t customQuota);

    // 设置 Redis 客户端（可选）
    void setRedisClient(RedisClient* redis) { redis_ = redis; }

    // 启动/停止同步线程
    void start();
    void stop();

    // 统计
    uint64_t totalChecks() const { return totalChecks_.load(); }
    uint64_t localAllowed() const { return localAllowed_.load(); }
    uint64_t localRejected() const { return localRejected_.load(); }
    uint64_t redisFallbacks() const { return redisFallbacks_.load(); }
    uint64_t redisSyncs() const { return redisSyncs_.load(); }

private:
    // 分片：把单一全局锁拆成 kShardCount 个，按 key 哈希路由，降低锁争用
    static constexpr size_t kShardCount = 64;
    struct Shard {
        std::unordered_map<std::string, LocalSlidingWindow> windows;
        std::mutex mtx;
    };

    // 构建本地 key
    std::string buildKey(const Request& req, const Rule& rule) const;

    size_t shardIndex(const std::string& key) const {
        return std::hash<std::string>{}(key) % kShardCount;
    }

    // 获取或创建窗口（需已持有对应分片锁）
    LocalSlidingWindow& getOrCreateWindow(Shard& shard, const std::string& key,
                                           uint32_t windowSec,
                                           uint32_t maxReq);

    // 同步到 Redis（后台线程调用）
    void syncToRedis();

    // 数据成员
    RedisClient* redis_;
    int syncIntervalMs_;

    std::array<Shard, kShardCount> shards_;

    std::atomic<bool> running_{false};
    std::thread syncThread_;

    // 统计
    std::atomic<uint64_t> totalChecks_{0};
    std::atomic<uint64_t> localAllowed_{0};
    std::atomic<uint64_t> localRejected_{0};
    std::atomic<uint64_t> redisFallbacks_{0};
    std::atomic<uint64_t> redisSyncs_{0};
};
