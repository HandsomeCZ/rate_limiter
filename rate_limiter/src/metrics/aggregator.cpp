// SlidingWindowAggregator实现：Bucket[60]环形数组 + O(1)读写 + 秒级推进清理 + query(最近N秒)
#include "metrics/aggregator.h"
#include "common/common.h"
#include <algorithm>

SlidingWindowAggregator::SlidingWindowAggregator() {
    currentSec_ = nowSec();
}

SlidingWindowAggregator::Bucket& SlidingWindowAggregator::currentBucket() {
    uint64_t now = nowSec();

    // 秒数推进 → 清理中间跳过的 bucket
    if (now != currentSec_) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (now != currentSec_) {  // double-check
            // 清理从 currentSec_+1 到 now 之间的 bucket
            for (uint64_t t = currentSec_ + 1; t <= now; ++t) {
                buckets_[t % BUCKET_COUNT].reset();
            }
            currentSec_ = now;
        }
    }

    return buckets_[now % BUCKET_COUNT];
}

void SlidingWindowAggregator::recordRequest(bool isReject) {
    currentBucket().add(isReject);
}

SlidingWindowAggregator::WindowStats SlidingWindowAggregator::query(
    int windowSec) const {

    if (windowSec > WINDOW_SEC) windowSec = WINDOW_SEC;
    if (windowSec <= 0) windowSec = 1;

    WindowStats stats{0, 0};
    uint64_t now = nowSec();

    // 遍历最近 windowSec 个 bucket
    for (int i = 0; i < windowSec; ++i) {
        uint64_t idx = (now - i) % BUCKET_COUNT;
        stats.total  += buckets_[idx].total.load();
        stats.reject += buckets_[idx].reject.load();
    }

    return stats;
}

void SlidingWindowAggregator::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (int i = 0; i < BUCKET_COUNT; ++i) {
        buckets_[i].reset();
    }
    currentSec_ = nowSec();
}
