#pragma once
// ============================================================================
// feature.h — 特征值对象
//
// Feature 是风险信号的最小单元。每个 Extractor 产出若干个 Feature，
// 汇总后交给 DecisionEngine 做评分。
//
// 设计要点：
//   - name/value/weight 三元组：运营可理解的最小风险单位
//   - source 字段：出问题时能定位是哪个 Extractor 产出的
//   - defaultSafe：降级时该 feature 的默认值（安全侧）
//   - 值对象语义：拷贝即独立
// ============================================================================

#include <string>
#include <vector>
#include <sstream>

struct Feature {
    std::string name;         // 特征名（如 "qps", "ip_risk", "account_age"）
    double      value = 0.0;  // 当前值
    double      weight = 1.0; // 对本条规则的贡献权重
    std::string source;       // 来源 Extractor 名称（调试用）
    double      defaultSafe = 0.0; // 降级时的默认安全值

    // 带权重的有效贡献值
    double contribution() const { return value * weight; }

    // 工厂方法：快速构造
    static Feature make(const std::string& name, double value,
                        double weight = 1.0,
                        const std::string& source = "") {
        Feature f;
        f.name = name; f.value = value; f.weight = weight;
        f.source = source; f.defaultSafe = 0.0;
        return f;
    }
};

// 从特征向量中按名称查找
inline const Feature* findFeature(const std::vector<Feature>& features,
                                   const std::string& name) {
    for (auto& f : features) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

// 调试输出
inline std::string featuresToString(const std::vector<Feature>& features) {
    std::ostringstream oss;
    for (size_t i = 0; i < features.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << features[i].name << "=" << features[i].value;
    }
    return oss.str();
}
