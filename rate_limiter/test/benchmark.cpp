// ============================================================================
// benchmark.cpp — 压测工具
//
// 用法:
//   ./benchmark [--qps 10000] [--duration 10] [--threads 8]
//
// 输出:
//   - 实际 QPS
//   - 平均/P50/P99 延迟
//   - 拒绝率
// ============================================================================

#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"
#include "rate_limiter/rate_limiter.h"
#include "local_cache/local_cache.h"
#include "config_manager/config_manager.h"
#include "redis_client/redis_client.h"
#include "rule_engine/rule_engine.h"
#include "decision_engine/decision_engine.h"
#include "decision_engine/feature_extractor.h"

#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <iomanip>
#include <cstring>

// ---------------------------------------------------------------------------
// 命令行参数解析
// ---------------------------------------------------------------------------
struct BenchConfig {
    int targetQps  = 10000;
    int durationSec = 10;
    int threads    = 8;
    bool useRedis  = false;      // 是否连接真实 Redis（默认模拟）
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
        else if (strcmp(argv[i], "--redis") == 0)
            cfg.useRedis = true;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    auto benchCfg = parseArgs(argc, argv);

    std::cout << "================================================\n";
    std::cout << "  Rate Limit System — Benchmark\n";
    std::cout << "================================================\n";
    std::cout << "  Target QPS : " << benchCfg.targetQps << "\n";
    std::cout << "  Duration   : " << benchCfg.durationSec << "s\n";
    std::cout << "  Threads    : " << benchCfg.threads << "\n";
    std::cout << "  Redis      : " << (benchCfg.useRedis ? "real" : "simulated") << "\n";
    std::cout << "================================================\n\n";

    // =====================================================================
    // 1. 初始化组件（依赖注入）
    // =====================================================================
    auto& cfgMgr = ConfigManager::instance();

    // 配置限流规则
    std::vector<Rule> rules;

    // 规则1：每用户每秒最多 50 次请求（user 维度滑动窗口）
    Rule userRule;
    userRule.id = 1;
    userRule.name = "user_rate_limit";
    userRule.limitType = LimitType::USER;
    userRule.algorithm = Algorithm::SLIDING_WINDOW;
    userRule.windowSec = 1;
    userRule.maxReq = 50;
    userRule.priority = 10;
    userRule.enabled = true;
    rules.push_back(userRule);

    // 规则2：每 IP 每秒最多 100 次请求
    Rule ipRule;
    ipRule.id = 2;
    ipRule.name = "ip_rate_limit";
    ipRule.limitType = LimitType::IP;
    ipRule.algorithm = Algorithm::SLIDING_WINDOW;
    ipRule.windowSec = 1;
    ipRule.maxReq = 100;
    ipRule.priority = 20;
    ipRule.enabled = true;
    rules.push_back(ipRule);

    // 规则3：组合维度 user+api（每秒 20 次）
    Rule comboRule;
    comboRule.id = 3;
    comboRule.name = "user_api_combo";
    comboRule.limitType = LimitType::COMBO;
    comboRule.algorithm = Algorithm::SLIDING_WINDOW;
    comboRule.windowSec = 1;
    comboRule.maxReq = 20;
    comboRule.priority = 5;
    comboRule.matchApi = "/api/v1/order";
    comboRule.enabled = true;
    rules.push_back(comboRule);

    // 规则4：API 维度全局限流（每秒 1000 次）
    Rule apiRule;
    apiRule.id = 4;
    apiRule.name = "api_global_limit";
    apiRule.limitType = LimitType::API;
    apiRule.algorithm = Algorithm::FIXED_WINDOW;
    apiRule.windowSec = 1;
    apiRule.maxReq = 1000;
    apiRule.priority = 30;
    apiRule.matchApi = "/api/v1/order";
    apiRule.enabled = true;
    rules.push_back(apiRule);

    cfgMgr.setRules(rules);

    // 添加白名单（VIP 用户）
    cfgMgr.addWhitelist("vip_user_001", "", "");
    // 添加黑名单（已知攻击 IP）
    cfgMgr.addBlacklist("", "10.0.0.99", "");

    // 初始化 LocalCache
    LocalCache localCache(&cfgMgr, std::chrono::milliseconds(1000));
    localCache.start();

    // 初始化 Redis（如果使用真实 Redis）
    RedisClient* redis = nullptr;
    RateLimiterFacade* limiter = nullptr;

    if (benchCfg.useRedis) {
        redis = new RedisClient("127.0.0.1", 6379, 32, 200);
        if (!redis->init()) {
            std::cerr << "[WARN] Redis connection failed, running in degraded mode\n";
            delete redis;
            redis = nullptr;
        }
    }

    // 如果 Redis 不可用，创建一个空指针的 RateLimiterFacade
    //（实际场景会触发 fail-open）
    if (!redis) {
        std::cout << "[INFO] No Redis — all requests will be ALLOW (degraded mode)\n";
        // 最小化初始化：用一个无法连接的 Redis 模拟降级
        redis = new RedisClient("127.0.0.1", 6379, 1, 10);
    }

    limiter = new RateLimiterFacade(redis, 16);

    // 特征提取器 + 决策引擎
    FeatureExtractorPipeline featurePipeline;
    featurePipeline.add(std::make_unique<StaticFeatureExtractor>());
    if (benchCfg.useRedis) {
        featurePipeline.add(std::make_unique<VelocityFeatureExtractor>(redis));
    }
    DecisionEngine decisionEngine;

    // 构建 Service → Controller（5 参数）
    RateLimitService service(&localCache, limiter, redis,
                             &featurePipeline, &decisionEngine);
    RateLimitController controller(&service);

    // =====================================================================
    // 2. 准备测试数据
    // =====================================================================
    std::vector<std::string> userIds = {
        "user_1001", "user_1002", "user_1003", "user_2001", "user_2002",
        "vip_user_001", "user_3001", "user_3002", "user_4001", "user_4002"
    };
    std::vector<std::string> ips = {
        "192.168.1.1", "192.168.1.2", "192.168.1.3",
        "10.0.0.1", "10.0.0.2", "10.0.0.99"  // 10.0.0.99 是黑名单
    };
    std::vector<std::string> apis = {
        "/api/v1/order", "/api/v1/user/info", "/api/v1/product/list"
    };

    // =====================================================================
    // 3. 执行压测
    // =====================================================================
    auto& stats = controller.stats();
    stats.reset();

    int totalRequests = benchCfg.targetQps * benchCfg.durationSec;
    int perThread = totalRequests / benchCfg.threads;

    std::vector<uint64_t> latencies(totalRequests);
    std::atomic<uint64_t> latencyIdx{0};

    std::cout << "Total requests: " << totalRequests << "\n";
    std::cout << "Per thread: " << perThread << "\n\n";

    ThreadPool pool(benchCfg.threads);
    std::vector<std::future<void>> futures;

    auto startTime = std::chrono::steady_clock::now();

    for (int t = 0; t < benchCfg.threads; ++t) {
        futures.push_back(pool.submit([&, t]() {
            thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<size_t> uidDist(0, userIds.size() - 1);
            std::uniform_int_distribution<size_t> ipDist(0, ips.size() - 1);
            std::uniform_int_distribution<size_t> apiDist(0, apis.size() - 1);

            for (int i = 0; i < perThread; ++i) {
                Request req;
                req.userId = userIds[uidDist(rng)];
                req.ip     = ips[ipDist(rng)];
                req.api    = apis[apiDist(rng)];

                auto t1 = std::chrono::steady_clock::now();
                auto decision = controller.allow(req);
                auto t2 = std::chrono::steady_clock::now();

                uint64_t us = std::chrono::duration_cast<
                    std::chrono::microseconds>(t2 - t1).count();
                uint64_t idx = latencyIdx.fetch_add(1);
                if (idx < latencies.size()) {
                    latencies[idx] = us;
                }

                // 速率控制：确保不超过目标 QPS
                if (benchCfg.targetQps > 0 && benchCfg.targetQps < 100000) {
                    auto elapsed = std::chrono::duration_cast<
                        std::chrono::milliseconds>(t2 - startTime).count();
                    int expected = (int)((double)benchCfg.targetQps *
                                         elapsed / 1000.0 / benchCfg.threads);
                    if (i > expected + 10) {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                }
            }
        }));
    }

    // 等待完成
    for (auto& f : futures) f.get();
    auto endTime = std::chrono::steady_clock::now();
    auto actualMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime).count();

    // =====================================================================
    // 4. 输出结果
    // =====================================================================
    // 排序延迟（取前 latencyIdx 个有效值）
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
    std::cout << "  Degraded      : " << stats.degraded.load() << "\n";
    std::cout << "  Redis Errors  : " << stats.redisErrors.load() << "\n";
    std::cout << "------------------------------------------------\n";
    std::cout << "  Avg Latency   : " << avg << " μs\n";
    std::cout << "  P50 Latency   : " << p50 << " μs\n";
    std::cout << "  P99 Latency   : " << p99 << " μs\n";
    std::cout << "  Max Latency   : " << latencies[valid - 1] << " μs\n";
    std::cout << "  Duration      : " << actualMs << " ms\n";
    std::cout << "================================================\n";

    // =====================================================================
    // 5. 清理
    // =====================================================================
    localCache.stop();
    delete limiter;
    delete redis;

    return 0;
}
