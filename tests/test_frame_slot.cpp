// FrameSlot 是收包线程与 UI 线程之间唯一的共享状态，必须验证：
//   1. 覆盖式语义（只保留最新帧）
//   2. 丢帧计数准确
//   3. 多线程下不崩、不丢数据（无数据竞争，配合 TSan 使用）

#include "ui/frame_slot.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

lowlat::DecodedFrame make_frame(std::uint32_t index)
{
    lowlat::DecodedFrame frame;
    frame.frame_index = index;
    frame.capture_ts_us = static_cast<std::int64_t>(index) * 1000;
    frame.ready_ts_us = static_cast<std::int64_t>(index) * 1000 + 500;
    return frame;
}

}  // namespace

TEST(FrameSlot, TakeWithoutPushReturnsFalse)
{
    lowlat::FrameSlot slot;
    lowlat::DecodedFrame out;

    EXPECT_FALSE(slot.take(&out));
    EXPECT_FALSE(slot.has_pending());
    EXPECT_EQ(slot.pushed(), 0u);
    EXPECT_EQ(slot.dropped(), 0u);
}

TEST(FrameSlot, PushThenTakeReturnsSameFrame)
{
    lowlat::FrameSlot slot;
    slot.push(make_frame(42));

    EXPECT_TRUE(slot.has_pending());
    EXPECT_EQ(slot.pushed(), 1u);
    EXPECT_EQ(slot.dropped(), 0u);

    lowlat::DecodedFrame out;
    ASSERT_TRUE(slot.take(&out));
    EXPECT_EQ(out.frame_index, 42u);
    EXPECT_EQ(out.capture_ts_us, 42000);
    EXPECT_EQ(out.ready_ts_us, 42500);

    // 取走之后就空了
    EXPECT_FALSE(slot.take(&out));
    EXPECT_FALSE(slot.has_pending());
}

TEST(FrameSlot, OverwriteKeepsOnlyLatestAndCountsDrops)
{
    lowlat::FrameSlot slot;
    for (std::uint32_t i = 0; i < 5; ++i) {
        slot.push(make_frame(i));
    }

    EXPECT_EQ(slot.pushed(), 5u);
    EXPECT_EQ(slot.dropped(), 4u);  // 前 4 个被覆盖

    lowlat::DecodedFrame out;
    ASSERT_TRUE(slot.take(&out));
    EXPECT_EQ(out.frame_index, 4u);  // 拿到的是最新的
}

TEST(FrameSlot, ClearDiscardsPending)
{
    lowlat::FrameSlot slot;
    slot.push(make_frame(1));
    slot.clear();

    lowlat::DecodedFrame out;
    EXPECT_FALSE(slot.take(&out));
}

TEST(FrameSlot, RejectsNullOutput)
{
    lowlat::FrameSlot slot;
    slot.push(make_frame(1));
    EXPECT_FALSE(slot.take(nullptr));
    EXPECT_TRUE(slot.has_pending());  // 取失败不能把数据弄丢
}

TEST(FrameSlot, ConcurrentProducerAndConsumerStayConsistent)
{
    lowlat::FrameSlot slot;
    constexpr int kProduced = 20000;

    std::atomic<int> consumed{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (int i = 0; i < kProduced; ++i) {
            slot.push(make_frame(static_cast<std::uint32_t>(i)));
        }
        producer_done.store(true);
    });

    std::thread consumer([&]() {
        lowlat::DecodedFrame out;
        while (!producer_done.load() || slot.has_pending()) {
            if (slot.take(&out)) {
                consumed.fetch_add(1);
            }
        }
    });

    producer.join();
    consumer.join();

    // 消费数 + 被覆盖数 应当等于生产总数
    EXPECT_EQ(slot.pushed(), static_cast<std::uint64_t>(kProduced));
    EXPECT_EQ(consumed.load() + static_cast<int>(slot.dropped()),
              kProduced);
    EXPECT_GT(consumed.load(), 0);
}
