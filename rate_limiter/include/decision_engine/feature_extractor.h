#pragma once
// ============================================================================
// feature_extractor.h — 特征提取层
//
// IFeatureExtractor 接口 + 两个实现类 + Pipeline
//
// 设计要点：
//   - 每个 Extractor 独立负责一类特征
//   - 单个 Extractor 失败不阻断后续（降级填 defaultSafe）
//   - Pipeline 负责编排所有 Extractor
//   - 新增特征维度只需加一个 Extractor 子类
//
// 为什么不用共享的 FeatureVector 而用 vector<Feature>？
//   - 动态扩展：新增特征不需要修改头文件
//   - 序列化友好：直接遍历 vector 转 JSON
//   - 规则表达式：findFeature(name) 比 switch/case 更灵活
//
// 当然生产环境可以用强类型 FeatureVector + 反射，这里为面试展示
// 动态特征的灵活性。
// ============================================================================

#include "common/common.h"
#include "decision_engine/feature.h"
#include "rate_limiter/local_rate_limiter.h"  // 复用 LocalSlidingWindow
#include <vector>
#include <memory>
#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>

class RedisClient;

// ---------------------------------------------------------------------------
// QPSTracker — 本地 QPS 计数器（用于风控特征提取优化）
//
// 设计：
//   - 复用 LocalSlidingWindow 维护每个 key 的 1min/5min 滑动窗口
//   - 安全区（< 80% 阈值）直接返回本地计数，不查 Redis
//   - 警戒区（>= 80%）查 Redis 精确值
//   - 后台线程每 100ms 异步同步到 Redis
// ---------------------------------------------------------------------------
class QPSTracker {
public:
    explicit QPSTracker(RedisClient* redis = nullptr);
    ~QPSTracker();

    // 记录一次请求
    void record(const std::string& key);

    // 获取指定窗口的计数（本地优先 + Redis 兜底）
    uint32_t getCount(const std::string& key, uint32_t windowSec);

    // 启动后台同步线程
    void start();

    // 停止后台同步线程
    void stop();

    // 统计
    uint64_t localHits() const { return localHits_.load(); }
    uint64_t redisFallbacks() const { return redisFallbacks_.load(); }

private:
    struct WindowPair {
        LocalSlidingWindow window1m;  // 1 分钟窗口（windowSec=60）
        LocalSlidingWindow window5m;  // 5 分钟窗口（windowSec=300）
        std::mutex mtx;

        WindowPair() : window1m(60, 100000), window5m(300, 100000) {}
    };

    void syncToRedis();

    RedisClient* redis_;
    std::unordered_map<std::string, WindowPair> windows_;
    std::mutex mtx_;
    std::thread syncThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> localHits_{0};
    std::atomic<uint64_t> redisFallbacks_{0};
    static constexpr uint32_t SYNC_INTERVAL_MS = 100;
    static constexpr uint32_t THRESHOLD_1M = 1000;  // 1min 阈值
    static constexpr uint32_t THRESHOLD_5M = 5000;  // 5min 阈值
};

// ---------------------------------------------------------------------------
// IFeatureExtractor — 特征提取器接口
// ---------------------------------------------------------------------------
class IFeatureExtractor {
public:
    virtual ~IFeatureExtractor() = default;

    // Extractor 名称（调试/监控用）
    virtual const char* name() const = 0;

    // 从 Request 提取特征
    // @return 提取到的 Feature 列表（空 = 无特征产出）
    virtual std::vector<Feature> extract(const Request& req) = 0;

    // 是否依赖外部服务（Redis / 第三方 API）
    virtual bool requiresExternal() const = 0;
};

// ---------------------------------------------------------------------------
// StaticFeatureExtractor — 纯本地提取（零外部依赖）
//
// 提取的特征：
//   - api_sensitivity : API 敏感度（0-3）
//   - time_hour       : 请求时间-小时（凌晨操作风险更高）
//   - account_age_days: 账号年龄（基于 userId hash 模拟）
// ---------------------------------------------------------------------------
class StaticFeatureExtractor : public IFeatureExtractor {
public:
    const char* name() const override { return "static"; }
    std::vector<Feature> extract(const Request& req) override;
    bool requiresExternal() const override { return false; }

private:
    int getApiSensitivity(const std::string& api);
};

// ---------------------------------------------------------------------------
// VelocityFeatureExtractor — 请求频率特征
//
// 提取的特征：
//   - qps_1min  : 最近 60 秒请求数（本地计数 + Redis 兜底）
//   - qps_5min  : 最近 300 秒请求数
//   - burst_ratio: 1min / 5min 比值（突发流量检测）
//
// 优化：使用 QPSTracker 实现本地优先，减少 90% Redis 调用
// ---------------------------------------------------------------------------
class VelocityFeatureExtractor : public IFeatureExtractor {
public:
    explicit VelocityFeatureExtractor(RedisClient* redis, QPSTracker* tracker = nullptr);

    const char* name() const override { return "velocity"; }
    std::vector<Feature> extract(const Request& req) override;
    bool requiresExternal() const override { return true; }

private:
    RedisClient* redis_;
    QPSTracker* tracker_;
    int getRecentCount(const std::string& key, uint32_t windowSec);
};

// ---------------------------------------------------------------------------
// FeatureExtractorPipeline — 多 Extractor 编排器
//
// 职责：
//   1. 顺序执行所有 Extractor
//   2. 单个 Extractor 失败 → 该 Extractor 的特征被跳过（不阻断）
//   3. 汇总所有 Feature 到统一 vector
//   4. 记录降级次数
//
// 为什么顺序而不是并行？
//   - 避免线程同步开销（每个 Extractor < 1ms）
//   - 简单可靠，延迟可预测
//   - 面试场景下顺序执行已足够清晰
// ---------------------------------------------------------------------------
class FeatureExtractorPipeline {
public:
    // 添加 Extractor（按添加顺序执行）
    void add(std::unique_ptr<IFeatureExtractor> ext);

    // 执行全量提取，返回汇总 Feature 列表
    std::vector<Feature> extractAll(const Request& req);

    // 统计
    int degraded() const { return degraded_.load(); }

private:
    std::vector<std::unique_ptr<IFeatureExtractor>> extractors_;
    std::atomic<int> degraded_{0};
};