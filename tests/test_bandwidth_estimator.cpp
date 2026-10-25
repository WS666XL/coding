// 带宽估计器的验证：全部用合成时序，不依赖真实网络。
//
// 这是拥塞控制里唯一能严谨自测的部分——真实网络下的收敛行为受缓冲区、
// 交叉流量影响，只能给出趋势；而"延迟涨了该降速、延迟平稳该提速"这条
// 逻辑本身是可以精确断言的。

#include "protocol/bandwidth_estimator.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr std::int64_t kFrameIntervalUs = 33000;  // 30fps

lowlat::BandwidthEstimator::Config make_config()
{
    lowlat::BandwidthEstimator::Config config;
    config.min_bps = 300000;
    config.max_bps = 4000000;
    config.start_bps = 1000000;
    config.increase_factor = 1.08;
    config.decrease_factor = 0.85;
    config.cooldown_us = 0;           // 默认关掉降温，需要时单独测
    config.decrease_cooldown_us = 0;  // 同上，避免干扰"连续过载"的用例
    config.window_samples = 10;
    config.overuse_threshold_us = 15000;
    config.underuse_threshold_us = 15000;
    config.loss_high = 0.10;
    config.loss_low = 0.02;
    return config;
}

// 以固定节奏喂样本：arrival_delta 相对 send_delta 的额外偏移由 extra_us 决定
void feed(lowlat::BandwidthEstimator* estimator, int samples, std::int64_t extra_per_sample_us)
{
    std::int64_t send = 1000000;
    std::int64_t arrival = 1000000;

    for (int i = 0; i < samples; ++i) {
        estimator->on_sample(send, arrival, arrival);
        send += kFrameIntervalUs;
        arrival += kFrameIntervalUs + extra_per_sample_us;
    }
}

}  // namespace

TEST(BandwidthEstimator, StartsAtConfiguredBitrate)
{
    lowlat::BandwidthEstimator estimator(make_config());
    EXPECT_EQ(estimator.target_bps(), 1000000);
    EXPECT_EQ(estimator.state(), lowlat::BandwidthEstimator::State::Normal);
    EXPECT_EQ(estimator.increase_events(), 0u);
}

TEST(BandwidthEstimator, ConstantDelayIncreasesBitrate)
{
    lowlat::BandwidthEstimator estimator(make_config());
    feed(&estimator, 100, 0);

    EXPECT_GT(estimator.target_bps(), 1000000);
    EXPECT_LE(estimator.target_bps(), 4000000);
    EXPECT_GT(estimator.increase_events(), 0u);
    EXPECT_EQ(estimator.decrease_events(), 0u);
    EXPECT_EQ(estimator.state(), lowlat::BandwidthEstimator::State::Normal);
}

TEST(BandwidthEstimator, GrowingQueueDelayTriggersOveruseAndDecrease)
{
    lowlat::BandwidthEstimator estimator(make_config());

    // 每个样本比上个样本多 2ms 到达：链路队列在稳定堆积
    feed(&estimator, 30, 2000);

    EXPECT_EQ(estimator.state(), lowlat::BandwidthEstimator::State::Overuse);
    EXPECT_LT(estimator.target_bps(), 1000000);
    EXPECT_GT(estimator.decrease_events(), 0u);
    EXPECT_GT(estimator.overuse_events(), 0u);
    EXPECT_GT(estimator.last_window_delay_us(), 15000);
}

TEST(BandwidthEstimator, DrainingQueueIsUnderuseWithoutIncrease)
{
    lowlat::BandwidthEstimator estimator(make_config());

    // 到达间隔反而在缩短：队列在排空
    feed(&estimator, 30, -2000);

    EXPECT_EQ(estimator.state(), lowlat::BandwidthEstimator::State::Underuse);
    // 欠载时既不该降速，也不该趁机提速
    EXPECT_EQ(estimator.target_bps(), 1000000);
    EXPECT_GT(estimator.underuse_events(), 0u);
    EXPECT_EQ(estimator.increase_events(), 0u);
}

TEST(BandwidthEstimator, DecreaseCooldownPreventsConsecutiveCuts)
{
    // 大过冲之后队列需要时间排空，期间模型仍会看到过载。
    // 没有降速冷却时会被连续下调，最终远远低于链路真实容量。
    // 用同样的输入对比两种配置，避免依赖窗口对齐这类易碎假设。
    lowlat::BandwidthEstimator::Config no_cool = make_config();
    no_cool.decrease_cooldown_us = 0;

    lowlat::BandwidthEstimator::Config with_cool = make_config();
    with_cool.decrease_cooldown_us = 500000;

    lowlat::BandwidthEstimator eager(no_cool);
    lowlat::BandwidthEstimator patient(with_cool);

    feed(&eager, 40, 8000);
    feed(&patient, 40, 8000);

    EXPECT_GT(eager.decrease_events(), patient.decrease_events());
    // 冷却版降得更少，最终码率更高
    EXPECT_GT(patient.target_bps(), eager.target_bps());
}

TEST(BandwidthEstimator, SevereOveruseDecreasesFasterThanMildOne)
{
    // 同一窗口长度下，延迟堆得越高应该降得越多
    lowlat::BandwidthEstimator mild(make_config());
    feed(&mild, 30, 2000);  // 每样本 +2ms，窗口累计 20ms

    lowlat::BandwidthEstimator severe(make_config());
    feed(&severe, 30, 8000);  // 每样本 +8ms，窗口累计 80ms

    EXPECT_EQ(mild.state(), lowlat::BandwidthEstimator::State::Overuse);
    EXPECT_EQ(severe.state(), lowlat::BandwidthEstimator::State::Overuse);
    EXPECT_LT(severe.target_bps(), mild.target_bps());
}

TEST(BandwidthEstimator, HighLossDecreasesBitrateImmediately)
{
    lowlat::BandwidthEstimator estimator(make_config());
    estimator.on_loss_report(0.20, 5000000);

    EXPECT_EQ(estimator.target_bps(), 850000);  // 1000000 * 0.85
    EXPECT_EQ(estimator.decrease_events(), 1u);
}

TEST(BandwidthEstimator, ModerateLossBlocksIncreaseViaCooldown)
{
    lowlat::BandwidthEstimator::Config config = make_config();
    config.cooldown_us = 1000000;  // 1 秒内不许提速
    lowlat::BandwidthEstimator estimator(config);

    // 5% 丢包：不降速，但进入冷却
    estimator.on_loss_report(0.05, 2000000);
    EXPECT_EQ(estimator.target_bps(), 1000000);

    // 冷却期内喂正常样本：25 个样本 ≈ 825ms，仍短于 1 秒冷却
    std::int64_t send = 2000000;
    std::int64_t arrival = 2000000;
    for (int i = 0; i < 25; ++i) {
        estimator.on_sample(send, arrival, arrival);
        send += kFrameIntervalUs;
        arrival += kFrameIntervalUs;
    }
    EXPECT_EQ(estimator.target_bps(), 1000000);
    EXPECT_EQ(estimator.increase_events(), 0u);
}

TEST(BandwidthEstimator, ClampsToMaxBitrate)
{
    lowlat::BandwidthEstimator estimator(make_config());
    feed(&estimator, 400, 0);  // 足够多窗口把码率顶到上限

    EXPECT_EQ(estimator.target_bps(), 4000000);
}

TEST(BandwidthEstimator, ClampsToMinBitrate)
{
    lowlat::BandwidthEstimator estimator(make_config());
    feed(&estimator, 400, 3000);  // 持续严重过载

    EXPECT_EQ(estimator.target_bps(), 300000);
}

TEST(BandwidthEstimator, ResetRestoresInitialState)
{
    lowlat::BandwidthEstimator estimator(make_config());
    feed(&estimator, 50, 2000);
    ASSERT_LT(estimator.target_bps(), 1000000);

    estimator.reset();
    EXPECT_EQ(estimator.target_bps(), 1000000);
    EXPECT_EQ(estimator.state(), lowlat::BandwidthEstimator::State::Normal);
    EXPECT_EQ(estimator.decrease_events(), 0u);
    EXPECT_EQ(estimator.last_window_delay_us(), 0);

    // 复位后第一个样本只作为基准，不参与计算
    estimator.on_sample(1000, 1000, 1000);
    EXPECT_EQ(estimator.target_bps(), 1000000);
}

TEST(BandwidthEstimator, SanitizesInvalidConfig)
{
    lowlat::BandwidthEstimator::Config config;
    config.min_bps = 0;
    config.max_bps = 100;
    config.start_bps = 50;
    config.window_samples = 0;
    config.increase_factor = 0.5;
    config.decrease_factor = 2.0;

    lowlat::BandwidthEstimator estimator(config);
    EXPECT_GE(estimator.config().min_bps, 1000);
    EXPECT_GE(estimator.config().max_bps, estimator.config().min_bps);
    EXPECT_EQ(estimator.config().window_samples, 1u);
    EXPECT_GE(estimator.config().increase_factor, 1.0);
    EXPECT_LE(estimator.config().decrease_factor, 1.0);
    EXPECT_GE(estimator.target_bps(), estimator.config().min_bps);
    EXPECT_LE(estimator.target_bps(), estimator.config().max_bps);
}

TEST(BandwidthEstimator, DelayedFrameAfterIdleDoesNotLookLikeOveruse)
{
    // 真实场景：网络空闲一段时间后突然来一帧，到达间隔远大于发送间隔，
    // 但这不代表拥塞——同理，长时间停顿后的第一帧不应该被误判。
    lowlat::BandwidthEstimator estimator(make_config());

    estimator.on_sample(1000000, 1000000, 1000000);
    // 停顿 2 秒后的一帧：发送间隔与到达间隔都变大了 2 秒
    estimator.on_sample(3000000, 3000000, 3000000);

    std::int64_t send = 3000000;
    std::int64_t arrival = 3000000;
    for (int i = 0; i < 20; ++i) {
        estimator.on_sample(send, arrival, arrival);
        send += kFrameIntervalUs;
        arrival += kFrameIntervalUs;
    }

    EXPECT_EQ(estimator.state(), lowlat::BandwidthEstimator::State::Normal);
    EXPECT_EQ(estimator.decrease_events(), 0u);
}
