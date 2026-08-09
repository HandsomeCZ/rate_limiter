// DecisionEngine实现：IScorer评分策略(Additive/Weighted/MLModel) + 分数→Decision映射 + DecisionResult构造
#include "decision_engine/decision_engine.h"

// ============================================================================
// AdditiveScorer
// ============================================================================
// 评分逻辑：
//   总分 = Σ(触发规则的 effectiveScore) + Σ(特征的 contribution)
//
// 为什么特征也参与评分？
//   - 规则回答"这个信号组合是否异常"（二元触发）
//   - 特征值回答"这个信号有多大"（连续量化）
//   - 例如：QPS=50 只触发了"高频请求"规则（+20分），
//          但 QPS 特征本身也贡献 50*0.3=15 分
//   - 两部分叠加才是完整的风险画像
int AdditiveScorer::score(const std::vector<RiskRule>& rules,
                           const std::vector<Feature>& features,
                           std::vector<uint64_t>& outTriggeredIds) {
    int total = 0;

    // ---- Part 1: 规则触发贡献 ----
    for (const auto& rule : rules) {
        if (rule.triggered(features)) {
            total += rule.effectiveScore();
            outTriggeredIds.push_back(rule.id);
        }
    }

    // ---- Part 2: 特征值贡献 ----
    // 每个特征的 value × weight 计入总分
    // weight 为 0 的特征不参与评分
    for (const auto& f : features) {
        total += static_cast<int>(f.contribution());
    }

    return total;
}

// ============================================================================
// WeightedScorer
// ============================================================================
void WeightedScorer::setCategoryWeight(const std::string& category, double w) {
    categoryWeights_[category] = w;
}

int WeightedScorer::score(const std::vector<RiskRule>& rules,
                           const std::vector<Feature>& features,
                           std::vector<uint64_t>& outTriggeredIds) {
    int total = 0;

    for (const auto& rule : rules) {
        if (rule.triggered(features)) {
            int contrib = rule.effectiveScore();
            // 按类别加权
            auto it = categoryWeights_.find(rule.category);
            if (it != categoryWeights_.end()) {
                contrib = static_cast<int>(contrib * it->second);
            }
            total += contrib;
            outTriggeredIds.push_back(rule.id);
        }
    }

    for (const auto& f : features) {
        total += static_cast<int>(f.contribution());
    }

    return total;
}

// ============================================================================
// MLModelScorer — 扩展预留
//
// 当前实现退化为 AdditiveScorer 的行为。
// 生产环境接入真实模型时，替换为 ONNX Runtime 推理调用。
// 架构其余部分（DecisionEngine / Service / Controller）无感知。
// ============================================================================
int MLModelScorer::score(const std::vector<RiskRule>& rules,
                          const std::vector<Feature>& features,
                          std::vector<uint64_t>& outTriggeredIds) {
    // 生产环境替换为：
    //   1. 将 features 转为模型输入 tensor
    //   2. ort->Run(input, output)
    //   3. return output * 100

    // 当前退化为规则累加（保证接口可用，不假装有模型）
    int total = 0;
    for (const auto& rule : rules) {
        if (rule.triggered(features)) {
            total += rule.effectiveScore();
            outTriggeredIds.push_back(rule.id);
        }
    }
    for (const auto& f : features) {
        total += static_cast<int>(f.contribution());
    }
    return total;
}

// ============================================================================
// DecisionEngine
// ============================================================================

DecisionEngine::DecisionEngine() {
    scorer_ = std::make_unique<AdditiveScorer>();
}

void DecisionEngine::setScorer(std::unique_ptr<IScorer> scorer) {
    scorer_ = std::move(scorer);
}

void DecisionEngine::setThresholds(const ThresholdConfig& cfg) {
    thresholds_ = cfg;
}

DecisionResult DecisionEngine::evaluate(
    const std::vector<RiskRule>& rules,
    const std::vector<Feature>& features) {

    DecisionResult result;
    result.riskScore = 0;

    if (!scorer_) {
        result.decision = Decision::ALLOW;
        result.reason = "no scorer configured";
        return result;
    }

    // ---- Step 1: 评分 ----
    result.riskScore = scorer_->score(rules, features, result.triggeredRuleIds);

    // ---- Step 2: 分数 → 决策 ----
    result.decision = scoreToDecision(result.riskScore);

    // ---- Step 3: 额度（LIMIT 级别） ----
    result.limitQuota = (result.decision == Decision::LIMIT)
                        ? thresholds_.limitQuota
                        : thresholds_.normalQuota;

    // ---- Step 4: 人类可读原因 ----
    std::ostringstream reason;
    reason << "[score=" << result.riskScore << "] ";
    for (auto id : result.triggeredRuleIds) {
        for (const auto& r : rules) {
            if (r.id == id) {
                reason << r.name << "; ";
                break;
            }
        }
    }
    reason << decisionStr(result.decision);
    result.reason = reason.str();

    return result;
}

Decision DecisionEngine::scoreToDecision(int score) const {
    if (score > thresholds_.rejectThreshold)    return Decision::REJECT;
    if (score > thresholds_.limitThreshold)     return Decision::CHALLENGE;
    if (score > thresholds_.allowThreshold)     return Decision::LIMIT;
    return Decision::ALLOW;
}
