#pragma once
// ============================================================================
// aggregator.h — 滑动窗口聚合器
//
// 用途：
//   统计最近 N 秒内的请求总量和拒绝量，计算实时拦截率。
//   典型用法：监控大盘展示"最近 60 秒拦截率"。
//
// 数据结构：环形数组
//   - 60 个 bucket，每个 bucket 代表 1 秒
//   - 当前秒的索引 = (now_sec % 60)
//   - 写入时先清理过期 bucket（如果秒数跳跃）
//
// 为什么用环形数组而不是 Redis ZSet？
//   - 指标统计是本地操作，不需要跨机共享
//   - 环形数组 O(1) 读写，ZSet O(log N)
//   - 内存固定（60 × 2 × 8 = 960 bytes），无 GC
//
// 线程安全：
//   - 每个 bucket 的计数用 atomic
//   - 秒切换时用 mutex（频率低：每秒 1 次）
// ============================================================================

#include <atomic>
#include <mutex>
#include <cstdint>

class SlidingWindowAggregator {
public:
    static constexpr int WINDOW_SEC = 60;   // 窗口大小（秒）
    static constexpr int BUCKET_COUNT = 60;  // bucket 数量（1 秒 = 1 bucket）

    struct Bucket {
        std::atomic<uint64_t> total{0};
        std::atomic<uint64_t> reject{0};

        void reset() { total = 0; reject = 0; }
        void add(bool isReject) {
            total++;
            if (isReject) reject++;
        }
    };

    SlidingWindowAggregator();

    // 记录一次请求
    // @param isReject  是否被拒绝
    void recordRequest(bool isReject);

    // 查询最近 windowSec 秒的聚合值
    // @return {total, reject}
    struct WindowStats {
        uint64_t total;
        uint64_t reject;
    };
    WindowStats query(int windowSec = WINDOW_SEC) const;

    // 查询拦截率
    // @return 0.0 ~ 1.0（无请求时返回 0.0）
    double interceptRate(int windowSec = WINDOW_SEC) const;

    // 重置所有 bucket（压测用）
    void reset();

private:
    Bucket& currentBucket();
    void advanceBucket(uint64_t nowSec);

    Bucket buckets_[BUCKET_COUNT];
    mutable std::mutex mtx_;
    uint64_t currentSec_ = 0;
};

// 便捷函数
inline double SlidingWindowAggregator::interceptRate(int windowSec) const {
    auto s = query(windowSec);
    if (s.total == 0) return 0.0;
    return static_cast<double>(s.reject) / static_cast<double>(s.total);
}
