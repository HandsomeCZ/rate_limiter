// RuleEngine实现：规则加载 + 责任链组装 + shared_mutex并发保护
#include "rule_engine/rule_engine.h"
#include <algorithm>

RuleEngine::RuleEngine() {
    rules_.reserve(256);
    whitelist_.reserve(1024);
    blacklist_.reserve(1024);
}

void RuleEngine::loadRules(const std::vector<Rule>& rules) {
    std::unique_lock lock(mutex_);
    rules_ = rules;
    std::sort(rules_.begin(), rules_.end(),
              [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
}

void RuleEngine::setWhitelist(const std::unordered_set<std::string>& wl) {
    std::unique_lock lock(mutex_);
    whitelist_ = wl;
}

void RuleEngine::setBlacklist(const std::unordered_set<std::string>& bl) {
    std::unique_lock lock(mutex_);
    blacklist_ = bl;
}

RuleMatchResult RuleEngine::match(const Request& req) const {
    // 读取共享状态（whitelist_/blacklist_/rules_）前必须加共享锁，
    // 否则与 loadRules/setWhitelist/setBlacklist 的写入构成数据竞争。
    std::shared_lock lock(mutex_);

    RuleMatchResult result;
    result.timestampMs = nowMs();

    // 构建责任链并执行
    auto chain = buildRuleChain(whitelist_, blacklist_, rules_);
    chain->handle(req, result);

    return result;
}

size_t RuleEngine::ruleCount() const {
    std::shared_lock lock(mutex_);
    return rules_.size();
}
