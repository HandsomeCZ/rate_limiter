#pragma once
// ============================================================================
// LocalCache — 本地规则缓存层
//
// 职责：
//   1. 缓存规则快照（来自 ConfigManager）
//   2. 1 秒定时刷新
//   3. 提供线程安全的规则/黑白名单查询
//   4. 避免每次请求都要穿透到 RuleEngine（减少 shared_mutex 争抢）
//
// 为什么缓存规则而不是计数？
//   - 计数必须在 Redis 中保证强一致性（多机共享）
//   - 规则变更频率低（分钟级），适合本地缓存
//   - 规则快照很小（< 1000 条），内存开销可忽略
//
// 为什么 1 秒刷新？
//   - 太快（100ms）→ ConfigManager 的 shared_mutex 成为瓶颈
//   - 太慢（10s）→ 规则变更生效太慢
//   - 1 秒是工程上经过验证的平衡点（阿里 Sentinel 默认也是 1s）
// ============================================================================

#include "common/common.h"
#include "rule_engine/rule.h"
#include "rule_engine/rule_engine.h"
#include "decision_engine/risk_rule.h"
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <unordered_set>

class ConfigManager;

class LocalCache {
public:
    explicit LocalCache(ConfigManager* cfgMgr,
                        std::chrono::milliseconds interval
                        = std::chrono::milliseconds(1000));
    ~LocalCache();

    void start();
    void stop();

    // 获取匹配的限流规则列表
    std::vector<Rule> getRules(const Request& req) const;

    // 获取匹配的风控规则列表（NEW）
    std::vector<RiskRule> getRiskRules(const Request& req) const;

    // 黑白名单快速检查
    bool isWhitelisted(const Request& req) const;
    bool isBlacklisted(const Request& req) const;

    // 统计
    uint64_t refreshCount() const { return refreshCount_; }

private:
    void refresh();         // 执行一次全量刷新
    void refreshLoop();     // 后台线程

    ConfigManager* cfgMgr_;
    std::chrono::milliseconds interval_;

    // 本地缓存的快照
    std::vector<Rule> rules_;
    std::vector<RiskRule> riskRules_;                // NEW
    std::unordered_set<std::string> whitelist_;
    std::unordered_set<std::string> blacklist_;

    mutable std::shared_mutex mtx_;
    std::atomic<bool> running_{false};
    std::thread refreshThread_;
    std::atomic<uint64_t> refreshCount_{0};

    // 黑白名单辅助
    static std::string makeKey(const std::string& uid,
                                const std::string& ip,
                                const std::string& api);
    static bool matchSet(const std::unordered_set<std::string>& set,
                         const Request& req);
    static bool ruleMatches(const Rule& rule, const Request& req);
};
