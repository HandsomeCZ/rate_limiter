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
#include <vector>
#include <memory>
#include <atomic>
#include <string>

class RedisClient;

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
//   - qps_1min  : 最近 60 秒请求数（需 Redis 查询）
//   - qps_5min  : 最近 300 秒请求数
//   - burst_ratio: 1min / 5min 比值（突发流量检测）
//
// 需要 Redis：查询滑动窗口计数
// ---------------------------------------------------------------------------
class VelocityFeatureExtractor : public IFeatureExtractor {
public:
    explicit VelocityFeatureExtractor(RedisClient* redis);

    const char* name() const override { return "velocity"; }
    std::vector<Feature> extract(const Request& req) override;
    bool requiresExternal() const override { return true; }

private:
    RedisClient* redis_;
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
