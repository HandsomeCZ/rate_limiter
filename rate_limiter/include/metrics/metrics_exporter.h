#pragma once
// ============================================================================
// metrics_exporter.h — 指标导出器
//
// 职责：
//   1. 将 Metrics 中的指标格式化为 Prometheus text format
//   2. 预留 HTTP endpoint 接口（/metrics）
//   3. 预留 PushGateway 推送接口
//
// 为什么单独抽 Exporter？
//   - Metrics 只负责统计，Exporter 只负责格式化/导出
//   - 后续替换为 OpenTelemetry 只需改 Exporter
//   - 可以同时存在多个 Exporter（Prometheus + 日志文件 + Kafka）
//
// Prometheus 格式示例：
//   # HELP risk_total_requests Total requests
//   # TYPE risk_total_requests counter
//   risk_total_requests 100000
//   risk_allow_count 82000
//   risk_reject_count 18000
//   risk_intercept_rate 0.18
// ============================================================================

#include "metrics/metrics.h"
#include "metrics/aggregator.h"
#include <string>
#include <sstream>
#include <functional>

class MetricsExporter {
public:
    explicit MetricsExporter(Metrics* metrics = nullptr);

    // 生成 Prometheus text format（一行一个指标）
    std::string toPrometheusText() const;

    // 生成 JSON 格式（方便调试）
    std::string toJson() const;

    // 设置自定义标签（如 instance="10.0.0.1", job="risk-engine"）
    void setLabel(const std::string& key, const std::string& value);

private:
    Metrics* metrics_;
    std::string labels_;  // Prometheus labels（逗号分隔）
};

// Prometheus 格式示例输出
inline std::string MetricsExporter::toPrometheusText() const {
    if (!metrics_) return "";

    std::ostringstream oss;

    // ---- 基础计数器 ----
    oss << "# HELP risk_requests_total Total requests processed\n";
    oss << "# TYPE risk_requests_total counter\n";
    oss << "risk_requests_total" << labels_ << " " << metrics_->total() << "\n\n";

    oss << "# HELP risk_allow_total Total allowed requests\n";
    oss << "# TYPE risk_allow_total counter\n";
    oss << "risk_allow_total" << labels_ << " " << metrics_->allowed() << "\n\n";

    oss << "# HELP risk_limit_total Total limited requests\n";
    oss << "# TYPE risk_limit_total counter\n";
    oss << "risk_limit_total" << labels_ << " " << metrics_->limited() << "\n\n";

    oss << "# HELP risk_challenge_total Total challenged requests\n";
    oss << "# TYPE risk_challenge_total counter\n";
    oss << "risk_challenge_total" << labels_ << " " << metrics_->challenged() << "\n\n";

    oss << "# HELP risk_reject_total Total rejected requests\n";
    oss << "# TYPE risk_reject_total counter\n";
    oss << "risk_reject_total" << labels_ << " " << metrics_->rejected() << "\n\n";

    // ---- 拦截率（gauge） ----
    oss << "# HELP risk_intercept_rate Reject ratio in sliding window\n";
    oss << "# TYPE risk_intercept_rate gauge\n";
    oss << "risk_intercept_rate" << labels_ << " "
        << metrics_->slidingWindow().interceptRate(60) << "\n\n";

    // ---- 规则命中 ----
    oss << "# HELP risk_rule_hit_total Rule hit count by rule_id\n";
    oss << "# TYPE risk_rule_hit_total counter\n";
    for (auto& kv : metrics_->ruleHitSnapshot()) {
        oss << "risk_rule_hit_total" << labels_
            << "{rule_id=\"" << kv.first << "\"} " << kv.second << "\n";
    }

    return oss.str();
}
