#include "protocol/jitter_buffer.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

// 用 1kHz 时钟让换算精确（1 个 tick = 1ms），避免浮点误差影响断言
constexpr int kClockRate = 1000;
constexpr std::int64_t kFrameIntervalUs = 33000;
constexpr std::uint32_t kTimestampStep = 33;  // 33ms @ 1kHz

}  // namespace

TEST(JitterEstimator, HasNoEstimateWithSingleSample)
{
    lowlat::JitterEstimator estimator;
    EXPECT_FALSE(estimator.has_estimate());
    EXPECT_EQ(estimator.samples(), 0u);

    estimator.update(0, 0, kClockRate);
    EXPECT_FALSE(estimator.has_estimate());  // 只有一个点算不出抖动
    EXPECT_EQ(estimator.samples(), 1u);
    EXPECT_DOUBLE_EQ(estimator.jitter_us(), 0.0);
}

TEST(JitterEstimator, ConstantCadenceYieldsNoJitter)
{
    lowlat::JitterEstimator estimator;

    std::int64_t arrival = 1000000;
    std::uint32_t timestamp = 0;
    for (int i = 0; i < 100; ++i) {
        estimator.update(arrival, timestamp, kClockRate);
        arrival += kFrameIntervalUs;
        timestamp += kTimestampStep;
    }

    EXPECT_TRUE(estimator.has_estimate());
    EXPECT_LT(estimator.jitter_us(), 1.0);
}

TEST(JitterEstimator, SingleSpikeIsSmoothedThenDecays)
{
    lowlat::JitterEstimator estimator;

    std::int64_t arrival = 1000000;
    std::uint32_t timestamp = 0;

    // 先稳定 20 个包
    for (int i = 0; i < 20; ++i) {
        estimator.update(arrival, timestamp, kClockRate);
        arrival += kFrameIntervalUs;
        timestamp += kTimestampStep;
    }
    ASSERT_LT(estimator.jitter_us(), 1.0);

    // 一个包晚到 10ms
    arrival += 10000;
    estimator.update(arrival, timestamp, kClockRate);
    const double after_spike = estimator.jitter_us();
    // 单次偏差 10000us，被 1/16 平滑后约 625us
    EXPECT_GT(after_spike, 400.0);
    EXPECT_LT(after_spike, 700.0);

    // 之后恢复正常节奏，估计应逐步衰减
    arrival += kFrameIntervalUs;
    timestamp += kTimestampStep;
    for (int i = 0; i < 60; ++i) {
        estimator.update(arrival, timestamp, kClockRate);
        arrival += kFrameIntervalUs;
        timestamp += kTimestampStep;
    }
    EXPECT_LT(estimator.jitter_us(), 50.0);
}

TEST(JitterEstimator, AlternatingJitterConvergesToAmplitude)
{
    lowlat::JitterEstimator estimator;

    std::int64_t arrival = 1000000;
    std::uint32_t timestamp = 0;

    // 到达间隔在 33ms / 43ms 之间来回跳，等价于 ±10ms 的抖动
    for (int i = 0; i < 300; ++i) {
        estimator.update(arrival, timestamp, kClockRate);
        timestamp += kTimestampStep;
        arrival += (i % 2 == 0) ? kFrameIntervalUs + 10000 : kFrameIntervalUs - 10000;
    }

    EXPECT_NEAR(estimator.jitter_us(), 10000.0, 300.0);
}

TEST(JitterEstimator, HandlesTimestampWraparound)
{
    lowlat::JitterEstimator estimator;

    std::int64_t arrival = 1000000;
    std::uint32_t timestamp = 0xFFFFFFFFu - 100;  // 即将回绕

    for (int i = 0; i < 100; ++i) {
        estimator.update(arrival, timestamp, kClockRate);
        arrival += kFrameIntervalUs;
        timestamp += kTimestampStep;  // 自然回绕
    }

    EXPECT_LT(estimator.jitter_us(), 1.0);
}

TEST(JitterEstimator, ResetClearsState)
{
    lowlat::JitterEstimator estimator;

    std::int64_t arrival = 0;
    std::uint32_t timestamp = 0;
    for (int i = 0; i < 10; ++i) {
        estimator.update(arrival, timestamp, kClockRate);
        arrival += kFrameIntervalUs + 5000;
        timestamp += kTimestampStep;
    }
    ASSERT_GT(estimator.jitter_us(), 0.0);

    estimator.reset();
    EXPECT_DOUBLE_EQ(estimator.jitter_us(), 0.0);
    EXPECT_EQ(estimator.samples(), 0u);
    EXPECT_FALSE(estimator.has_estimate());
}

TEST(JitterEstimator, IgnoresInvalidClockRate)
{
    lowlat::JitterEstimator estimator;
    estimator.update(0, 0, 0);
    EXPECT_EQ(estimator.samples(), 0u);
}

TEST(BufferDepthPolicy, ClampsToConfiguredRange)
{
    lowlat::BufferDepthPolicy::Config config;
    config.min_ms = 20;
    config.max_ms = 120;
    config.jitter_multiplier = 3.0;
    lowlat::BufferDepthPolicy policy(config);

    EXPECT_EQ(policy.compute_depth_ms(0.0), 20);
    EXPECT_EQ(policy.compute_depth_ms(-5.0), 20);
    EXPECT_EQ(policy.compute_depth_ms(1000.0), 20);   // 1ms 抖动 × 3 = 3ms → 取 min
    EXPECT_EQ(policy.compute_depth_ms(10000.0), 30);  // 10ms × 3 = 30ms
    EXPECT_EQ(policy.compute_depth_ms(25000.0), 75);  // 25ms × 3 = 75ms
    EXPECT_EQ(policy.compute_depth_ms(100000.0), 120);  // 300ms → 取 max
}

TEST(BufferDepthPolicy, SanitizesInvalidConfig)
{
    lowlat::BufferDepthPolicy::Config config;
    config.min_ms = 0;
    config.max_ms = -10;
    config.jitter_multiplier = 0.0;
    lowlat::BufferDepthPolicy policy(config);

    EXPECT_EQ(policy.config().min_ms, 1);
    EXPECT_GE(policy.config().max_ms, policy.config().min_ms);
    EXPECT_GT(policy.config().jitter_multiplier, 0.0);
    EXPECT_EQ(policy.compute_depth_ms(0.0), policy.config().min_ms);
}

TEST(BufferDepthPolicy, RttFloorWinsWhenJitterIsSmall)
{
    lowlat::BufferDepthPolicy::Config config;
    config.min_ms = 20;
    config.max_ms = 120;
    config.jitter_multiplier = 3.0;
    config.rtt_margin_ms = 10;
    lowlat::BufferDepthPolicy policy(config);

    // 抖动只有 1ms（×3 = 3ms），但 RTT 50ms：窗口必须能覆盖一个来回
    EXPECT_EQ(policy.compute_depth_ms(1000.0, 50000.0), 60);
}

TEST(BufferDepthPolicy, JitterWinsWhenLargerThanRtt)
{
    lowlat::BufferDepthPolicy::Config config;
    config.min_ms = 20;
    config.max_ms = 120;
    config.jitter_multiplier = 3.0;
    config.rtt_margin_ms = 10;
    lowlat::BufferDepthPolicy policy(config);

    // 抖动 30ms（×3 = 90ms）远大于 RTT+margin = 15ms
    EXPECT_EQ(policy.compute_depth_ms(30000.0, 5000.0), 90);
}

TEST(BufferDepthPolicy, SingleArgumentOverloadEqualsZeroRtt)
{
    lowlat::BufferDepthPolicy policy;
    EXPECT_EQ(policy.compute_depth_ms(10000.0), policy.compute_depth_ms(10000.0, 0.0));
}

TEST(BufferDepthPolicy, DefaultConfigIsUsable)
{
    lowlat::BufferDepthPolicy policy;
    EXPECT_EQ(policy.compute_depth_ms(0.0), policy.config().min_ms);
    EXPECT_LE(policy.compute_depth_ms(1e9), policy.config().max_ms);
}
