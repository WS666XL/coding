#include "common/frame_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <vector>

TEST(FramePool, RejectsZeroSizes)
{
    EXPECT_EQ(lowlat::FramePool::create(0, 4), nullptr);
    EXPECT_EQ(lowlat::FramePool::create(64, 0), nullptr);
}

TEST(FramePool, AcquireAndReleaseRoundTrip)
{
    auto pool = lowlat::FramePool::create(1024, 4);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->buffer_size(), 1024u);
    EXPECT_EQ(pool->available(), 4u);

    std::vector<std::shared_ptr<std::uint8_t>> frames;
    for (int i = 0; i < 4; ++i) {
        frames.push_back(pool->acquire());
    }
    for (const auto& frame : frames) {
        ASSERT_NE(frame, nullptr);
    }
    EXPECT_EQ(pool->available(), 0u);

    frames.clear();
    EXPECT_EQ(pool->available(), 4u);
    EXPECT_EQ(pool->exhausted_count(), 0u);
}

TEST(FramePool, ExhaustionReturnsNullAndCounts)
{
    auto pool = lowlat::FramePool::create(256, 2);
    ASSERT_NE(pool, nullptr);

    auto first = pool->acquire();
    auto second = pool->acquire();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(pool->acquire(), nullptr);
    EXPECT_EQ(pool->exhausted_count(), 1u);
}

TEST(FramePool, SlotsAreDistinctAndReusable)
{
    auto pool = lowlat::FramePool::create(512, 8);
    ASSERT_NE(pool, nullptr);

    std::set<std::uint8_t*> pointers;
    std::vector<std::shared_ptr<std::uint8_t>> frames;
    for (int i = 0; i < 8; ++i) {
        auto frame = pool->acquire();
        ASSERT_NE(frame, nullptr);
        pointers.insert(frame.get());
        frames.push_back(std::move(frame));
    }
    EXPECT_EQ(pointers.size(), 8u);

    frames.clear();
    EXPECT_EQ(pool->available(), 8u);
}

TEST(FramePool, BuffersStayValidAfterPoolDestruction)
{
    std::shared_ptr<std::uint8_t> outstanding;
    {
        auto pool = lowlat::FramePool::create(64, 1);
        ASSERT_NE(pool, nullptr);
        outstanding = pool->acquire();
        ASSERT_NE(outstanding, nullptr);
        outstanding.get()[0] = 0xAB;
    }
    // 池已销毁，deleter 持有 Core，访问与归还都不应崩溃
    EXPECT_EQ(outstanding.get()[0], 0xAB);
    outstanding.reset();
    SUCCEED();
}

TEST(FramePool, ConcurrentAcquireReleaseIsSafe)
{
    auto pool = lowlat::FramePool::create(128, 8);
    ASSERT_NE(pool, nullptr);

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&pool] {
            for (int i = 0; i < 20000; ++i) {
                auto frame = pool->acquire();
                if (!frame) {
                    std::this_thread::yield();
                    continue;
                }
                frame.get()[0] = static_cast<std::uint8_t>(i & 0xFF);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(pool->available(), 8u);
}
