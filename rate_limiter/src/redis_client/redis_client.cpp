// Redis存储层实现：连接池(32) + RAII封装 + Lua原子执行(SLIDE_LUA/SHARD_LUA) + 熔断fail-open
#include "redis_client/redis_client.h"

#include "common/common.h"
#include <hiredis/hiredis.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>

// ============================================================================
// RedisReply
// ============================================================================

RedisReply::RedisReply(redisReply* r) : reply_(r) {}

RedisReply::~RedisReply() {
    if (reply_) { freeReplyObject(reply_); reply_ = nullptr; }
}

RedisReply::RedisReply(RedisReply&& o) noexcept : reply_(o.reply_) {
    o.reply_ = nullptr;
}

RedisReply& RedisReply::operator=(RedisReply&& o) noexcept {
    if (this != &o) {
        if (reply_) freeReplyObject(reply_);
        reply_ = o.reply_;
        o.reply_ = nullptr;
    }
    return *this;
}

bool RedisReply::ok() const {
    return reply_ && reply_->type != REDIS_REPLY_ERROR;
}

int64_t RedisReply::integer() const {
    return (reply_ && reply_->type == REDIS_REPLY_INTEGER) ? reply_->integer : -1;
}

const char* RedisReply::str() const {
    return (reply_ && (reply_->type == REDIS_REPLY_STRING ||
                       reply_->type == REDIS_REPLY_ERROR)) ? reply_->str : "";
}

bool RedisReply::isNil() const {
    return reply_ && reply_->type == REDIS_REPLY_NIL;
}

// ============================================================================
// RedisConnection
// ============================================================================

RedisConnection::RedisConnection(const std::string& host, int port, int timeoutMs)
    : host_(host), port_(port), timeoutMs_(timeoutMs) {}

RedisConnection::~RedisConnection() { disconnect(); }

bool RedisConnection::connect() {
    disconnect();
    struct timeval tv;
    tv.tv_sec  = timeoutMs_ / 1000;
    tv.tv_usec = (timeoutMs_ % 1000) * 1000;

    ctx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (!ctx_ || ctx_->err) {
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        return false;
    }
    redisEnableKeepAlive(ctx_);
    return true;
}

bool RedisConnection::isAlive() {
    if (!ctx_) return false;
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "PING"));
    bool ok = r && r->type == REDIS_REPLY_STATUS && strcmp(r->str, "PONG") == 0;
    if (r) freeReplyObject(r);
    return ok;
}

void RedisConnection::disconnect() {
    if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
}

RedisReply RedisConnection::exec(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!ctx_) return RedisReply(nullptr);

    va_list ap;
    va_start(ap, fmt);
    redisReply* r = static_cast<redisReply*>(redisvCommand(ctx_, fmt, ap));
    va_end(ap);

    // 自动重连一次
    if (!r && ctx_->err) {
        if (connect()) {
            va_start(ap, fmt);
            r = static_cast<redisReply*>(redisvCommand(ctx_, fmt, ap));
            va_end(ap);
        }
    }
    return RedisReply(r);
}

RedisReply RedisConnection::eval(const std::string& script,
                                  const std::vector<std::string>& keys,
                                  const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!ctx_) return RedisReply(nullptr);

    // 构造 EVAL 参数数组
    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.push_back("EVAL");   argvlen.push_back(4);
    argv.push_back(script.c_str()); argvlen.push_back(script.size());

    std::string nk = std::to_string(keys.size());
    argv.push_back(nk.c_str()); argvlen.push_back(nk.size());

    for (auto& k : keys)  { argv.push_back(k.c_str()); argvlen.push_back(k.size()); }
    for (auto& a : args)  { argv.push_back(a.c_str()); argvlen.push_back(a.size()); }

    redisReply* r = static_cast<redisReply*>(
        redisCommandArgv(ctx_, (int)argv.size(), argv.data(), argvlen.data()));

    if (!r && ctx_->err) {
        if (connect()) {
            r = static_cast<redisReply*>(
                redisCommandArgv(ctx_, (int)argv.size(), argv.data(), argvlen.data()));
        }
    }
    return RedisReply(r);
}

// ============================================================================
// RedisConnectionPool
// ============================================================================

RedisConnectionPool::RedisConnectionPool(const std::string& host, int port,
                                         int poolSize, int timeoutMs)
    : host_(host), port_(port), poolSize_(poolSize), timeoutMs_(timeoutMs) {}

RedisConnectionPool::~RedisConnectionPool() {
    std::lock_guard<std::mutex> lock(mtx_);
    while (!idle_.empty()) idle_.pop();
}

bool RedisConnectionPool::init() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (int i = 0; i < poolSize_; ++i) {
        auto conn = std::make_shared<RedisConnection>(host_, port_, timeoutMs_);
        if (!conn->connect()) {
            fprintf(stderr, "[RedisPool] conn %d/%d failed\n", i + 1, poolSize_);
            return false;
        }
        idle_.push(conn);
    }
    printf("[RedisPool] %d connections ready\n", poolSize_);
    return true;
}

std::shared_ptr<RedisConnection> RedisConnectionPool::borrow(int waitMs) {
    std::unique_lock<std::mutex> lock(mtx_);
    if (idle_.empty()) {
        if (waitMs == 0) return nullptr;
        if (!cv_.wait_for(lock, std::chrono::milliseconds(waitMs),
                          [this] { return !idle_.empty(); }))
            return nullptr;
    }
    auto conn = idle_.front();
    idle_.pop();
    outstanding_++;
    return conn;
}

void RedisConnectionPool::recycle(std::shared_ptr<RedisConnection> conn) {
    if (!conn) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        idle_.push(conn);
        outstanding_--;
    }
    cv_.notify_one();
}

bool RedisConnectionPool::healthy() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return !idle_.empty();
}

size_t RedisConnectionPool::idleCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return idle_.size();
}

// ============================================================================
// RedisClient — Lua 脚本
// ============================================================================

// 滑动窗口原子判定
static const char* SLIDE_LUA = R"LUA(
-- 滑动窗口限流 Lua 脚本
-- KEYS[1]    : rate limit key
-- ARGV[1]    : now (ms)
-- ARGV[2]    : window size (ms)
-- ARGV[3]    : unique member (now_ms + random)
-- ARGV[4]    : max requests
-- ARGV[5]    : TTL (seconds)

local now     = tonumber(ARGV[1])
local window  = tonumber(ARGV[2])
local member  = ARGV[3]
local max_req = tonumber(ARGV[4])
local ttl     = tonumber(ARGV[5])

-- 1. 删除窗口外的旧数据
redis.call('ZREMRANGEBYSCORE', KEYS[1], 0, now - window)

-- 2. 先检查计数（避免无效的 ZADD）
local count = redis.call('ZCARD', KEYS[1])

if count >= max_req then
    redis.call('EXPIRE', KEYS[1], ttl)
    return 0  -- 拒绝
end

-- 3. 记录本次请求
redis.call('ZADD', KEYS[1], now, member)
redis.call('EXPIRE', KEYS[1], ttl)
return 1  -- 放行
)LUA";

// 分片版滑动窗口
static const char* SHARD_LUA = R"LUA(
-- 分片滑动窗口（逻辑同上，单分片独立判断）
local now     = tonumber(ARGV[1])
local window  = tonumber(ARGV[2])
local member  = ARGV[3]
local max_req = tonumber(ARGV[4])
local ttl     = tonumber(ARGV[5])

redis.call('ZREMRANGEBYSCORE', KEYS[1], 0, now - window)
local count = redis.call('ZCARD', KEYS[1])

if count >= max_req then
    redis.call('EXPIRE', KEYS[1], ttl)
    return 0
end

redis.call('ZADD', KEYS[1], now, member)
redis.call('EXPIRE', KEYS[1], ttl)
return 1
)LUA";

// ============================================================================
// RedisClient
// ============================================================================

RedisClient::RedisClient(const std::string& host, int port,
                         int poolSize, int timeoutMs) {
    pool_ = std::make_unique<RedisConnectionPool>(host, port, poolSize, timeoutMs);
}

bool RedisClient::init() {
    return pool_->init();
}

int64_t RedisClient::fixedWindowIncr(const std::string& key, uint32_t windowSec) {
    auto conn = pool_->borrow();
    if (!conn) {
        consecutiveFailures_++;
        return -1;
    }

    auto reply = conn->exec("INCR %s", key.c_str());
    if (!reply.ok()) {
        consecutiveFailures_++;
        pool_->recycle(conn);
        return -1;
    }

    int64_t v = reply.integer();
    if (v == 1) {
        conn->exec("EXPIRE %s %u", key.c_str(), windowSec);
    }

    consecutiveFailures_ = 0;
    pool_->recycle(conn);
    return v;
}

bool RedisClient::slidingWindowCheck(const std::string& key,
                                      uint32_t windowSec, uint32_t maxReq) {
    auto conn = pool_->borrow();
    if (!conn) { consecutiveFailures_++; return true; }  // fail-open

    uint64_t now = nowMs();
    // 唯一 member: timestamp + 随机数，避免同毫秒冲突
    static thread_local std::mt19937 rng(std::random_device{}());
    std::string member = std::to_string(now) + "_" +
                         std::to_string(rng() % 100000);

    std::vector<std::string> keys = {key};
    std::vector<std::string> args = {
        std::to_string(now),
        std::to_string(windowSec * 1000ULL),
        member,
        std::to_string(maxReq),
        std::to_string(windowSec + 2)
    };

    auto reply = conn->eval(SLIDE_LUA, keys, args);
    pool_->recycle(conn);

    if (!reply.ok()) { consecutiveFailures_++; return true; }
    consecutiveFailures_ = 0;
    return reply.integer() == 1;
}

bool RedisClient::slidingWindowShardCheck(const std::string& baseKey,
                                           uint32_t windowSec, uint32_t maxReq,
                                           uint32_t shardIdx, uint32_t shardCount) {
    auto conn = pool_->borrow();
    if (!conn) { consecutiveFailures_++; return true; }

    uint64_t now = nowMs();
    static thread_local std::mt19937 rng(std::random_device{}());
    std::string member = std::to_string(now) + "_" +
                         std::to_string(rng() % 100000);

    uint32_t perShard = (maxReq + shardCount - 1) / shardCount;
    std::string key = baseKey + ":" + std::to_string(shardIdx);

    std::vector<std::string> keys = {key};
    std::vector<std::string> args = {
        std::to_string(now),
        std::to_string(windowSec * 1000ULL),
        member,
        std::to_string(perShard),
        std::to_string(windowSec + 2)
    };

    auto reply = conn->eval(SHARD_LUA, keys, args);
    pool_->recycle(conn);

    if (!reply.ok()) { consecutiveFailures_++; return true; }
    consecutiveFailures_ = 0;
    return reply.integer() == 1;
}

int64_t RedisClient::evalInt(const std::string& script,
                              const std::vector<std::string>& keys,
                              const std::vector<std::string>& args) {
    auto conn = pool_->borrow();
    if (!conn) return -1;

    auto reply = conn->eval(script, keys, args);
    pool_->recycle(conn);

    if (!reply.ok()) return -1;
    return reply.integer();
}

bool RedisClient::isAvailable() {
    // 熔断检查
    if (consecutiveFailures_ >= circuitThreshold_) {
        circuitOpen_ = true;
        return false;
    }
    circuitOpen_ = false;
    return pool_->healthy();
}
