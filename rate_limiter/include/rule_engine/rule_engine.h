#pragma once
// ============================================================================
// RuleEngine — 规则引擎（聚合根）
//
// 职责：
//   1. 持有所有规则和黑白名单
//   2. 每次请求进来，构建责任链进行匹配
//   3. 返回 RuleMatchResult
//
// 为什么 RuleEngine 不直接包含匹配逻辑：
//   - 匹配逻辑在 Handler 中，RuleEngine 只负责组装
//   - 符合"单一职责"原则
// ============================================================================

#include "rule.h"
#include "rule_handler.h"
#include "common/common.h"
#include <shared_mutex>
#include <unordered_set>
#include <vector>

class RuleEngine {
public:
    RuleEngine();

    // 批量加载规则（从 LocalCache/ConfigManager 同步而来）
    void loadRules(const std::vector<Rule>& rules);

    // 黑白名单更新
    void setWhitelist(const std::unordered_set<std::string>& wl);
    void setBlacklist(const std::unordered_set<std::string>& bl);

    // 核心：匹配请求对应的规则
    // 返回 RuleMatchResult（包含是否命中黑白名单 + 匹配的限流规则列表）
    RuleMatchResult match(const Request& req) const;

    // 查询
    size_t ruleCount() const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<Rule> rules_;
    std::unordered_set<std::string> whitelist_;
    std::unordered_set<std::string> blacklist_;
};
