#pragma once
// ============================================================================
// RateLimitController — 接口层 / 控制器
//
// 职责：
//   1. 暴露 allow(Request) → Decision 接口
//   2. 参数校验（userId/ip/api 格式检查）
//   3. 补充时间戳
//   4. 委托给 Service 层处理
//   5. 日志/监控埋点（面试中说明"实际会接入 CAT/Prometheus"）
//
// 为什么 Controller 这么薄？
//   - 接口层不应该有业务逻辑
//   - 让它只做"翻译和转发"，核心逻辑在 Service 中
//   - 符合 MVC/DDD 分层思想
//
// 为什么不用 gRPC / HTTP server？
//   - 面试关注核心算法和架构，网络层是加分项但非必须
//   - 可以说明"生产环境会 wrap 成 gRPC Service"
// ============================================================================

#include "common/common.h"
#include <memory>

class RateLimitService;

class RateLimitController {
public:
    explicit RateLimitController(RateLimitService* service);
    ~RateLimitController() = default;

    // 核心接口：判断请求是否允许
    Decision allow(const Request& req);

    // 带时间戳自动填充的便捷接口
    Decision allow(std::string userId, std::string ip, std::string api);

    // 获取统计
    Stats& stats();
    const Stats& stats() const;

private:
    bool validate(const Request& req) const;

    RateLimitService* service_;
};
