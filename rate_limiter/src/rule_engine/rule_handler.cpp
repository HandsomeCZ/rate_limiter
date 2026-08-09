// 责任链Handler实现：WhitelistHandler → BlacklistHandler → DimensionHandler + 工厂函数
#include "rule_engine/rule_handler.h"

// ============================================================================
// WhitelistHandler
// ============================================================================
void WhitelistHandler::doHandle(const Request& req, RuleMatchResult& result) {
    auto check = [&](const std::string& u, const std::string& i,
                     const std::string& a) {
        return whitelist_.count(u + "|" + i + "|" + a) > 0;
    };
    if (check(req.userId, req.ip, req.api) || check(req.userId, req.ip, "") ||
        check(req.userId, "", req.api)     || check("", req.ip, req.api)    ||
        check(req.userId, "", "")          || check("", req.ip, "")         ||
        check("", "", req.api)) {
        result.hitWhitelist = true;
    }
}

// ============================================================================
// BlacklistHandler
// ============================================================================
void BlacklistHandler::doHandle(const Request& req, RuleMatchResult& result) {
    auto check = [&](const std::string& u, const std::string& i,
                     const std::string& a) {
        return blacklist_.count(u + "|" + i + "|" + a) > 0;
    };
    if (check(req.userId, req.ip, req.api) || check(req.userId, req.ip, "") ||
        check(req.userId, "", req.api)     || check("", req.ip, req.api)    ||
        check(req.userId, "", "")          || check("", req.ip, "")         ||
        check("", "", req.api)) {
        result.hitBlacklist = true;
    }
}

// ============================================================================
// DimensionHandler
// ============================================================================
bool DimensionHandler::ruleMatches(const Rule& rule, const Request& req) {
    if (!rule.enabled) return false;
    if (!rule.matchUserId.empty() && rule.matchUserId != req.userId) return false;
    if (!rule.matchIp.empty()     && rule.matchIp     != req.ip)      return false;
    if (!rule.matchApi.empty()    && rule.matchApi    != req.api)     return false;
    return true;
}

void DimensionHandler::doHandle(const Request& req, RuleMatchResult& result) {
    result.timestampMs = nowMs();
    for (const auto& rule : rules_) {
        if (ruleMatches(rule, req)) {
            result.matchedRules.push_back(rule);
        }
    }
}

// ============================================================================
// 责任链工厂
// ============================================================================
std::unique_ptr<RuleHandler> buildRuleChain(
    const std::unordered_set<std::string>& whitelist,
    const std::unordered_set<std::string>& blacklist,
    const std::vector<Rule>& rules) {

    auto wl = std::make_unique<WhitelistHandler>(whitelist);
    auto bl = std::make_unique<BlacklistHandler>(blacklist);
    auto dim = std::make_unique<DimensionHandler>(rules);

    // 链: Whitelist → Blacklist → Dimension
    bl->setNext(std::move(dim));
    wl->setNext(std::move(bl));

    return wl;
}
