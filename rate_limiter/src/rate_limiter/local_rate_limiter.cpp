#include "rate_limiter/local_rate_limiter.h"
#include "redis_client/redis_client.h"
#include <chrono>
#include <vector>

LocalRateLimiter::LocalRateLimiter(RedisClient* redis, int syncIntervalMs)
    : redis_(redis), syncIntervalMs_(syncIntervalMs) {}

LocalRateLimiter::~LocalRateLimiter() {
    stop();
}

bool LocalRateLimiter::check(const Request& req, const Rule& rule) {
    return checkWithQuota(req, rule, rule.maxReq);
}

bool LocalRateLimiter::checkWithQuota(const Request& req, const Rule& rule, uint32_t customQuota) {
    totalChecks_.fetch_add(1, std::memory_order_relaxed);

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::string key = buildKey(req, rule);
    Shard& shard = shards_[shardIndex(key)];

    bool needRedis = false;
    {
        std::lock_guard<std::mutex> lock(shard.mtx);
        auto& window = getOrCreateWindow(shard, key, rule.windowSec, customQuota);

        // 1. 本地快速检查
        size_t localCount = window.count(nowMs);

        // 安全区：本地计数 < 80% 阈值 → 直接放行（不查 Redis）
        uint32_t safeThreshold = static_cast<uint32_t>(customQuota * 0.8);
        if (localCount < safeThreshold) {
            window.checkAndRecord(nowMs);  // 记录本地计数
            localAllowed_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // 警戒区：本地计数 >= 80% → 需要查 Redis 兜底（在锁外做 I/O，避免阻塞）
        needRedis = true;
    }

    // 2. 警戒区：Redis 精确校验（网络 I/O 在锁外执行）
    if (needRedis && redis_ && redis_->isAvailable()) {
        redisFallbacks_.fetch_add(1, std::memory_order_relaxed);
        bool redisAllowed = redis_->slidingWindowCheck(key, rule.windowSec, customQuota);

        std::lock_guard<std::mutex> lock(shard.mtx);
        auto& window = getOrCreateWindow(shard, key, rule.windowSec, customQuota);
        if (redisAllowed) {
            window.checkAndRecord(nowMs);  // 同步记录到本地
            localAllowed_.fetch_add(1, std::memory_order_relaxed);
            return true;
        } else {
            localRejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // 3. Redis 不可用 → Fail-Open（默认放行）
    std::lock_guard<std::mutex> lock(shard.mtx);
    auto& window = getOrCreateWindow(shard, key, rule.windowSec, customQuota);
    window.checkAndRecord(nowMs);
    localAllowed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::string LocalRateLimiter::buildKey(const Request& req, const Rule& rule) const {
    // reserve + append 拼接，避免 ostringstream 的堆分配（热路径）
    std::string key;
    key.reserve(6 + 16 + 1 + req.userId.size() + 1 + req.api.size() + 8);
    key.append("local:");
    appendU32(key, rule.id);
    key.push_back(':');
    switch (rule.limitType) {
        case LimitType::USER:
            key.append("user:"); key.append(req.userId);
            break;
        case LimitType::IP:
            key.append("ip:"); key.append(req.ip);
            break;
        case LimitType::COMBO:
            key.append("combo:"); key.append(req.userId);
            key.push_back(':'); key.append(req.api);
            break;
        case LimitType::API:
            key.append("api:"); key.append(req.api);
            break;
        default:
            key.append("unknown");
            break;
    }
    return key;
}

// 带 Stats 统计的检查（适配 Service 层接口）
bool LocalRateLimiter::checkRule(const Request& req, const Rule& rule, Stats& stats) {
    // 注意：不在这里计数，由 processRateLimitOnly() 统一计数
    return check(req, rule);
}

// 带自定义配额 + Stats 统计的检查
bool LocalRateLimiter::checkRuleWithQuota(const Request& req, const Rule& rule,
                                          Stats& stats, uint32_t customQuota) {
    bool ok = checkWithQuota(req, rule, customQuota);
    // 注意：不在这里计数，由 process() 统一计数
    return ok;
}

LocalSlidingWindow& LocalRateLimiter::getOrCreateWindow(Shard& shard,
                                                          const std::string& key,
                                                          uint32_t windowSec,
                                                          uint32_t maxReq) {
    auto it = shard.windows.find(key);
    if (it == shard.windows.end()) {
        auto result = shard.windows.emplace(key, LocalSlidingWindow(windowSec, maxReq));
        return result.first->second;
    }
    return it->second;
}

void LocalRateLimiter::start() {
    if (running_.load() || !redis_) return;
    running_.store(true);
    syncThread_ = std::thread(&LocalRateLimiter::syncToRedis, this);
}

void LocalRateLimiter::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (syncThread_.joinable()) {
        syncThread_.join();
    }
}

void LocalRateLimiter::syncToRedis() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(syncIntervalMs_));

        // 先在锁内收集各分片的增量，再在锁外做 Redis I/O，避免长事务阻塞热路径
        struct Pending { std::string key; uint32_t delta; uint32_t windowSec; };
        std::vector<Pending> pending;
        pending.reserve(256);
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mtx);
            for (auto& [key, window] : shard.windows) {
                // 只同步自上次以来的增量，避免每个周期重复累加全量窗口计数（原 bug）
                uint32_t delta = static_cast<uint32_t>(window.takePending());
                if (delta > 0) {
                    // 用独立命名空间，避免与警戒区 slidingWindowCheck（ZSet）在同一 key 上
                    // 执行 INCRBY（string）导致 Redis WRONGTYPE 错误。
                    pending.push_back({key + ":sync", delta, window.windowSec_});
                }
            }
        }

        for (auto& p : pending) {
            redis_->incrBy(p.key, p.delta);
            redis_->expire(p.key, p.windowSec);
        }
        redisSyncs_.fetch_add(1, std::memory_order_relaxed);
    }
}
