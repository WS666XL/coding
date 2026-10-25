#include "common/stats.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(Stats, CountersAccumulateThenReset)
{
    lowlat::Stats stats;
    stats.add_frames(3);
    stats.add_frames(2);
    stats.add_bytes(1000);
    stats.add_packets(10);
    stats.add_packets_lost(2);
    stats.add_fec_recovered(1);
    stats.add_retransmits(4);
    stats.add_errors(1);

    const auto snapshot = stats.snapshot_and_reset();
    EXPECT_EQ(snapshot.frames, 5u);
    EXPECT_EQ(snapshot.bytes, 1000u);
    EXPECT_EQ(snapshot.packets, 10u);
    EXPECT_EQ(snapshot.packets_lost, 2u);
    EXPECT_EQ(snapshot.fec_recovered, 1u);
    EXPECT_EQ(snapshot.retransmits, 4u);
    EXPECT_EQ(snapshot.errors, 1u);

    const auto second = stats.snapshot_and_reset();
    EXPECT_EQ(second.frames, 0u);
    EXPECT_EQ(second.bytes, 0u);
}

TEST(Stats, PercentilesOnKnownSamples)
{
    lowlat::Stats stats(1024);
    for (int i = 1; i <= 100; ++i) {
        stats.record_latency_us(static_cast<std::int64_t>(i) * 1000);  // 1ms .. 100ms
    }

    const auto snapshot = stats.peek();
    EXPECT_EQ(snapshot.latency_samples, 100u);
    EXPECT_NEAR(snapshot.latency_avg_ms, 50.5, 0.01);
    EXPECT_NEAR(snapshot.latency_p50_ms, 50.5, 0.01);
    EXPECT_NEAR(snapshot.latency_p99_ms, 99.01, 0.01);
    EXPECT_NEAR(snapshot.latency_max_ms, 100.0, 0.001);
}

TEST(Stats, SnapshotClearsLatencyWindow)
{
    lowlat::Stats stats(8);
    stats.record_latency_us(1000);
    stats.record_latency_us(2000);

    const auto first = stats.snapshot_and_reset();
    EXPECT_EQ(first.latency_samples, 2u);

    const auto second = stats.snapshot_and_reset();
    EXPECT_EQ(second.latency_samples, 0u);
    EXPECT_DOUBLE_EQ(second.latency_p99_ms, 0.0);
}

TEST(Stats, NegativeLatencyIsClampedToZero)
{
    lowlat::Stats stats;
    stats.record_latency_us(-500);

    const auto snapshot = stats.peek();
    ASSERT_EQ(snapshot.latency_samples, 1u);
    EXPECT_DOUBLE_EQ(snapshot.latency_max_ms, 0.0);
    EXPECT_DOUBLE_EQ(snapshot.latency_p50_ms, 0.0);
}

TEST(Stats, EmptyWindowReportsZero)
{
    lowlat::Stats stats;
    const auto snapshot = stats.peek();
    EXPECT_EQ(snapshot.latency_samples, 0u);
    EXPECT_DOUBLE_EQ(snapshot.latency_p99_ms, 0.0);
}

TEST(Stats, ToStringContainsKeyFields)
{
    lowlat::Stats stats;
    stats.add_frames(1);
    stats.record_latency_us(1234);

    const std::string text = stats.snapshot_and_reset().to_string();
    EXPECT_NE(text.find("frames=1"), std::string::npos);
    EXPECT_NE(text.find("p99"), std::string::npos);
    EXPECT_NE(text.find("ms"), std::string::npos);
}

TEST(Stats, LatencyWindowWrapsWithoutOverflow)
{
    lowlat::Stats stats(4);
    for (int i = 0; i < 10; ++i) {
        stats.record_latency_us(static_cast<std::int64_t>(i + 1) * 1000);
    }
    const auto snapshot = stats.peek();
    EXPECT_EQ(snapshot.latency_samples, 4u);
    EXPECT_LE(snapshot.latency_max_ms, 10.0);
}
