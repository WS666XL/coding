#include "protocol/nack.hpp"
#include "transport/stream_sender.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

TEST(Nack, RoundTrip)
{
    lowlat::NackRequest request;
    request.sequences = {10, 11, 300, 65535};

    std::array<std::uint8_t, 128> buffer{};
    const std::size_t written = lowlat::serialize_nack(request, buffer.data());
    EXPECT_EQ(written, 2u + 2u * 4u);
    EXPECT_EQ(written, request.serialized_size());

    lowlat::NackRequest parsed;
    ASSERT_TRUE(lowlat::parse_nack(lowlat::ByteSpan(buffer.data(), written), &parsed));
    EXPECT_EQ(parsed.sequences, request.sequences);
}

TEST(Nack, ByteLayoutIsBigEndian)
{
    lowlat::NackRequest request;
    request.sequences = {0x1234, 0xABCD};

    std::array<std::uint8_t, 16> buffer{};
    ASSERT_EQ(lowlat::serialize_nack(request, buffer.data()), 6u);

    EXPECT_EQ(buffer[0], 0x00);
    EXPECT_EQ(buffer[1], 0x02);  // count = 2
    EXPECT_EQ(buffer[2], 0x12);
    EXPECT_EQ(buffer[3], 0x34);
    EXPECT_EQ(buffer[4], 0xAB);
    EXPECT_EQ(buffer[5], 0xCD);
}

TEST(Nack, EmptyRequestIsValidButEmpty)
{
    lowlat::NackRequest request;
    std::array<std::uint8_t, 8> buffer{};
    ASSERT_EQ(lowlat::serialize_nack(request, buffer.data()), 2u);

    lowlat::NackRequest parsed;
    ASSERT_TRUE(lowlat::parse_nack(lowlat::ByteSpan(buffer.data(), 2), &parsed));
    EXPECT_TRUE(parsed.empty());
}

TEST(Nack, SerializeTruncatesToMaxSequences)
{
    lowlat::NackRequest request;
    for (std::uint16_t i = 0; i < 100; ++i) {
        request.sequences.push_back(i);
    }

    std::vector<std::uint8_t> buffer(request.serialized_size() + 16, 0);
    const std::size_t written = lowlat::serialize_nack(request, buffer.data());
    EXPECT_EQ(written, 2u + 2u * lowlat::NackRequest::kMaxSequences);

    lowlat::NackRequest parsed;
    ASSERT_TRUE(lowlat::parse_nack(lowlat::ByteSpan(buffer.data(), written), &parsed));
    EXPECT_EQ(parsed.sequences.size(), lowlat::NackRequest::kMaxSequences);
    EXPECT_EQ(parsed.sequences.front(), 0);
}

TEST(Nack, RejectsMalformedPayloads)
{
    lowlat::NackRequest parsed;

    const std::uint8_t too_short[1] = {0};
    EXPECT_FALSE(lowlat::parse_nack(lowlat::ByteSpan(too_short, 1), &parsed));

    // count 声称 2 个但只给了 1 个
    const std::uint8_t truncated[4] = {0x00, 0x02, 0x00, 0x01};
    EXPECT_FALSE(lowlat::parse_nack(lowlat::ByteSpan(truncated, 4), &parsed));

    // count 超过上限
    const std::uint8_t oversized[2] = {0x00, 0xFF};
    EXPECT_FALSE(lowlat::parse_nack(lowlat::ByteSpan(oversized, 2), &parsed));

    EXPECT_FALSE(lowlat::parse_nack(lowlat::ByteSpan(nullptr, 0), &parsed));
    EXPECT_FALSE(lowlat::parse_nack(lowlat::ByteSpan(truncated, 4), nullptr));
}

TEST(RetransmitCache, StoresAndFinds)
{
    lowlat::RetransmitCache cache(8);
    const std::vector<std::uint8_t> payload{1, 2, 3, 4};

    cache.store(42, lowlat::ByteSpan(payload.data(), payload.size()));
    const auto* found = cache.find(42);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, payload);

    EXPECT_EQ(cache.find(43), nullptr);
}

TEST(RetransmitCache, OverwritesSameSlotWithoutLosingOthers)
{
    lowlat::RetransmitCache cache(4);
    const std::vector<std::uint8_t> a{9, 9};
    const std::vector<std::uint8_t> b{7};

    cache.store(0, lowlat::ByteSpan(a.data(), a.size()));
    cache.store(4, lowlat::ByteSpan(b.data(), b.size()));  // 与 seq 0 同槽

    const auto* found = cache.find(4);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, b);
    EXPECT_EQ(cache.find(0), nullptr);  // 已被覆盖
}

TEST(RetransmitCache, SurvivesSequenceWraparound)
{
    lowlat::RetransmitCache cache(16);
    const std::vector<std::uint8_t> payload{5, 5, 5};

    cache.store(65535, lowlat::ByteSpan(payload.data(), payload.size()));
    const auto* found = cache.find(65535);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, payload);
}

TEST(RetransmitCache, RejectsEmptyPayload)
{
    lowlat::RetransmitCache cache(4);
    const std::uint8_t dummy = 0;
    cache.store(1, lowlat::ByteSpan(&dummy, 0));
    EXPECT_EQ(cache.find(1), nullptr);
}
