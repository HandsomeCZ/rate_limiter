#pragma once
// ============================================================================
// ConfigManager — 配置管理模块（单例）
//
// 职责：
//   1. 管理 Redis 连接配置
//   2. 管理限流规则全集
//   3. 管理黑白名单
//   4. 管理分片/降级配置
//   5. 支持配置热加载
//
// 设计：
//   为什么用单例而不是依赖注入？
//     - 配置是全局状态，注入到每个组件会造成构造参数爆炸
//     - 配置通过 shared_mutex 保证读多写少场景的并发安全
//     - 面试场景下单例已足够清晰
//
//   为什么不是 etcd/consul 实时 watch？
//     - 定时 pull (1s) 比 push (watch) 更简单可靠
//     - 1s 延迟对规则变更足够（线上规则不会期待毫秒级生效）
// ============================================================================

#include "common/common.h"
#include "decision_engine/risk_rule.h"
#include "decision_engine/decision_engine.h"
#include <shared_mutex>
#include <unordered_set>
#include <vector>
#include <string>

// 前向声明
struct Rule;

class ConfigManager {
public:
    // ---- Redis 配置 ----
    struct RedisConfig {
        std::string host       = "127.0.0.1";
        int         port       = 6379;
        int         poolSize   = 32;
        int         connTimeoutMs = 200;
    };

    // ---- 分片配置 ----
    struct ShardConfig {
        uint32_t count       = 16;   // 分片数
        uint32_t hotThreshold = 500; // 超过此 QPS 视为热点
    };

    // ---- 降级配置 ----
    struct DegradeConfig {
        bool failOpen      = true;   // Redis 故障时放行
        int  circuitBreakerThreshold = 10;  // 连续失败次数触发熔断
        int  circuitBreakerTimeoutMs = 5000; // 熔断恢复时间
    };

    static ConfigManager& instance();

    // ---- Redis 配置 ----
    RedisConfig getRedisConfig() const;
    void        setRedisConfig(const RedisConfig& cfg);

    // ---- 分片配置 ----
    ShardConfig getShardConfig() const;
    void        setShardConfig(const ShardConfig& cfg);

    // ---- 降级配置 ----
    DegradeConfig getDegradeConfig() const;
    void          setDegradeConfig(const DegradeConfig& cfg);

    // ---- 限流规则管理 ----
    std::vector<Rule> getRules() const;
    void              setRules(const std::vector<Rule>& rules);
    void              addRule(const Rule& rule);
    void              removeRule(uint64_t ruleId);

    // ---- 风控规则管理（NEW） ----
    std::vector<RiskRule> getRiskRules() const;
    void                  setRiskRules(const std::vector<RiskRule>& rules);
    void                  addRiskRule(const RiskRule& rule);
    void                  removeRiskRule(uint64_t ruleId);

    // ---- 风控阈值配置（NEW） ----
    ThresholdConfig   getThresholdConfig() const;
    void              setThresholdConfig(const ThresholdConfig& cfg);

    // ---- 黑白名单 ----
    bool isWhitelisted(const std::string& userId,
                       const std::string& ip,
                       const std::string& api) const;
    bool isBlacklisted(const std::string& userId,
                       const std::string& ip,
                       const std::string& api) const;

    void addWhitelist(const std::string& userId,
                      const std::string& ip,
                      const std::string& api);
    void addBlacklist(const std::string& userId,
                      const std::string& ip,
                      const std::string& api);

    // ---- 快照 ----
    // 供 LocalCache 定时拉取使用（返回引用需注意线程安全，这里返回副本）
    struct Snapshot {
        std::vector<Rule>           rules;
        std::vector<RiskRule>       riskRules;       // NEW
        ThresholdConfig             thresholds;       // NEW
        std::unordered_set<std::string> whitelist;
        std::unordered_set<std::string> blacklist;
    };
    Snapshot snapshot() const;

private:
    ConfigManager();
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    static std::string makeSetKey(const std::string& uid,
                                   const std::string& ip,
                                   const std::string& api);

    mutable std::shared_mutex mutex_;
    RedisConfig    redisConfig_;
    ShardConfig    shardConfig_;
    DegradeConfig  degradeConfig_;
    ThresholdConfig thresholds_;                   // NEW
    std::vector<Rule> rules_;
    std::vector<RiskRule> riskRules_;              // NEW
    std::unordered_set<std::string> whitelist_;
    std::unordered_set<std::string> blacklist_;
};
