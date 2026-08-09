#pragma once
// ============================================================================
// rule.h — 限流规则数据结构（Rule + RuleMatchResult）
//
// 与风控 RiskRule 的区别：
//   - Rule 回答"窗口内还能来多少次？"（频率控制）
//   - RiskRule 回答"这个请求有多大风险？"（风险评分）
// 两者串联：风控在前判定风险等级，限流在后执行频率控制
// ============================================================================

#include "common/common.h"
#include <string>

// ---------------------------------------------------------------------------
// 单条限流规则
// ---------------------------------------------------------------------------
struct Rule {
    uint64_t    id       = 0;
    std::string name;
    LimitType   limitType = LimitType::USER;
    Algorithm   algorithm = Algorithm::SLIDING_WINDOW;
    uint32_t    windowSec = 1;        // 窗口大小（秒）
    uint32_t    maxReq    = 100;      // 窗口内允许的最大请求数
    int32_t     priority  = 0;       // 越小越优先
    bool        enabled   = true;

    // 匹配条件（空 = 匹配所有）
    std::string matchUserId;
    std::string matchIp;
    std::string matchApi;
};

// ---------------------------------------------------------------------------
// 规则匹配结果（责任链产出）
// ---------------------------------------------------------------------------
struct RuleMatchResult {
    bool  hitWhitelist = false;      // 命中白名单 → 无条件放行
    bool  hitBlacklist = false;      // 命中黑名单 → 无条件拒绝
    std::vector<Rule> matchedRules;  // 命中的限流规则（按优先级升序）
    uint64_t timestampMs = 0;       // 匹配时间
};
