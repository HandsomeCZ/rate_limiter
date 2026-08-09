#pragma once
// ============================================================================
// risk_event.h — 风控事件（不可变值对象）
//
// 每次 Service::process() 决策完成后构造一个 RiskEvent 异步上报。
// 事件包含完整的决策上下文：谁、做了什么操作、结果是什么、为什么。
//
// 设计要点：
//   - 全部字段值语义（可拷贝、可移动），无指针，适合跨线程传递
//   - 自带时间戳（构造时自动填充），消费者不需要再取时间
//   - requestId 支持链路追踪（上下游关联）
// ============================================================================

#include "common/common.h"
#include <string>
#include <vector>
#include <sstream>
#include <chrono>

struct RiskEvent {
    // ---- 请求标识 ----
    std::string requestId;      // 唯一请求 ID（UUID 或 traceId）
    uint64_t    timestampMs;    // 事件发生时间

    // ---- 主体身份 ----
    std::string userId;
    std::string ip;
    std::string api;

    // ---- 决策结果 ----
    Decision    decision;       // ALLOW / LIMIT / CHALLENGE / REJECT
    int         riskScore;      // 风控评分
    int         limitQuota;     // LIMIT 时的建议配额

    // ---- 触发规则 ----
    std::vector<uint64_t> triggeredRuleIds;
    std::string           reason;       // 人类可读原因

    // ---- 耗时（微秒） ----
    uint64_t    latencyUs;

    // 工厂方法：从决策上下文快速构造
    static RiskEvent make(
        const std::string& requestId,
        const Request& req,
        Decision decision,
        int riskScore,
        int limitQuota,
        const std::vector<uint64_t>& ruleIds,
        const std::string& reason,
        uint64_t latencyUs)
    {
        RiskEvent e;
        e.requestId    = requestId;
        e.timestampMs  = (req.timestampMs > 0) ? req.timestampMs : nowMs();
        e.userId       = req.userId;
        e.ip           = req.ip;
        e.api          = req.api;
        e.decision     = decision;
        e.riskScore    = riskScore;
        e.limitQuota   = limitQuota;
        e.triggeredRuleIds = ruleIds;
        e.reason       = reason;
        e.latencyUs    = latencyUs;
        return e;
    }

    // 序列化为一行 JSON（方便写日志 / Kafka）
    std::string toJson() const {
        std::ostringstream oss;
        oss << "{"
            << "\"ts\":" << timestampMs
            << ",\"uid\":\"" << userId << "\""
            << ",\"ip\":\"" << ip << "\""
            << ",\"api\":\"" << api << "\""
            << ",\"decision\":\"" << decisionStr(decision) << "\""
            << ",\"score\":" << riskScore
            << ",\"latency\":" << latencyUs
            << ",\"reason\":\"" << reason << "\""
            << "}";
        return oss.str();
    }
};
