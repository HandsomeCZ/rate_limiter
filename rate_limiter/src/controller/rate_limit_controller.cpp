// Controller实现：参数校验(空值/超长) + 时间戳自动填充 + 纯委托Service(无业务逻辑)
#include "controller/rate_limit_controller.h"
#include "service/rate_limit_service.h"

RateLimitController::RateLimitController(RateLimitService* service)
    : service_(service) {}

Decision RateLimitController::allow(const Request& req) {
    // 参数校验
    if (!validate(req)) {
        return Decision::REJECT;  // 非法请求直接拒绝
    }
    return service_->process(req);
}

Decision RateLimitController::allow(std::string userId,
                                     std::string ip,
                                     std::string api) {
    Request req;
    req.userId      = std::move(userId);
    req.ip          = std::move(ip);
    req.api         = std::move(api);
    req.timestampMs = nowMs();
    return allow(req);
}

Stats& RateLimitController::stats()       { return service_->stats(); }
const Stats& RateLimitController::stats() const { return service_->stats(); }

bool RateLimitController::validate(const Request& req) const {
    // 基本校验：至少有一个有效标识
    if (req.userId.empty() && req.ip.empty()) return false;
    // API 不能为空
    if (req.api.empty()) return false;
    // userId/IP 长度限制（防止超长 key 攻击 Redis）
    if (req.userId.size() > 128 || req.ip.size() > 64) return false;
    return true;
}
