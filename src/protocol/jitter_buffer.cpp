#include "protocol/jitter_buffer.hpp"

#include <cmath>

namespace lowlat {
namespace {

constexpr double kSmoothingFactor = 16.0;
constexpr double kMicrosPerSecond = 1000000.0;

}  // namespace

void JitterEstimator::reset() noexcept
{
    jitter_us_ = 0.0;
    has_previous_ = false;
    previous_arrival_us_ = 0;
    previous_timestamp_ = 0;
    samples_ = 0;
}

double JitterEstimator::update(std::int64_t arrival_us, std::uint32_t rtp_timestamp,
                               int clock_rate) noexcept
{
    if (clock_rate <= 0) {
        return jitter_us_;
    }

    if (!has_previous_) {
        has_previous_ = true;
        previous_arrival_us_ = arrival_us;
        previous_timestamp_ = rtp_timestamp;
        ++samples_;
        return jitter_us_;
    }

    // 时间戳差值用 uint32 相减，天然处理 2^32 回绕
    const std::uint32_t timestamp_delta = rtp_timestamp - previous_timestamp_;
    const double timestamp_delta_us =
        static_cast<double>(timestamp_delta) * kMicrosPerSecond / static_cast<double>(clock_rate);
    const double arrival_delta_us = static_cast<double>(arrival_us - previous_arrival_us_);

    const double deviation = std::fabs(arrival_delta_us - timestamp_delta_us);
    jitter_us_ += (deviation - jitter_us_) / kSmoothingFactor;

    previous_arrival_us_ = arrival_us;
    previous_timestamp_ = rtp_timestamp;
    ++samples_;
    return jitter_us_;
}

BufferDepthPolicy::BufferDepthPolicy() : BufferDepthPolicy(Config{}) {}

BufferDepthPolicy::BufferDepthPolicy(Config config) : config_(config)
{
    if (config_.min_ms < 1) {
        config_.min_ms = 1;
    }
    if (config_.max_ms < config_.min_ms) {
        config_.max_ms = config_.min_ms;
    }
    if (config_.jitter_multiplier <= 0.0) {
        config_.jitter_multiplier = 1.0;
    }
    if (config_.rtt_margin_ms < 0) {
        config_.rtt_margin_ms = 0;
    }
}

int BufferDepthPolicy::compute_depth_ms(double jitter_us) const noexcept
{
    return compute_depth_ms(jitter_us, 0.0);
}

int BufferDepthPolicy::compute_depth_ms(double jitter_us, double rtt_us) const noexcept
{
    const double jitter_depth_us =
        (jitter_us > 0.0) ? jitter_us * config_.jitter_multiplier : 0.0;
    const double rtt_depth_us =
        (rtt_us > 0.0) ? rtt_us + static_cast<double>(config_.rtt_margin_ms) * 1000.0 : 0.0;

    const double depth_us = (jitter_depth_us > rtt_depth_us) ? jitter_depth_us : rtt_depth_us;
    const double depth_ms = depth_us / 1000.0;

    if (depth_ms <= static_cast<double>(config_.min_ms)) {
        return config_.min_ms;
    }
    if (depth_ms >= static_cast<double>(config_.max_ms)) {
        return config_.max_ms;
    }
    return static_cast<int>(depth_ms + 0.5);
}

}  // namespace lowlat
