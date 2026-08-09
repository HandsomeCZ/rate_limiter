#pragma once
// ============================================================================
// decision_engine.h — 决策引擎
//
// 三层结构的核心：
//   Layer 1 (Feature)     → FeatureVector（外部传入）
//   Layer 2 (Score)       → IScorer.score()  → totalScore
//   Layer 3 (Decision)    → scoreToLevel()    → DecisionResult
//
// 可扩展点：
//   - IScorer：替换评分策略（累加 → 加权 → ML 模型）
//   - ThresholdConfig：分数→级别映射（热加载）
//
// 为什么 IScorer 是接口？
//   - 累加评分：适合规则系统（可解释、可审计）
//   - ML 评分：适合复杂模式（高准确率、自动学习）
//   - A/B 测试：两套 Scorer 并存，按流量分组
// ============================================================================

#include "common/common.h"
#include "decision_engine/risk_rule.h"
#include "decision_engine/feature.h"
#include <vector>
#include <memory>
#include <sstream>
#include <unordered_map>

// ---------------------------------------------------------------------------
// DecisionResult — 决策引擎输出
// ---------------------------------------------------------------------------
struct DecisionResult {
    int         riskScore   = 0;
    Decision    decision    = Decision::ALLOW;
    int         limitQuota  = 100;    // LIMIT 级别时的建议配额（req/s）
    std::string reason;               // 人类可读
    std::vector<uint64_t> triggeredRuleIds;

    bool isAllowed()  const { return decision == Decision::ALLOW; }
    bool isRejected() const { return decision == Decision::REJECT; }
    bool needsLimit() const { return decision == Decision::LIMIT; }
};

// ---------------------------------------------------------------------------
// ThresholdConfig — 分数→级别的映射
//
// 可热加载：从 ConfigManager 1 秒刷新一次
// ---------------------------------------------------------------------------
struct ThresholdConfig {
    int allowThreshold     = 30;   //  0 ~ 30  → ALLOW
    int limitThreshold     = 50;   // 31 ~ 50  → LIMIT
    int rejectThreshold    = 80;   // 51 ~ 80  → CHALLENGE
                                   // 81+      → REJECT
    int normalQuota = 100;
    int limitQuota  = 10;
};

// ---------------------------------------------------------------------------
// IScorer — 评分策略接口（策略模式）
//
// 输入：触发规则列表 + 特征向量
// 输出：风险总分 + 触发规则 ID 列表
//
// 为什么把"特征参与评分"放在 Scorer 中？
//   - 不同 Scorer 对特征的使用方式不同
//   - AdditiveScorer: rule.score + feature.contribution
//   - MLModelScorer: 特征向量直接送入模型推理
// ---------------------------------------------------------------------------
class IScorer {
public:
    virtual ~IScorer() = default;
    virtual const char* name() const = 0;

    // @param rules     命中的风控规则
    // @param features  特征向量
    // @param outTriggeredIds [out] 实际触发的规则 ID
    // @return 风险总分
    virtual int score(const std::vector<RiskRule>& rules,
                      const std::vector<Feature>& features,
                      std::vector<uint64_t>& outTriggeredIds) = 0;
};

// ---- 累加评分器（默认） ----
// 总分 = Σ(触发规则.effectiveScore) + Σ(特征.contribution)
class AdditiveScorer : public IScorer {
public:
    const char* name() const override { return "additive"; }

    int score(const std::vector<RiskRule>& rules,
              const std::vector<Feature>& features,
              std::vector<uint64_t>& outTriggeredIds) override;
};

// ---- 加权分组评分器 ----
// 不同风险类别可有不同的全局倍率
// 例如：支付场景下 behavior 风险权重 ×2
class WeightedScorer : public IScorer {
public:
    const char* name() const override { return "weighted"; }
    void setCategoryWeight(const std::string& category, double w);

    int score(const std::vector<RiskRule>& rules,
              const std::vector<Feature>& features,
              std::vector<uint64_t>& outTriggeredIds) override;

private:
    std::unordered_map<std::string, double> categoryWeights_;
};

// ---- ML 模型评分器（扩展预留，非实现） ----
//
// 这是一个 IScorer 的占位实现，展示架构的可扩展性。
//
// 生产环境接入流程（不需要改动 DecisionEngine 一行代码）：
//   Step 1: 算法团队用 Python 训练模型（LR / XGBoost / LightGBM）
//   Step 2: 导出为 ONNX 格式（或 PMML / TensorFlow SavedModel）
//   Step 3: 实现 MLModelScorer::score():
//             - 将 Feature[] 转为模型输入 tensor
//             - 调用 ONNX Runtime / TensorFlow Lite 推理
//             - 返回 [0, 100] 的风险分数
//   Step 4: decisionEngine_->setScorer(new MLModelScorer(modelPath))
//
// 为什么现在不实现？
//   - 模型是数据驱动的，脱离真实业务数据训练的模型没有意义
//   - 接口已就位，接入时业务代码（Service / DecisionEngine）零改动
//   - 这正是开闭原则的价值：对扩展开放，对修改关闭
//
// 当前实现：将所有 feature 值相加（仅用于验证 IScorer 接口正确性）
class MLModelScorer : public IScorer {
public:
    const char* name() const override { return "ml_model"; }

    // 设置模型文件路径（ONNX / PMML / TensorFlow SavedModel）
    // 生产环境在这里加载模型，当前为空实现
    void setModelPath(const std::string& path) { modelPath_ = path; }

    int score(const std::vector<RiskRule>& rules,
              const std::vector<Feature>& features,
              std::vector<uint64_t>& outTriggeredIds) override;

private:
    std::string modelPath_;  // 预留：模型文件路径
};

// ---------------------------------------------------------------------------
// DecisionEngine — 决策引擎（聚合根）
//
// 职责：
//   1. 调用 IScorer 计算风险总分
//   2. 根据 ThresholdConfig 映射分数到 Decision
//   3. 构造 DecisionResult（包含 score + decision + reason）
//
// 不负责：
//   - 特征提取（那是 FeatureExtractor 的事）
//   - 规则匹配（那是 RuleEngine 的事）
//   - 限流计数（那是 RateLimiter 的事）
// ---------------------------------------------------------------------------
class DecisionEngine {
public:
    DecisionEngine();

    // 替换评分策略（策略模式）
    void setScorer(std::unique_ptr<IScorer> scorer);

    // 更新阈值（从 ConfigManager 热加载）
    void setThresholds(const ThresholdConfig& cfg);

    // 核心：评估风险
    DecisionResult evaluate(const std::vector<RiskRule>& rules,
                            const std::vector<Feature>& features);

    // 查询当前配置
    const ThresholdConfig& thresholds() const { return thresholds_; }
    const IScorer* scorer() const { return scorer_.get(); }

private:
    Decision scoreToDecision(int score) const;

    std::unique_ptr<IScorer> scorer_;
    ThresholdConfig thresholds_;
};
