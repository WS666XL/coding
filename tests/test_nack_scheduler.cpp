#include "protocol/nack_scheduler.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

// 所有包都"仍然缺失"
struct AllMissing {
    bool operator()(std::uint16_t) const { return true; }
};

// 所有包都"其实已经到了"
struct AllPresent {
    bool operator()(std::uint16_t) const { return false; }
};

}  // namespace

TEST(NackScheduler, WaitsForReorderWindowBeforeRequesting)
{
    lowlat::NackScheduler scheduler(8, 32);
    scheduler.note_missing(42, 0);

    EXPECT_TRUE(scheduler.collect(5000, AllMissing{}).empty());   // 5ms，窗口未到
    EXPECT_TRUE(scheduler.collect(7999, AllMissing{}).empty());   // 7.999ms，仍在等

    const auto ready = scheduler.collect(8000, AllMissing{});
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), 42);
    EXPECT_EQ(scheduler.pending(), 0u);
}

TEST(NackScheduler, DropsEntriesThatArrivedInTime)
{
    lowlat::NackScheduler scheduler(8, 32);
    scheduler.note_missing(7, 0);
    scheduler.note_missing(9, 0);

    // 7 号在窗口内晚到了，9 号确实丢了
    const auto ready = scheduler.collect(10000, [](std::uint16_t sequence) {
        return sequence != 7;
    });

    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), 9u);
    EXPECT_EQ(scheduler.pending(), 0u);
}

TEST(NackScheduler, DoesNotResetTimestampOnRepeatedNote)
{
    lowlat::NackScheduler scheduler(8, 32);
    scheduler.note_missing(100, 0);
    scheduler.note_missing(100, 5000);  // 再次发现同一缺口，不应重置计时

    const auto ready = scheduler.collect(8000, AllMissing{});
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), 100u);
}

TEST(NackScheduler, GathersMultipleSequencesIntoOneBatch)
{
    lowlat::NackScheduler scheduler(5, 32);
    for (std::uint16_t sequence = 10; sequence < 15; ++sequence) {
        scheduler.note_missing(sequence, 0);
    }

    const auto ready = scheduler.collect(5000, AllMissing{});
    ASSERT_EQ(ready.size(), 5u);
    for (std::size_t i = 0; i < ready.size(); ++i) {
        EXPECT_EQ(ready[i], static_cast<std::uint16_t>(10 + i));
    }
}

TEST(NackScheduler, BoundsPendingQueue)
{
    lowlat::NackScheduler scheduler(0, 4);
    for (std::uint16_t sequence = 0; sequence < 10; ++sequence) {
        scheduler.note_missing(sequence, 0);
    }
    EXPECT_EQ(scheduler.pending(), 4u);

    const auto ready = scheduler.collect(0, AllMissing{});
    ASSERT_EQ(ready.size(), 4u);
    // 保留的是最新的 4 个
    EXPECT_EQ(ready.front(), 6u);
    EXPECT_EQ(ready.back(), 9u);
}

TEST(NackScheduler, ZeroDelayRequestsImmediately)
{
    lowlat::NackScheduler scheduler(0, 8);
    scheduler.note_missing(3, 12345);

    const auto ready = scheduler.collect(12345, AllMissing{});
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), 3u);
}

TEST(NackScheduler, ResetClearsPending)
{
    lowlat::NackScheduler scheduler(8, 8);
    scheduler.note_missing(1, 0);
    EXPECT_EQ(scheduler.pending(), 1u);

    scheduler.reset();
    EXPECT_EQ(scheduler.pending(), 0u);
    EXPECT_TRUE(scheduler.collect(100000, AllMissing{}).empty());
}

TEST(NackScheduler, SanitizesNegativeDelay)
{
    lowlat::NackScheduler scheduler(-5, 0);
    EXPECT_EQ(scheduler.delay_ms(), 0);
    scheduler.note_missing(1, 0);
    EXPECT_GE(scheduler.pending(), 1u);
}
