// ConfigManager单例实现：Redis/分片/降级配置 + 限流规则CRUD + 风控规则CRUD + 快照导出
#include "config_manager/config_manager.h"
#include "rule_engine/rule.h"
#include <algorithm>

ConfigManager::ConfigManager() {
    rules_.reserve(256);
    riskRules_.reserve(128);
    whitelist_.reserve(1024);
    blacklist_.reserve(1024);
}

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

// ---- Redis ----
ConfigManager::RedisConfig ConfigManager::getRedisConfig() const {
    std::shared_lock lock(mutex_);
    return redisConfig_;
}

void ConfigManager::setRedisConfig(const RedisConfig& cfg) {
    std::unique_lock lock(mutex_);
    redisConfig_ = cfg;
}

// ---- Shard ----
ConfigManager::ShardConfig ConfigManager::getShardConfig() const {
    std::shared_lock lock(mutex_);
    return shardConfig_;
}

void ConfigManager::setShardConfig(const ShardConfig& cfg) {
    std::unique_lock lock(mutex_);
    shardConfig_ = cfg;
}

// ---- Degrade ----
ConfigManager::DegradeConfig ConfigManager::getDegradeConfig() const {
    std::shared_lock lock(mutex_);
    return degradeConfig_;
}

void ConfigManager::setDegradeConfig(const DegradeConfig& cfg) {
    std::unique_lock lock(mutex_);
    degradeConfig_ = cfg;
}

// ---- Rules ----
std::vector<Rule> ConfigManager::getRules() const {
    std::shared_lock lock(mutex_);
    return rules_;
}

void ConfigManager::setRules(const std::vector<Rule>& rules) {
    std::unique_lock lock(mutex_);
    rules_ = rules;
    std::sort(rules_.begin(), rules_.end(),
              [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
}

void ConfigManager::addRule(const Rule& rule) {
    std::unique_lock lock(mutex_);
    rules_.push_back(rule);
    std::sort(rules_.begin(), rules_.end(),
              [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
}

void ConfigManager::removeRule(uint64_t ruleId) {
    std::unique_lock lock(mutex_);
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
        [ruleId](const Rule& r) { return r.id == ruleId; }), rules_.end());
}

// ---- RiskRules (NEW) ----
std::vector<RiskRule> ConfigManager::getRiskRules() const {
    std::shared_lock lock(mutex_);
    return riskRules_;
}

void ConfigManager::setRiskRules(const std::vector<RiskRule>& rules) {
    std::unique_lock lock(mutex_);
    riskRules_ = rules;
    std::sort(riskRules_.begin(), riskRules_.end(),
              [](const RiskRule& a, const RiskRule& b) { return a.priority < b.priority; });
}

void ConfigManager::addRiskRule(const RiskRule& rule) {
    std::unique_lock lock(mutex_);
    riskRules_.push_back(rule);
    std::sort(riskRules_.begin(), riskRules_.end(),
              [](const RiskRule& a, const RiskRule& b) { return a.priority < b.priority; });
}

void ConfigManager::removeRiskRule(uint64_t ruleId) {
    std::unique_lock lock(mutex_);
    riskRules_.erase(std::remove_if(riskRules_.begin(), riskRules_.end(),
        [ruleId](const RiskRule& r) { return r.id == ruleId; }), riskRules_.end());
}

// ---- ThresholdConfig (NEW) ----
ThresholdConfig ConfigManager::getThresholdConfig() const {
    std::shared_lock lock(mutex_);
    return thresholds_;
}

void ConfigManager::setThresholdConfig(const ThresholdConfig& cfg) {
    std::unique_lock lock(mutex_);
    thresholds_ = cfg;
}

// ---- 黑白名单 ----
std::string ConfigManager::makeSetKey(const std::string& uid,
                                       const std::string& ip,
                                       const std::string& api) {
    return uid + "|" + ip + "|" + api;
}

bool ConfigManager::isWhitelisted(const std::string& userId,
                                   const std::string& ip,
                                   const std::string& api) const {
    std::shared_lock lock(mutex_);
    auto check = [&](const std::string& u, const std::string& i,
                     const std::string& a) {
        return whitelist_.count(makeSetKey(u, i, a)) > 0;
    };
    return check(userId, ip, api)     || check(userId, ip, "")     ||
           check(userId, "", api)     || check("", ip, api)        ||
           check(userId, "", "")      || check("", ip, "")         ||
           check("", "", api);
}

bool ConfigManager::isBlacklisted(const std::string& userId,
                                   const std::string& ip,
                                   const std::string& api) const {
    std::shared_lock lock(mutex_);
    auto check = [&](const std::string& u, const std::string& i,
                     const std::string& a) {
        return blacklist_.count(makeSetKey(u, i, a)) > 0;
    };
    return check(userId, ip, api)     || check(userId, ip, "")     ||
           check(userId, "", api)     || check("", ip, api)        ||
           check(userId, "", "")      || check("", ip, "")         ||
           check("", "", api);
}

void ConfigManager::addWhitelist(const std::string& userId,
                                  const std::string& ip,
                                  const std::string& api) {
    std::unique_lock lock(mutex_);
    whitelist_.insert(makeSetKey(userId, ip, api));
}

void ConfigManager::addBlacklist(const std::string& userId,
                                  const std::string& ip,
                                  const std::string& api) {
    std::unique_lock lock(mutex_);
    blacklist_.insert(makeSetKey(userId, ip, api));
}

ConfigManager::Snapshot ConfigManager::snapshot() const {
    std::shared_lock lock(mutex_);
    return Snapshot{ rules_, riskRules_, thresholds_, whitelist_, blacklist_ };
}
