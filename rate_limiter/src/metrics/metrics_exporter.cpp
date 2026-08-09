// MetricsExporter实现：Prometheus text format + JSON格式导出
#include "metrics/metrics_exporter.h"

MetricsExporter::MetricsExporter(Metrics* metrics)
    : metrics_(metrics ? metrics : &Metrics::instance()) {}

void MetricsExporter::setLabel(const std::string& key, const std::string& value) {
    // 格式：{key="value"}
    std::ostringstream oss;
    oss << "{" << key << "=\"" << value << "\"}";
    labels_ = oss.str();
}

std::string MetricsExporter::toJson() const {
    if (!metrics_) return "{}";

    std::ostringstream oss;
    oss << "{"
        << "\"total\":" << metrics_->total()
        << ",\"allow\":" << metrics_->allowed()
        << ",\"limit\":" << metrics_->limited()
        << ",\"challenge\":" << metrics_->challenged()
        << ",\"reject\":" << metrics_->rejected()
        << ",\"intercept_rate\":" << metrics_->slidingWindow().interceptRate(60)
        << "}";
    return oss.str();
}
