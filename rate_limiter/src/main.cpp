// ============================================================================
// main.cpp — 系统启动入口（示例）
//
// 展示依赖注入与初始化顺序
// ============================================================================

#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"
#include "rate_limiter/rate_limiter.h"
#include "local_cache/local_cache.h"
#include "config_manager/config_manager.h"
#include "redis_client/redis_client.h"
#include "decision_engine/decision_engine.h"
#include "decision_engine/feature_extractor.h"

#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "=== Rate Limit System Starting ===\n";

    // 1. 配置管理器
    auto& cfg = ConfigManager::instance();
    cfg.setRedisConfig({"127.0.0.1", 6379, 32, 200});

    // 加载示例限流规则
    std::vector<Rule> rules;
    Rule r1;
    r1.id = 1; r1.name = "user_basic";
    r1.limitType = LimitType::USER;
    r1.algorithm = Algorithm::SLIDING_WINDOW;
    r1.windowSec = 1; r1.maxReq = 100; r1.priority = 10;
    rules.push_back(r1);

    Rule r2;
    r2.id = 2; r2.name = "ip_basic";
    r2.limitType = LimitType::IP;
    r2.algorithm = Algorithm::SLIDING_WINDOW;
    r2.windowSec = 1; r2.maxReq = 200; r2.priority = 20;
    rules.push_back(r2);

    Rule r3;
    r3.id = 3; r3.name = "order_api";
    r3.limitType = LimitType::COMBO;
    r3.algorithm = Algorithm::SLIDING_WINDOW;
    r3.windowSec = 1; r3.maxReq = 20;
    r3.priority = 5;
    r3.matchApi = "/api/v1/order";
    rules.push_back(r3);

    cfg.setRules(rules);
    cfg.addWhitelist("admin", "", "");
    cfg.addBlacklist("", "192.168.1.100", "");

    // 加载示例风控规则
    std::vector<RiskRule> riskRules;
    RiskRule rr1;
    rr1.id = 101; rr1.name = "high_sensitivity_api";
    rr1.priority = 1; rr1.score = 30; rr1.category = "api_risk";
    auto cond1 = std::make_shared<ThresholdCondition>("api_sensitivity", ThresholdCondition::GE, 2);
    rr1.condition = cond1;
    riskRules.push_back(std::move(rr1));

    RiskRule rr2;
    rr2.id = 102; rr2.name = "night_operation";
    rr2.priority = 2; rr2.score = 10; rr2.category = "behavior";
    auto cond2 = std::make_shared<ThresholdCondition>("time_hour_risk", ThresholdCondition::EQ, 1);
    rr2.condition = cond2;
    riskRules.push_back(std::move(rr2));

    cfg.setRiskRules(riskRules);

    // 2. 本地缓存
    LocalCache cache(&cfg, std::chrono::milliseconds(1000));
    cache.start();

    // 3. Redis 连接
    RedisClient redis("127.0.0.1", 6379, 32, 200);
    bool redisOk = redis.init();
    if (!redisOk) {
        std::cout << "[WARN] Redis not available, running degraded\n";
    }

    // 4. 限流器门面（16 分片）
    RateLimiterFacade limiter(&redis, 16);

    // 5. 特征提取器流水线
    FeatureExtractorPipeline featurePipeline;
    featurePipeline.add(std::make_unique<StaticFeatureExtractor>());
    if (redisOk) {
        featurePipeline.add(std::make_unique<VelocityFeatureExtractor>(&redis));
    }

    // 6. 决策引擎
    DecisionEngine decisionEngine;

    // 7. 服务层（5 参数构造函数）
    RateLimitService service(&cache, &limiter, &redis,
                             &featurePipeline, &decisionEngine);

    // 8. 控制器
    RateLimitController controller(&service);

    std::cout << "=== System Ready ===\n\n";

    // 7. 模拟请求
    const int N = 100000;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i) {
        std::string uid = "user_" + std::to_string(i % 100);
        std::string ip  = "192.168.1." + std::to_string(i % 50);
        std::string api = (i % 3 == 0) ? "/api/v1/order" : "/api/v1/user/info";

        controller.allow(uid, ip, api);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    auto& s = controller.stats();
    double qps = (double)s.total.load() * 1000.0 / ms;
    double rejectRate = s.total.load() > 0
        ? (double)s.rejected.load() / s.total.load() * 100.0 : 0.0;

    std::cout << "Requests: " << s.total.load() << "\n";
    std::cout << "Allowed:  " << s.allowed.load() << "\n";
    std::cout << "Rejected: " << s.rejected.load() << "\n";
    std::cout << "QPS:      " << qps << "\n";
    std::cout << "Reject%:  " << rejectRate << "%\n";
    std::cout << "Degraded: " << s.degraded.load() << "\n";
    std::cout << "Duration: " << ms << "ms\n";

    cache.stop();
    return 0;
}
