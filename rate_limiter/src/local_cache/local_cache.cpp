// LocalCache实现：规则/黑白名单快照 + 1s定时刷新(50ms分片检查退出) + shared_mutex读写分离
#include "local_cache/local_cache.h"
#include "config_manager/config_manager.h"
#include <algorithm>

LocalCache::LocalCache(ConfigManager* cfgMgr,
                       std::chrono::milliseconds interval)
    : cfgMgr_(cfgMgr), interval_(interval) {
    refresh();  // 构造时立即加载一次
}

LocalCache::~LocalCache() { stop(); }

void LocalCache::start() {
    if (running_.exchange(true)) return;

    refreshThread_ = std::thread([this] { refreshLoop(); });
    printf("[LocalCache] started, interval=%lldms\n",
           (long long)interval_.count());
}

void LocalCache::stop() {
    running_.store(false);
    if (refreshThread_.joinable()) refreshThread_.join();
}

void LocalCache::refresh() {
    auto snap = cfgMgr_->snapshot();

    std::unique_lock lock(mtx_);
    rules_      = std::move(snap.rules);
    riskRules_  = std::move(snap.riskRules);
    whitelist_  = std::move(snap.whitelist);
    blacklist_  = std::move(snap.blacklist);
    refreshCount_++;
}

void LocalCache::refreshLoop() {
    while (running_.load(std::memory_order_acquire)) {
        // 分步睡眠（每 50ms 检查一次是否停止）
        for (uint32_t i = 0; i < interval_.count() && running_.load(); i += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!running_.load()) break;
        refresh();
    }
}

// ============================================================================
// 规则匹配（从本地缓存）
// ============================================================================

bool LocalCache::ruleMatches(const Rule& rule, const Request& req) {
    if (!rule.enabled) return false;
    if (!rule.matchUserId.empty() && rule.matchUserId != req.userId) return false;
    if (!rule.matchIp.empty()     && rule.matchIp     != req.ip)      return false;
    if (!rule.matchApi.empty()    && rule.matchApi    != req.api)     return false;
    return true;
}

std::vector<Rule> LocalCache::getRules(const Request& req) const {
    std::shared_lock lock(mtx_);
    std::vector<Rule> matched;
    matched.reserve(4);
    for (const auto& r : rules_) {
        if (ruleMatches(r, req)) matched.push_back(r);
    }
    return matched;
}

// ============================================================================
// 风控规则匹配（NEW）
// ============================================================================
static bool riskRuleMatches(const RiskRule& rule, const Request& /*req*/) {
    // 风控规则不像限流规则那样做 userId/ip/api 精确匹配。
    // 风控规则依赖 FeatureVector 做条件判断（IRuleCondition.evaluate）。
    // 这里做第一层粗筛：只返回启用的规则。
    // 精确判断在 DecisionEngine 中由 IRuleCondition 完成。
    return rule.enabled;
}

std::vector<RiskRule> LocalCache::getRiskRules(const Request& req) const {
    std::shared_lock lock(mtx_);
    std::vector<RiskRule> matched;
    matched.reserve(riskRules_.size());
    for (const auto& r : riskRules_) {
        if (riskRuleMatches(r, req)) matched.push_back(r);
    }
    return matched;
}

// ============================================================================
// 黑白名单
// ============================================================================

std::string LocalCache::makeKey(const std::string& uid,
                                 const std::string& ip,
                                 const std::string& api) {
    return uid + "|" + ip + "|" + api;
}

bool LocalCache::matchSet(const std::unordered_set<std::string>& set,
                           const Request& req) {
    auto check = [&](const std::string& u, const std::string& i,
                     const std::string& a) {
        return set.count(makeKey(u, i, a)) > 0;
    };
    return check(req.userId, req.ip, req.api) || check(req.userId, req.ip, "") ||
           check(req.userId, "", req.api)     || check("", req.ip, req.api)    ||
           check(req.userId, "", "")          || check("", req.ip, "")         ||
           check("", "", req.api);
}

bool LocalCache::isWhitelisted(const Request& req) const {
    std::shared_lock lock(mtx_);
    return matchSet(whitelist_, req);
}

bool LocalCache::isBlacklisted(const Request& req) const {
    std::shared_lock lock(mtx_);
    return matchSet(blacklist_, req);
}
