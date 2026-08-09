#pragma once
// ============================================================================
// redis_client.h — Redis 存储层
//
// 职责：
//   1. 管理 Redis 连接池
//   2. 提供高层操作接口（INCR / EVAL Lua）
//   3. 连接健康检查与自动重连
//
// 为什么使用连接池而不是单连接？
//   - 单机 10w QPS → 单连接吞吐上限约 3-5w QPS（受 RTT 限制）
//   - 32 连接池 → 每条连接分摊 ~3000 QPS，轻松达标
//
// 为什么选择 hiredis 而不是 cpp_redis / redis-plus-plus？
//   - hiredis 是 C 库，性能最高（零拷贝读 Buffer）
//   - 面试项目强调"自己封装"，体现理解深度
//   - 生产环境可以用 redis-plus-plus，这里体现基础能力
// ============================================================================

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <atomic>

struct redisContext;
struct redisReply;

// ---------------------------------------------------------------------------
// RedisReply — RAII 封装
// ---------------------------------------------------------------------------
class RedisReply {
public:
    explicit RedisReply(redisReply* r = nullptr);
    ~RedisReply();
    RedisReply(RedisReply&& o) noexcept;
    RedisReply& operator=(RedisReply&& o) noexcept;
    RedisReply(const RedisReply&) = delete;
    RedisReply& operator=(const RedisReply&) = delete;

    bool ok() const;
    int64_t integer() const;
    const char* str() const;
    bool isNil() const;
    redisReply* raw() const { return reply_; }

private:
    redisReply* reply_;
};

// ---------------------------------------------------------------------------
// RedisConnection — 单连接（RAII + 互斥）
// ---------------------------------------------------------------------------
class RedisConnection {
public:
    RedisConnection(const std::string& host, int port, int timeoutMs);
    ~RedisConnection();

    bool connect();
    bool isAlive();
    void disconnect();

    // 执行命令
    RedisReply exec(const char* fmt, ...);
    // 执行 Lua 脚本
    RedisReply eval(const std::string& script,
                    const std::vector<std::string>& keys,
                    const std::vector<std::string>& args);

    redisContext* raw() { return ctx_; }

private:
    std::string host_;
    int port_;
    int timeoutMs_;
    redisContext* ctx_ = nullptr;
    std::mutex mtx_;
};

// ---------------------------------------------------------------------------
// RedisConnectionPool — 连接池
// ---------------------------------------------------------------------------
class RedisConnectionPool {
public:
    RedisConnectionPool(const std::string& host, int port,
                        int poolSize, int timeoutMs);
    ~RedisConnectionPool();

    bool init();

    // 借用/归还（RAII 版本由 RedisClient 封装）
    std::shared_ptr<RedisConnection> borrow(int waitMs = 50);
    void recycle(std::shared_ptr<RedisConnection> conn);

    bool healthy() const;
    size_t idleCount() const;

private:
    std::string host_;
    int port_;
    int poolSize_;
    int timeoutMs_;
    std::queue<std::shared_ptr<RedisConnection>> idle_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    size_t outstanding_ = 0;
};

// ---------------------------------------------------------------------------
// RedisClient — 高层操作接口
// ---------------------------------------------------------------------------
class RedisClient {
public:
    RedisClient(const std::string& host, int port,
                int poolSize = 32, int timeoutMs = 200);
    ~RedisClient() = default;

    bool init();

    // 固定窗口：INCR + 首次 EXPIRE
    // 返回 -1 = 错误, >=0 = 当前计数
    int64_t fixedWindowIncr(const std::string& key, uint32_t windowSec);

    // 滑动窗口：Lua 脚本原子判定
    // 返回 true = 允许
    bool slidingWindowCheck(const std::string& key,
                            uint32_t windowSec, uint32_t maxReq);

    // 分片滑动窗口
    bool slidingWindowShardCheck(const std::string& baseKey,
                                  uint32_t windowSec, uint32_t maxReq,
                                  uint32_t shardIdx, uint32_t shardCount);

    // 通用 EVAL（执行任意 Lua，返回 integer 结果）
    int64_t evalInt(const std::string& script,
                    const std::vector<std::string>& keys,
                    const std::vector<std::string>& args);

    bool isAvailable();

private:
    std::unique_ptr<RedisConnectionPool> pool_;
    std::atomic<bool> circuitOpen_{false};  // 熔断状态
    std::atomic<int> consecutiveFailures_{0};
    int circuitThreshold_ = 10;
};
