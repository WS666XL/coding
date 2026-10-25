#include "common/ring_buffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

struct Block {
    std::uint64_t seq{0};
    std::uint64_t payload[3]{0, 0, 0};
};

}  // namespace

TEST(SpscRingBuffer, CapacityRoundsUpToPowerOfTwo)
{
    lowlat::SpscRingBuffer<int> one(1);
    EXPECT_EQ(one.capacity(), 1u);

    lowlat::SpscRingBuffer<int> three(3);
    EXPECT_EQ(three.capacity(), 4u);

    lowlat::SpscRingBuffer<int> thousand(1000);
    EXPECT_EQ(thousand.capacity(), 1024u);

    const std::size_t zero_capacity = 0;
    EXPECT_THROW(
        {
            lowlat::SpscRingBuffer<int> guard(zero_capacity);
            (void)guard;
        },
        std::invalid_argument);
}

TEST(SpscRingBuffer, FifoOrderAndBoundaryBehaviour)
{
    lowlat::SpscRingBuffer<int> queue(4);

    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(queue.try_push(i));
    }
    EXPECT_TRUE(queue.full());
    EXPECT_EQ(queue.size(), 4u);

    EXPECT_FALSE(queue.try_push(99));
    EXPECT_EQ(queue.push_failures(), 1u);

    for (int i = 0; i < 4; ++i) {
        int out = -1;
        ASSERT_TRUE(queue.try_pop(&out));
        EXPECT_EQ(out, i);
    }

    EXPECT_TRUE(queue.empty());
    int out = -1;
    EXPECT_FALSE(queue.try_pop(&out));
    EXPECT_EQ(queue.pop_failures(), 1u);
}

TEST(SpscRingBuffer, NullOutPointerIsRejected)
{
    lowlat::SpscRingBuffer<int> queue(2);
    EXPECT_TRUE(queue.try_push(1));
    EXPECT_FALSE(queue.try_pop(nullptr));
    EXPECT_EQ(queue.size(), 1u);
}

TEST(SpscRingBuffer, WrapAroundKeepsOrder)
{
    lowlat::SpscRingBuffer<int> queue(4);

    int expected = 0;
    for (int round = 0; round < 1000; ++round) {
        ASSERT_TRUE(queue.try_push(round)) << "round=" << round;
        int out = -1;
        ASSERT_TRUE(queue.try_pop(&out)) << "round=" << round;
        ASSERT_EQ(out, expected) << "round=" << round;
        ++expected;
    }
    EXPECT_TRUE(queue.empty());
}

TEST(SpscRingBuffer, ConcurrentProducerConsumerKeepsOrderAndCount)
{
    constexpr std::uint64_t kTotal = 200000;

    lowlat::SpscRingBuffer<Block> queue(1024);
    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> received{0};
    std::atomic<bool> order_ok{true};

    std::thread producer([&queue, &producer_done] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            Block block;
            block.seq = i;
            block.payload[0] = i * 2;
            block.payload[1] = i * 3;
            block.payload[2] = i * 5;
            while (!queue.try_push(block)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&queue, &producer_done, &received, &order_ok] {
        std::uint64_t expected = 0;
        Block block;
        while (true) {
            if (queue.try_pop(&block)) {
                if (block.seq != expected || block.payload[0] != expected * 2 ||
                    block.payload[1] != expected * 3 || block.payload[2] != expected * 5) {
                    order_ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
                received.store(expected, std::memory_order_release);
                if (expected == kTotal) {
                    break;
                }
            } else {
                if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
                    break;
                }
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(order_ok.load());
    EXPECT_EQ(received.load(), kTotal);
    EXPECT_TRUE(queue.empty());
}
