// ============================================================================
// main.cpp — 合并入口: muduo风格 HTTP 服务器 + 限流风控
//
// 架构: HttpServer → OnMessage → RateLimitService.process() → Route
// ============================================================================

#include "Http.hpp"

#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"
#include "rate_limiter/rate_limiter.h"
#include "local_cache/local_cache.h"
#include "config_manager/config_manager.h"
#include "redis_client/redis_client.h"
#include "decision_engine/decision_engine.h"
#include "decision_engine/feature_extractor.h"

#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>

// 全局指针，用于信号处理和优雅退出
static HttpServer* g_server = nullptr;
static LocalCache* g_cache = nullptr;

void signalHandler(int sig) {
    std::cout << "\n[INFO] Received signal " << sig << ", shutting down...\n";
    if (g_cache) g_cache->stop();
    exit(0);
}

// ============ 初始化限流风控系统 ============
RateLimitService* initRateLimiter(RedisClient*& outRedis,
                                   LocalCache*& outCache,
                                   RateLimiterFacade*& outLimiter,
                                   FeatureExtractorPipeline*& outPipeline,
                                   DecisionEngine*& outEngine)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "=== Initializing Rate Limit System ===\n";

    // 1. 配置
    auto& cfg = ConfigManager::instance();
    cfg.setRedisConfig({"127.0.0.1", 6379, 32, 200});

    // 加载限流规则
    std::vector<Rule> rules;
    Rule r1; r1.id = 1; r1.name = "user_basic";
    r1.limitType = LimitType::USER; r1.algorithm = Algorithm::SLIDING_WINDOW;
    r1.windowSec = 1; r1.maxReq = 100; r1.priority = 10;
    rules.push_back(r1);

    Rule r2; r2.id = 2; r2.name = "ip_basic";
    r2.limitType = LimitType::IP; r2.algorithm = Algorithm::SLIDING_WINDOW;
    r2.windowSec = 1; r2.maxReq = 200; r2.priority = 20;
    rules.push_back(r2);

    Rule r3; r3.id = 3; r3.name = "api_strict";
    r3.limitType = LimitType::COMBO; r3.algorithm = Algorithm::SLIDING_WINDOW;
    r3.windowSec = 1; r3.maxReq = 20; r3.priority = 5;
    r3.matchApi = "/api/v1/order";
    rules.push_back(r3);

    cfg.setRules(rules);
    cfg.addWhitelist("admin", "", "");
    cfg.addBlacklist("", "192.168.1.100", "");

    // 加载风控规则
    std::vector<RiskRule> riskRules;
    RiskRule rr1; rr1.id = 101; rr1.name = "high_freq_ip";
    rr1.priority = 1; rr1.score = 30; rr1.category = "behavior";
    auto cond1 = std::make_shared<ThresholdCondition>("request_rate", ThresholdCondition::GE, 50);
    rr1.condition = cond1;
    riskRules.push_back(std::move(rr1));

    cfg.setRiskRules(riskRules);

    // 2. 本地缓存
    outCache = new LocalCache(&cfg, std::chrono::milliseconds(1000));
    g_cache = outCache;
    outCache->start();

    // 3. Redis
    outRedis = new RedisClient("127.0.0.1", 6379, 32, 200);
    bool redisOk = outRedis->init();
    if (!redisOk) std::cout << "[WARN] Redis not available, running degraded\n";

    // 4. 限流器门面
    outLimiter = new RateLimiterFacade(outRedis, 16);

    // 5. 特征提取器
    outPipeline = new FeatureExtractorPipeline();
    outPipeline->add(std::make_unique<StaticFeatureExtractor>());
    if (redisOk) outPipeline->add(std::make_unique<VelocityFeatureExtractor>(outRedis));

    // 6. 决策引擎
    outEngine = new DecisionEngine();

    // 7. 服务层
    auto* service = new RateLimitService(outCache, outLimiter, outRedis, outPipeline, outEngine);

    std::cout << "=== Rate Limit System Ready ===\n\n";
    return service;
}

// ============ 主函数 ============
int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) port = std::stoi(argv[1]);

    // 初始化限流系统
    RedisClient* redis = nullptr;
    LocalCache* cache = nullptr;
    RateLimiterFacade* limiter = nullptr;
    FeatureExtractorPipeline* pipeline = nullptr;
    DecisionEngine* engine = nullptr;
    RateLimitService* rateLimiter = initRateLimiter(redis, cache, limiter, pipeline, engine);

    // 创建 HTTP 服务器
    HttpServer server(port, 10);
    g_server = &server;
    server.SetThreadCount(4);

    // === 注入限流器 ===
    server.SetRateLimiter(rateLimiter);

    // 设置静态文件目录
    server.SetBaseDir("./www");

    // === 定义路由 ===

    // 健康检查
    server.Get("/health", [](const HttpRequest& req, HttpResponse* rsp) {
        rsp->SetContent("{\"status\":\"ok\"}", "application/json");
        rsp->SetClose(false);
    });

    // API: 用户信息
    server.Get("/api/v1/user/info", [](const HttpRequest& req, HttpResponse* rsp) {
        rsp->SetContent("{\"user\":{\"id\":\"123\",\"name\":\"demo\"}}", "application/json");
        rsp->SetClose(false);
    });

    // API: 订单
    server.Get("/api/v1/order", [](const HttpRequest& req, HttpResponse* rsp) {
        rsp->SetContent("{\"order\":{\"id\":\"456\",\"status\":\"pending\"}}", "application/json");
        rsp->SetClose(false);
    });

    server.Post("/api/v1/order", [](const HttpRequest& req, HttpResponse* rsp) {
        rsp->SetContent("{\"result\":\"created\",\"body\":\"" + req._body + "\"}", "application/json");
    });

    // API: 限流统计查询
    server.Get("/api/v1/stats", [rateLimiter](const HttpRequest& req, HttpResponse* rsp) {
        auto& s = rateLimiter->stats();
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"total\":%lu,\"allowed\":%lu,\"rejected\":%lu,\"degraded\":%lu}",
            s.total.load(), s.allowed.load(), s.rejected.load(), s.degraded.load());
        rsp->SetContent(buf, "application/json");
        rsp->SetClose(false);
    });

    // 404 兜底
    server.Get("/.*", [](const HttpRequest& req, HttpResponse* rsp) {
        rsp->_statu = 404;
    });

    std::cout << "====================================\n";
    std::cout << "  TCP/HTTP Server + Rate Limiter\n";
    std::cout << "  Listening on port: " << port << "\n";
    std::cout << "  Endpoints:\n";
    std::cout << "    GET  /health          - health check\n";
    std::cout << "    GET  /api/v1/user/info - user info\n";
    std::cout << "    GET  /api/v1/order     - order query (strict limit)\n";
    std::cout << "    POST /api/v1/order     - create order\n";
    std::cout << "    GET  /api/v1/stats     - rate limit stats\n";
    std::cout << "  Static files: ./www/\n";
    std::cout << "====================================\n\n";

    server.Listen();

    // Cleanup (不会执行到，signal handler 会 exit)
    delete rateLimiter;
    delete engine;
    delete pipeline;
    delete limiter;
    delete redis;
    delete cache;
    return 0;
}
