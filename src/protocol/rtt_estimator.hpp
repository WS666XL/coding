#pragma once

#include <cstdint>

namespace lowlat {

// RTT 估计：指数加权移动平均（EWMA），平滑系数 1/8（与 TCP 的 SRTT 一致）。
//
// 为什么不用 min RTT：min 只降不升，一个偶然贴近轮询点的样本就能把它拉到极小值，
// 导致基于它的缓冲深度在两端之间二值跳变。上一轮的实验里深度在 20ms 与 42ms
// 之间反复横跳，p50 延迟因此在 35ms 和 68ms 之间劈叉，就是 min RTT 造成的。
//
// EWMA 对小样本不敏感，又能跟上持续变化；同时保留 min 仅用于观测上报。
class RttEstimator {
public:
    void reset() noexcept;

    // sample_us <= 0 会被忽略（无效样本不该污染估计）
    void update(double sample_us) noexcept;

    bool has_estimate() const noexcept { return samples_ > 0; }
    double smoothed_us() const noexcept { return srtt_us_; }
    double min_us() const noexcept { return min_us_; }
    std::uint64_t samples() const noexcept { return samples_; }

private:
    double srtt_us_{0.0};
    double min_us_{0.0};
    std::uint64_t samples_{0};
};

}  // namespace lowlat
