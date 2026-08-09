#pragma once
// ============================================================================
// rule_handler.h — 责任链模式处理器
//
// 设计：
//   为什么用责任链而不是简单的 for 循环 + if/else？
//     - 每种检查逻辑独立封装，方便扩展（新增检查维度只需新增 Handler）
//     - 链的顺序体现优先级：白名单 > 黑名单 > 维度规则
//     - 面试中责任链是高频考点
//
//   责任链顺序（不可变）：
//     WhitelistHandler  → 命中则短路，直接 ALLOW
//     BlacklistHandler  → 命中则短路，直接 REJECT
//     DimensionHandler  → 收集匹配的限流规则
// ============================================================================

#include "rule.h"
#include "common/common.h"
#include <memory>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// 抽象处理器
// ---------------------------------------------------------------------------
class RuleHandler {
public:
    virtual ~RuleHandler() = default;

    // 设置下一个处理器
    void setNext(std::unique_ptr<RuleHandler> next) { next_ = std::move(next); }

    // 模板方法：执行自身逻辑，然后交给下一个
    void handle(const Request& req, RuleMatchResult& result) {
        doHandle(req, result);
        if (next_) {
            next_->handle(req, result);
        }
    }

protected:
    // 子类实现：判断并修改 result
    virtual void doHandle(const Request& req, RuleMatchResult& result) = 0;

private:
    std::unique_ptr<RuleHandler> next_;
};

// ---------------------------------------------------------------------------
// 白名单处理器 — 命中后短路（不再执行后续 handler）
// ---------------------------------------------------------------------------
class WhitelistHandler : public RuleHandler {
public:
    explicit WhitelistHandler(const std::unordered_set<std::string>& whitelist)
        : whitelist_(whitelist) {}

protected:
    void doHandle(const Request& req, RuleMatchResult& result) override;

private:
    const std::unordered_set<std::string>& whitelist_;
};

// ---------------------------------------------------------------------------
// 黑名单处理器 — 命中后短路
// ---------------------------------------------------------------------------
class BlacklistHandler : public RuleHandler {
public:
    explicit BlacklistHandler(const std::unordered_set<std::string>& blacklist)
        : blacklist_(blacklist) {}

protected:
    void doHandle(const Request& req, RuleMatchResult& result) override;

private:
    const std::unordered_set<std::string>& blacklist_;
};

// ---------------------------------------------------------------------------
// 维度规则处理器 — 收集匹配的限流规则
// ---------------------------------------------------------------------------
class DimensionHandler : public RuleHandler {
public:
    explicit DimensionHandler(const std::vector<Rule>& rules)
        : rules_(rules) {}

protected:
    void doHandle(const Request& req, RuleMatchResult& result) override;

    static bool ruleMatches(const Rule& rule, const Request& req);

private:
    const std::vector<Rule>& rules_;
};

// ---------------------------------------------------------------------------
// 责任链工厂
// ---------------------------------------------------------------------------
std::unique_ptr<RuleHandler> buildRuleChain(
    const std::unordered_set<std::string>& whitelist,
    const std::unordered_set<std::string>& blacklist,
    const std::vector<Rule>& rules);
