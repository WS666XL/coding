#pragma once

#include <cstdint>

namespace lowlat {

// RFC 3550 风格的到达抖动估计。
//
//   D(i,j) = (Rj - Ri) - (Sj - Si)
//   J(i)   = J(i-1) + (|D(i-1,i)| - J(i-1)) / 16
//
// R 是本机接收时刻，S 是包里的 RTP 时间戳。用"接收间隔 - 发送间隔"而不是
// "绝对到达时间 - 发送时间"，是因为前者不需要两端时钟同步——这是跨机部署时
// 唯一能直接用的口径。
//
// 平滑系数 16 直接取自 RFC，作用是让估计对单次抖动尖峰不敏感，又能在
// 8~16 个包内跟上持续性的抖动变化。
class JitterEstimator {
public:
    void reset() noexcept;

    // 返回更新后的抖动估计（微秒）。clock_rate 是 RTP 时钟频率，默认 90kHz。
    double update(std::int64_t arrival_us, std::uint32_t rtp_timestamp,
                  int clock_rate = 90000) noexcept;

    double jitter_us() const noexcept { return jitter_us_; }
    std::uint64_t samples() const noexcept { return samples_; }
    bool has_estimate() const noexcept { return samples_ > 1; }

private:
    double jitter_us_{0.0};
    bool has_previous_{false};
    std::int64_t previous_arrival_us_{0};
    std::uint32_t previous_timestamp_{0};
    std::uint64_t samples_{0};
};

// 抖动缓冲深度策略：深度 = clamp(min, max(k * jitter, rtt + margin), max)。
//
// 为什么同时看抖动和 RTT：这个保持窗口承担两项职责——
//   1) 吸收网络抖动与乱序（由 k * jitter 决定）
//   2) 等 NACK 重传回来（至少要一个 RTT）
// 只按抖动算深度会把职责 2 饿死：抖动很小时窗口被压到 20ms，重传还没回来帧就
// 已经落盘了。上一轮实验里自适应模式的"晚到丢弃"从 914 涨到 1908，就是这个原因。
class BufferDepthPolicy {
public:
    struct Config {
        int min_ms{20};
        int max_ms{120};
        double jitter_multiplier{3.0};
        int rtt_margin_ms{10};
    };

    BufferDepthPolicy();
    explicit BufferDepthPolicy(Config config);

    // 等价于 rtt_us = 0，保留给不关心重传的调用方与既有测试
    int compute_depth_ms(double jitter_us) const noexcept;

    int compute_depth_ms(double jitter_us, double rtt_us) const noexcept;

    const Config& config() const noexcept { return config_; }

private:
    Config config_{};
};

}  // namespace lowlat
