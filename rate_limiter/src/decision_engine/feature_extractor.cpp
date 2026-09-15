// 特征提取器实现：StaticExtractor(本地) + VelocityExtractor(Redis) + Pipeline编排(单失败不阻断)
#include "decision_engine/feature_extractor.h"
#include "redis_client/redis_client.h"
#include <unordered_map>
#include <cmath>

// ============================================================================
// QPSTracker — 本地 QPS 计数器（复用 LocalSlidingWindow）
// ============================================================================

QPSTracker::QPSTracker(RedisClient* redis)
    : redis_(redis) {}

QPSTracker::~QPSTracker() { stop(); }

void QPSTracker::record(const std::string& key) {
    uint64_t nowMs = ::nowMs();
    std::lock_guard<std::mutex> lock(mtx_);
    auto& pair = windows_[key];
    pair.window1m.checkAndRecord(nowMs);
    pair.window5m.checkAndRecord(nowMs);
}

uint32_t QPSTracker::getCount(const std::string& key, uint32_t windowSec) {
    uint64_t nowMs = ::nowMs();
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = windows_.find(key);
    if (it == windows_.end()) return 0;

    LocalSlidingWindow* window = nullptr;
    uint32_t threshold = 0;
    if (windowSec == 60) {
        window = &it->second.window1m;
        threshold = THRESHOLD_1M;
    } else {
        window = &it->second.window5m;
        threshold = THRESHOLD_5M;
    }

    // 1. 获取本地计数
    uint32_t localCount = static_cast<uint32_t>(window->count(nowMs));

    // 2. 安全区（< 80%）：直接返回本地计数
    uint32_t safeThreshold = static_cast<uint32_t>(threshold * 0.8);
    if (localCount < safeThreshold) {
        localHits_.fetch_add(1, std::memory_order_relaxed);
        return localCount;
    }

    // 3. 警戒区（>= 80%）：查 Redis 精确值（只读，不写入；Redis 计数由同步线程维护）
    if (redis_ && redis_->isAvailable()) {
        redisFallbacks_.fetch_add(1, std::memory_order_relaxed);
        int64_t redisCount = redis_->slidingWindowCount(key, windowSec);
        return (redisCount >= 0) ? static_cast<uint32_t>(redisCount) : localCount;
    }

    // 4. Redis 不可用：返回本地计数（降级）
    return localCount;
}

void QPSTracker::start() {
    if (running_.exchange(true) || !redis_) return;
    syncThread_ = std::thread(&QPSTracker::syncToRedis, this);
}

void QPSTracker::stop() {
    if (!running_.exchange(false)) return;
    if (syncThread_.joinable()) syncThread_.join();
}

void QPSTracker::syncToRedis() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(SYNC_INTERVAL_MS));
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [key, pair] : windows_) {
            // 只同步增量，避免每个周期重复累加全量计数（原 bug）
            uint32_t delta1m = static_cast<uint32_t>(pair.window1m.takePending());
            uint32_t delta5m = static_cast<uint32_t>(pair.window5m.takePending());
            if (delta1m > 0) {
                // 写入与 getCount 兜底校验相同的 ZSet，统一滑动窗口语义
                redis_->slidingWindowRecord(key, 60, delta1m);
            }
            if (delta5m > 0) {
                std::string key5m = key + ":5min";
                redis_->slidingWindowRecord(key5m, 300, delta5m);
            }
        }
    }
}

// ============================================================================
// StaticFeatureExtractor
// ============================================================================

int StaticFeatureExtractor::getApiSensitivity(const std::string& api) {
    // 内置映射表（生产环境从配置中心加载）
    static const std::unordered_map<std::string, int> map = {
        {"/api/v1/order",        3},  // 下单 - 高敏感
        {"/api/v1/payment",      3},  // 支付 - 高敏感
        {"/api/v1/transfer",     3},  // 转账 - 高敏感
        {"/api/v1/user/info",    1},  // 查信息 - 低敏感
        {"/api/v1/product/list", 0},  // 浏览 - 无敏感
    };

    // 前缀匹配（如 /api/v1/order/create 也匹配）
    for (const auto& kv : map) {
        if (api.find(kv.first) == 0) return kv.second;
    }
    return 0;  // 未知 API 默认不敏感
}

std::vector<Feature> StaticFeatureExtractor::extract(const Request& req) {
    std::vector<Feature> result;
    result.reserve(3);

    // ---- 特征1: API 敏感度 ----
    int sensitivity = getApiSensitivity(req.api);
    result.push_back(Feature::make("api_sensitivity", sensitivity,
                                    2.0, name()));
    // weight=2.0: API 敏感度是强信号

    // ---- 特征2: 请求时间 ----
    // 凌晨 0-5 点操作风险更高
    uint64_t now = (req.timestampMs > 0) ? req.timestampMs : nowMs();
    time_t t = static_cast<time_t>(now / 1000);
    struct tm tm_buf;
    int hour = 12;  // 默认中午
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &t) == 0) hour = tm_buf.tm_hour;
#else
    if (localtime_r(&t, &tm_buf)) hour = tm_buf.tm_hour;
#endif
    int hourRisk = (hour >= 0 && hour <= 5) ? 1 : 0;
    result.push_back(Feature::make("time_hour_risk", hourRisk,
                                    1.5, name()));

    // ---- 特征3: 新账号检测 ----
    // 简化：userId hash 最后 3 位模拟账号年龄
    // 生产环境从数据库/Redis 查询
    size_t hashVal = std::hash<std::string>{}(req.userId);
    int days = static_cast<int>(hashVal % 365 + 1);  // 1~365
    int newAccountRisk = (days < 7) ? 1 : 0;
    result.push_back(Feature::make("account_age_days", days,
                                    0.0, name()));
    // weight=0: 不直接参与评分，仅供规则条件判断
    (void)newAccountRisk;

    return result;
}

// ============================================================================
// VelocityFeatureExtractor
// ============================================================================

VelocityFeatureExtractor::VelocityFeatureExtractor(RedisClient* redis, QPSTracker* tracker)
    : redis_(redis), tracker_(tracker) {}

int VelocityFeatureExtractor::getRecentCount(const std::string& key,
                                              uint32_t windowSec) {
    // 优先使用 QPSTracker（本地 + Redis 兜底）
    if (tracker_) {
        return static_cast<int>(tracker_->getCount(key, windowSec));
    }
    // 降级：直接查 Redis
    if (!redis_ || !redis_->isAvailable()) return 0;
    int64_t count = redis_->fixedWindowIncr(key, windowSec);
    return (count >= 0) ? static_cast<int>(count) : 0;
}

std::vector<Feature> VelocityFeatureExtractor::extract(const Request& req) {
    std::vector<Feature> result;
    result.reserve(3);

    // 构造 Redis key（user 维度）
    std::string key1min = KeyBuilder::build(LimitType::USER,
                                             req.userId, req.api);
    std::string key5min = key1min + ":5min";

    // 记录本次请求到 QPSTracker
    if (tracker_) {
        tracker_->record(key1min);
    }

    int qps1 = getRecentCount(key1min, 60);
    int qps5 = getRecentCount(key5min, 300);

    // ---- 特征1: 1 分钟 QPS ----
    result.push_back(Feature::make("qps_1min", qps1, 0.3, name()));
    // weight=0.3: QPS 是中等信号

    // ---- 特征2: 5 分钟 QPS ----
    result.push_back(Feature::make("qps_5min", qps5, 0.1, name()));

    // ---- 特征3: 突发比例 ----
    // burst_ratio = qps_1min / max(qps_5min/5, 1)
    // 即 1 分钟平均 QPS 与 5 分钟平均 QPS 的比值
    // 比值 > 3 说明突然暴涨（可能是攻击）
    double avg5minQps = qps5 / 5.0;
    if (avg5minQps < 1.0) avg5minQps = 1.0;
    double burst = qps1 / avg5minQps;
    result.push_back(Feature::make("burst_ratio", burst, 1.5, name()));
    // weight=1.5: 突发流量是强信号

    return result;
}

// ============================================================================
// FeatureExtractorPipeline
// ============================================================================

void FeatureExtractorPipeline::add(std::unique_ptr<IFeatureExtractor> ext) {
    extractors_.push_back(std::move(ext));
}

std::vector<Feature> FeatureExtractorPipeline::extractAll(const Request& req) {
    std::vector<Feature> allFeatures;

    for (auto& ext : extractors_) {
        try {
            auto features = ext->extract(req);
            // 合并到汇总列表
            allFeatures.insert(allFeatures.end(),
                               std::make_move_iterator(features.begin()),
                               std::make_move_iterator(features.end()));
        } catch (...) {
            // 单个 Extractor 异常不阻断流水线
            degraded_++;
        }
    }

    return allFeatures;
}