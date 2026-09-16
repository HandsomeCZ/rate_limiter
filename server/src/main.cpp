// ============================================================================
// main.cpp 合并入口: muduo风格 HTTP 服务+ 限流风控
//
// 架构: HttpServer OnMessage -> RateLimitService.process() -> Route
// ============================================================================

#include "Http.hpp"

#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"
#include "rate_limiter/local_rate_limiter.h"
#include "local_cache/local_cache.h"
#include "config_manager/config_manager.h"
#include "redis_client/redis_client.h"
#include "decision_engine/decision_engine.h"
#include "decision_engine/feature_extractor.h"
#include "net/LoopThreadPool.h"
#include "net/TimerWheel.h"
#include "event_bus/event_queue.h"
#include "event_bus/event_producer.h"
#include "event_bus/event_consumer.h"
#include "metrics/metrics.h"
#include "metrics/metrics_exporter.h"

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

// ============ 初始化限流风控系统
RateLimitService* initRateLimiter(RedisClient*& outRedis,
                                   LocalCache*& outCache,
                                   LocalRateLimiter*& outLimiter,
                                   FeatureExtractorPipeline*& outPipeline,
                                   DecisionEngine*& outEngine,
                                   QPSTracker*& outTracker)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, signalHandler);
#ifndef _WIN32
    signal(SIGTERM, signalHandler);
#else
    signal(SIGBREAK, signalHandler);
#endif

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

    // 风控规则：触发条件必须引用特征提取器【实际产出】的特征名
    // 可用特征名：qps_1min / qps_5min / burst_ratio（Velocity）、
    //             api_sensitivity / time_hour_risk / account_age_days（Static）
    std::vector<RiskRule> riskRules;
    RiskRule rr1; rr1.id = 101; rr1.name = "high_freq_user";
    rr1.priority = 1; rr1.score = 30; rr1.category = "behavior";
    auto cond1 = std::make_shared<ThresholdCondition>("qps_1min", ThresholdCondition::GE, 50);
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

    // 4. 限流器（本地 + Redis 兜底）
    outLimiter = new LocalRateLimiter(redisOk ? outRedis : nullptr, 100);
    outLimiter->start();

    // 5. QPSTracker（风控特征提取优化：本地计数 + Redis 兜底）
    outTracker = new QPSTracker(redisOk ? outRedis : nullptr);
    outTracker->start();

    // 6. 特征提取器
    outPipeline = new FeatureExtractorPipeline();
    outPipeline->add(std::make_unique<StaticFeatureExtractor>());
    // 速度特征器始终添加：QPSTracker 本地滑动窗口即可计数，Redis 只是跨机兜底。
    // 原实现只在 redisOk 时添加，导致降级模式下 qps 特征缺失、风控规则无法触发。
    outPipeline->add(std::make_unique<VelocityFeatureExtractor>(outRedis, outTracker));

    // 7. 决策引擎
    outEngine = new DecisionEngine();

    // 7. 服务�?
    auto* service = new RateLimitService(outCache, outLimiter, outRedis, outPipeline, outEngine);

    std::cout << "=== Rate Limit System Ready ===\n\n";
    return service;
}

// ============ 主函数  
int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) port = std::stoi(argv[1]);

    // 初始化限流系统
    RedisClient* redis = nullptr;
    LocalCache* cache = nullptr;
    LocalRateLimiter* limiter = nullptr;
    FeatureExtractorPipeline* pipeline = nullptr;
    DecisionEngine* engine = nullptr;
    QPSTracker* tracker = nullptr;
    RateLimitService* rateLimiter = initRateLimiter(redis, cache, limiter, pipeline, engine, tracker);

    // === 异步数据回流（EventBus → Metrics）===
    // 主链路同步返回后，RiskEvent 异步入队，后台线程消费并更新指标，绝不阻塞请求线程。
    // 队列有界，满则静默丢弃（背压保护）。这三个对象在 main 栈上，生命周期覆盖整个服务运行期。
    BoundedEventQueue eventQueue(65536);
    EventProducer eventProducer(&eventQueue);
    EventConsumer eventConsumer(&eventQueue, &Metrics::instance());
    eventConsumer.start();
    rateLimiter->setEventProducer(&eventProducer);
    std::cout << "[EventBus] started (capacity=" << eventQueue.capacity() << ")\n";

    // 创建 HTTP 服务（内部有自己的 EventLoop + TcpServer）
    HttpServer server(port, 10);
    g_server = &server;

    // 线程数可从命令行指定: ./server 8080 8
    int threadCount = (argc > 2) ? std::stoi(argv[2]) : std::thread::hardware_concurrency();
    if (threadCount < 1) threadCount = 1;

    // 创建线程池（one loop per thread）
    LoopThreadPool threadPool(server.getLoop());
    threadPool.setThreadCount(threadCount);
    threadPool.start();
    std::cout << "[INFO] ThreadPool started with " << threadCount << " worker threads" << std::endl;
    server.setThreadPool(&threadPool);

    // 创建时间轮（60秒一个轮次 用于连接空闲超时）
    TimerWheel timerWheel(server.getLoop(), 60);
    // 把 TimerWheel 传给 TcpServer，启用连接超时管理
    server.setTimerWheel(&timerWheel);
    server.setConnectionTimeout(60);  // 60秒无请求则断开

    // === 注入限流系统 ===
    server.SetRateLimiter(rateLimiter);

    // 设置静态文件目录
    // server.SetBaseDir("./www");

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
            "{\"total\":%llu,\"allowed\":%llu,\"rejected\":%llu,\"degraded\":%llu}",
            s.total.load(), s.allowed.load(), s.rejected.load(), s.degraded.load());
        rsp->SetContent(buf, "application/json");
        rsp->SetClose(false);
    });

    // API: Prometheus 指标导出（由 EventBus 异步回流更新）
    server.Get("/metrics", [](const HttpRequest& req, HttpResponse* rsp) {
        MetricsExporter exporter;  // 默认绑定 Metrics::instance()
        rsp->SetContent(exporter.toPrometheusText(), "text/plain");
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
    std::cout << "    GET  /metrics         - Prometheus metrics\n";
    std::cout << "  Static files: ./www/\n";
    std::cout << "====================================\n\n";

    server.Listen();

    // Cleanup (不会执行到，signal handler 会退出)
    delete rateLimiter;
    delete engine;
    delete pipeline;
    delete tracker;
    delete limiter;
    delete redis;
    delete cache;
    return 0;
}