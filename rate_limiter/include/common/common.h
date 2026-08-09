#pragma once
// ============================================================================
// common.h — 全项目共享类型与工具
//
// 包含：Request / Decision / LimitType / Algorithm / KeyBuilder / Stats / ThreadPool
// 所有模块无需依赖彼此，只需包含此头文件即可获得基础类型定义
// ============================================================================

#include <string>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <sstream>
#include <functional>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>

// ---------------------------------------------------------------------------
// 请求数据结构
// ---------------------------------------------------------------------------
struct Request {
    std::string userId;
    std::string ip;
    std::string api;
    uint64_t    timestampMs = 0;  // 毫秒时间戳
};

// ---------------------------------------------------------------------------
// 判定结果
// ---------------------------------------------------------------------------
enum class Decision {
    ALLOW     = 0,
    LIMIT     = 1,  // 中风险：限流放行（收紧配额）
    CHALLENGE = 2,  // 需验证：返回 CAPTCHA / 二次确认
    REJECT    = 3,  // 高风险：直接拒绝
};

inline const char* decisionStr(Decision d) {
    switch (d) {
        case Decision::ALLOW:     return "ALLOW";
        case Decision::LIMIT:     return "LIMIT";
        case Decision::CHALLENGE: return "CHALLENGE";
        case Decision::REJECT:    return "REJECT";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 限流维度
// ---------------------------------------------------------------------------
enum class LimitType : uint8_t {
    USER  = 0,  // userId
    IP    = 1,  // client IP
    API   = 2,  // API path
    COMBO = 3,  // 组合（user+api / ip+api 等）
};

inline const char* limitTypeStr(LimitType t) {
    switch (t) {
        case LimitType::USER:  return "user";
        case LimitType::IP:    return "ip";
        case LimitType::API:   return "api";
        case LimitType::COMBO: return "combo";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 限流算法
// ---------------------------------------------------------------------------
enum class Algorithm {
    FIXED_WINDOW   = 0,
    SLIDING_WINDOW = 1,
};

// ---------------------------------------------------------------------------
// Redis Key 构建器
//   格式: limit:{type}:{id}:{api}
//   示例: limit:user:10086:/api/v1/order
// ---------------------------------------------------------------------------
struct KeyBuilder {
    // 基础 key
    static std::string build(LimitType type,
                             const std::string& id,
                             const std::string& api) {
        std::ostringstream oss;
        oss << "limit:" << limitTypeStr(type) << ":" << id << ":" << api;
        return oss.str();
    }

    // 分片 key
    static std::string buildShard(LimitType type,
                                   const std::string& id,
                                   const std::string& api,
                                   uint32_t shardIdx) {
        std::ostringstream oss;
        oss << "limit:" << limitTypeStr(type) << ":"
            << id << ":" << api << ":" << shardIdx;
        return oss.str();
    }
};

// ---------------------------------------------------------------------------
// 统计（全原子操作）
// ---------------------------------------------------------------------------
struct Stats {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> allowed{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> redisErrors{0};
    std::atomic<uint64_t> degraded{0};  // 降级放行次数

    void recordAllow()    { total++; allowed++; }
    void recordReject()   { total++; rejected++; }
    void recordDegraded() { degraded++; }
    void recordRedisErr() { redisErrors++; }

    void reset() { total = allowed = rejected = redisErrors = degraded = 0; }
};

// ---------------------------------------------------------------------------
// 时间工具
// ---------------------------------------------------------------------------
inline uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline uint64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// 轻量线程池（无第三方依赖）
// ---------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        { std::lock_guard<std::mutex> lock(mtx); stop = true; }
        cv.notify_all();
        for (auto& w : workers) if (w.joinable()) w.join();
    }

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using R = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> res = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.emplace([task] { (*task)(); });
        }
        cv.notify_one();
        return res;
    }

    size_t size() const { return workers.size(); }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
};
