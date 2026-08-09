#pragma once
// ============================================================================
// risk_rule.h — 风控规则
//
// 与限流 Rule 的区别：
//   - 限流 Rule: "每秒钟最多 100 次" → 回答"多少次"
//   - 风控 RiskRule: "IP 信誉 > 70 且 新设备 → +35 风险分" → 回答"多危险"
//
// RiskRule 的 evaluate() 不依赖 Redis，只对特征向量做纯计算。
// 这保证了决策延迟可预测（< 1μs，无网络往返）。
// ============================================================================

#include "feature.h"
#include <string>
#include <vector>
#include <memory>
#include <sstream>

// ---------------------------------------------------------------------------
// IRuleCondition — 规则触发条件的抽象
//
// 为什么不是 std::function<bool(FeatureVector)>？
//   - 必须可序列化（规则存数据库/配置中心，运营通过后台配）
//   - 必须可解释（describe() 输出人类可读的原因）
//   - 可扩展（MLCondition 需要加载模型文件）
// ---------------------------------------------------------------------------
class IRuleCondition {
public:
    virtual ~IRuleCondition() = default;
    virtual bool evaluate(const std::vector<Feature>& features) const = 0;
    virtual std::string describe() const = 0;
};

// ---- 阈值条件 ----
class ThresholdCondition : public IRuleCondition {
public:
    enum Op { GT, GE, LT, LE, EQ };

    ThresholdCondition(const std::string& featureName, Op op, double threshold)
        : featureName_(featureName), op_(op), threshold_(threshold) {}

    bool evaluate(const std::vector<Feature>& features) const override {
        auto* f = findFeature(features, featureName_);
        if (!f) return false;
        switch (op_) {
            case GT: return f->value >  threshold_;
            case GE: return f->value >= threshold_;
            case LT: return f->value <  threshold_;
            case LE: return f->value <= threshold_;
            case EQ: return f->value == threshold_;
        }
        return false;
    }

    std::string describe() const override {
        static const char* opStr[] = {">", ">=", "<", "<=", "=="};
        std::ostringstream oss;
        oss << featureName_ << " " << opStr[op_] << " " << threshold_;
        return oss.str();
    }

private:
    std::string featureName_;
    Op op_;
    double threshold_;
};

// ---- 组合条件 (AND / OR) ----
class CompositeCondition : public IRuleCondition {
public:
    enum Logic { AND, OR };
    explicit CompositeCondition(Logic logic = AND) : logic_(logic) {}

    void add(std::unique_ptr<IRuleCondition> c) {
        conditions_.push_back(std::move(c));
    }

    bool evaluate(const std::vector<Feature>& features) const override {
        if (conditions_.empty()) return false;
        if (logic_ == AND) {
            for (auto& c : conditions_)
                if (!c->evaluate(features)) return false;
            return true;
        } else { // OR
            for (auto& c : conditions_)
                if (c->evaluate(features)) return true;
            return false;
        }
    }

    std::string describe() const override {
        std::ostringstream oss;
        const char* sep = (logic_ == AND) ? " AND " : " OR ";
        oss << "(";
        for (size_t i = 0; i < conditions_.size(); ++i) {
            if (i > 0) oss << sep;
            oss << conditions_[i]->describe();
        }
        oss << ")";
        return oss.str();
    }

private:
    Logic logic_;
    std::vector<std::unique_ptr<IRuleCondition>> conditions_;
};

// ---------------------------------------------------------------------------
// RiskRule — 风控规则
//
// 一条规则 = 条件 + 分数 + 权重 + 分类
//   - condition:   什么时候触发（如 "ip_risk >= 70"）
//   - score:       触发后贡献多少分
//   - weight:      规则权重倍率（默认 1.0）
//   - category:    风险类别（ip_risk / behavior / velocity）
//   - effectiveScore(): 实际贡献 = score × weight
// ---------------------------------------------------------------------------
struct RiskRule {
    uint64_t    id       = 0;
    std::string name;
    int         priority = 0;
    bool        enabled  = true;
    int         score    = 0;       // 基础风险分
    double      weight   = 1.0;     // 规则权重
    std::string category;           // 风险类别
    std::string description;        // 人类可读的规则说明

    // 触发条件
    std::shared_ptr<IRuleCondition> condition;

    // 计算本条规则的实际贡献分
    int effectiveScore() const { return static_cast<int>(score * weight); }

    // 判断是否被特征向量触发
    bool triggered(const std::vector<Feature>& features) const {
        if (!enabled || !condition) return false;
        return condition->evaluate(features);
    }
};
