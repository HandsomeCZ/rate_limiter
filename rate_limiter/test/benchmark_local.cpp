// ============================================================================
// benchmark_local.cpp — 本地限流器压测（无 Redis 依赖）
//
// 用法:
//   ./benchmark_local [--qps 100000] [--duration 10] [--threads 8]
//
// 特点:
//   - 纯本地计数，零网络开销
//   - 预期 QPS 10w+
//   - 延迟 < 10μs
// ============================================================================

#include "rate_limiter/local_rate_limiter.h"
#include "redis_client/redis_client.h"
#include "config_manager/config_manager.h"
#include "rule_engine/rule_engine.h"
#include "decision_engine/decision_engine.h"
#include "decision_engine/feature_extractor.h"
#include "common/common.h"

#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <iomanip>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>

// ---------------------------------------------------------------------------
// 命令行参数解析
// ---------------------------------------------------------------------------
struct BenchConfig {
    int targetQps  = 100000;
    int durationSec = 10;
    int threads    = 8;
};

BenchConfig parseArgs(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--qps") == 0 && i + 1 < argc)
            cfg.targetQps = std::stoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg.durationSec = std::stoi(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            cfg.threads = std::stoi(argv[++i]);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// 简单 Stats
// ---------------------------------------------------------------------------
struct SimpleStats {
    std::atomic<uint64_t> total{0};
    std::atomic<uint64_t> allowed{0};
    std::atomic<uint64_t> rejected{0};
    void reset() { total = 0; allowed = 0; rejected = 0; }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    auto benchCfg = parseArgs(argc, argv);

    std::cout << "================================================\n";
    std::cout << "  Rate Limit System — Local Benchmark\n";
    std::cout << "================================================\n";
    std::cout << "  Target QPS : " << benchCfg.targetQps << "\n";
    std::cout << "  Duration   : " << benchCfg.durationSec << "s\n";
    std::cout << "  Threads    : " << benchCfg.threads << "\n";
    std::cout << "  Mode       : Local + Redis Fallback\n";
    std::cout << "================================================\n\n";

    // =====================================================================
    // 1. 初始化本地限流器（带 Redis 兜底）
    // =====================================================================
    RedisClient redis("127.0.0.1", 6379, 32, 200);
    bool redisAvailable = redis.init();
    
    LocalRateLimiter localLimiter(redisAvailable ? &redis : nullptr, 100);
    localLimiter.start();
    if (redisAvailable) {
        std::cout << "[Redis] Connected — fallback enabled\n";
    } else {
        std::cout << "[Redis] Not available — pure local mode\n";
    }

    // 配置限流规则
    std::vector<Rule> rules;

    // 规则1：每用户每秒最多 10000 次请求
    Rule userRule;
    userRule.id = 1;
    userRule.name = "user_rate_limit";
    userRule.limitType = LimitType::USER;
    userRule.windowSec = 1;
    userRule.maxReq = 10000;
    userRule.enabled = true;
    rules.push_back(userRule);

    // 规则2：每 IP 每秒最多 20000 次请求
    Rule ipRule;
    ipRule.id = 2;
    ipRule.name = "ip_rate_limit";
    ipRule.limitType = LimitType::IP;
    ipRule.windowSec = 1;
    ipRule.maxReq = 20000;
    ipRule.enabled = true;
    rules.push_back(ipRule);

    // 规则3：API 维度全局限流
    Rule apiRule;
    apiRule.id = 3;
    apiRule.name = "api_global_limit";
    apiRule.limitType = LimitType::API;
    apiRule.windowSec = 1;
    apiRule.maxReq = 50000;
    apiRule.enabled = true;
    rules.push_back(apiRule);

    // =====================================================================
    // 2. 准备测试数据
    // =====================================================================
    std::vector<std::string> userIds(100);
    for (int i = 0; i < 100; ++i) userIds[i] = "user_" + std::to_string(i);
    
    std::vector<std::string> ips(50);
    for (int i = 0; i < 50; ++i) ips[i] = "192.168.1." + std::to_string(i);
    
    std::vector<std::string> apis = {
        "/api/v1/order", "/api/v1/user/info", "/api/v1/product/list"
    };

    // =====================================================================
    // 3. 执行压测
    // =====================================================================
    SimpleStats stats;
    stats.reset();

    int totalRequests = benchCfg.targetQps * benchCfg.durationSec;
    int perThread = totalRequests / benchCfg.threads;

    std::vector<uint64_t> latencies(totalRequests);
    std::atomic<uint64_t> latencyIdx{0};

    std::cout << "Total requests: " << totalRequests << "\n";
    std::cout << "Per thread: " << perThread << "\n\n";

    auto startTime = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < benchCfg.threads; ++t) {
        threads.emplace_back([&, t]() {
            thread_local std::mt19937 rng(42 + t);  // 固定种子保证可重复
            std::uniform_int_distribution<size_t> uidDist(0, userIds.size() - 1);
            std::uniform_int_distribution<size_t> ipDist(0, ips.size() - 1);
            std::uniform_int_distribution<size_t> apiDist(0, apis.size() - 1);

            for (int i = 0; i < perThread; ++i) {
                Request req;
                req.userId = userIds[uidDist(rng)];
                req.ip     = ips[ipDist(rng)];
                req.api    = apis[apiDist(rng)];

                auto t1 = std::chrono::steady_clock::now();
                
                // 检查所有规则
                bool allowed = true;
                for (const auto& rule : rules) {
                    if (!localLimiter.check(req, rule)) {
                        allowed = false;
                        break;
                    }
                }

                auto t2 = std::chrono::steady_clock::now();
                uint64_t us = std::chrono::duration_cast<
                    std::chrono::microseconds>(t2 - t1).count();
                
                uint64_t idx = latencyIdx.fetch_add(1);
                if (idx < latencies.size()) {
                    latencies[idx] = us;
                }

                stats.total.fetch_add(1, std::memory_order_relaxed);
                if (allowed) {
                    stats.allowed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    stats.rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 等待完成
    for (auto& th : threads) th.join();
    auto endTime = std::chrono::steady_clock::now();
    auto actualMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime).count();

    // =====================================================================
    // 4. 输出结果
    // =====================================================================
    size_t valid = latencyIdx.load();
    if (valid > latencies.size()) valid = latencies.size();
    std::sort(latencies.begin(), latencies.begin() + valid);

    uint64_t p50 = latencies[valid * 50 / 100];
    uint64_t p99 = latencies[valid * 99 / 100];
    uint64_t avg = 0;
    for (size_t i = 0; i < valid; ++i) avg += latencies[i];
    avg /= valid;

    double actualQps = (double)stats.total.load() * 1000.0 / actualMs;
    double rejectRate = stats.total.load() > 0
        ? (double)stats.rejected.load() / stats.total.load() * 100.0 : 0.0;

    std::cout << "\n================================================\n";
    std::cout << "  BENCHMARK RESULTS\n";
    std::cout << "================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Actual QPS    : " << actualQps << "\n";
    std::cout << "  Total Requests: " << stats.total.load() << "\n";
    std::cout << "  Allowed       : " << stats.allowed.load() << "\n";
    std::cout << "  Rejected      : " << stats.rejected.load() << "\n";
    std::cout << "  Reject Rate   : " << rejectRate << "%\n";
    std::cout << "------------------------------------------------\n";
    std::cout << "  Avg Latency   : " << avg << " μs\n";
    std::cout << "  P50 Latency   : " << p50 << " μs\n";
    std::cout << "  P99 Latency   : " << p99 << " μs\n";
    std::cout << "  Max Latency   : " << latencies[valid - 1] << " μs\n";
    std::cout << "  Duration      : " << actualMs << " ms\n";
    std::cout << "================================================\n";

    // 限流器统计
    std::cout << "\n=== Local Rate Limiter Stats ===\n";
    std::cout << "  Total Checks : " << localLimiter.totalChecks() << "\n";
    std::cout << "  Local Allowed: " << localLimiter.localAllowed() << "\n";
    std::cout << "  Local Rejected: " << localLimiter.localRejected() << "\n";
    std::cout << "  Redis Fallbacks: " << localLimiter.redisFallbacks() << "\n";
    std::cout << "  Redis Syncs  : " << localLimiter.redisSyncs() << "\n";
    std::cout << "================================\n";

    return 0;
}