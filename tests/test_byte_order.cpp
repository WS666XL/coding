#include "common/byte_order.hpp"
#include "common/byte_span.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

TEST(ByteOrder, WritesBigEndianLayout)
{
    std::array<std::uint8_t, 8> buffer{};

    lowlat::net::write_be16(buffer.data(), 0x1234);
    EXPECT_EQ(buffer[0], 0x12);
    EXPECT_EQ(buffer[1], 0x34);

    lowlat::net::write_be32(buffer.data(), 0x89ABCDEF);
    EXPECT_EQ(buffer[0], 0x89);
    EXPECT_EQ(buffer[1], 0xAB);
    EXPECT_EQ(buffer[2], 0xCD);
    EXPECT_EQ(buffer[3], 0xEF);

    lowlat::net::write_be64(buffer.data(), 0x0102030405060708ULL);
    EXPECT_EQ(buffer[0], 0x01);
    EXPECT_EQ(buffer[1], 0x02);
    EXPECT_EQ(buffer[6], 0x07);
    EXPECT_EQ(buffer[7], 0x08);
}

TEST(ByteOrder, RoundTripAllWidths)
{
    std::array<std::uint8_t, 16> buffer{};

    lowlat::net::write_be16(buffer.data() + 0, 0xBEEFu);
    lowlat::net::write_be32(buffer.data() + 2, 0xDEADBEEFu);
    lowlat::net::write_be64(buffer.data() + 6, 0x0123456789ABCDEFULL);

    EXPECT_EQ(lowlat::net::read_be16(buffer.data() + 0), 0xBEEFu);
    EXPECT_EQ(lowlat::net::read_be32(buffer.data() + 2), 0xDEADBEEFu);
    EXPECT_EQ(lowlat::net::read_be64(buffer.data() + 6), 0x0123456789ABCDEFULL);
}

TEST(ByteOrder, HandlesUnalignedPointers)
{
    std::array<std::uint8_t, 16> buffer{};

    lowlat::net::write_be32(buffer.data() + 1, 0xCAFEBABEu);
    EXPECT_EQ(lowlat::net::read_be32(buffer.data() + 1), 0xCAFEBABEu);

    lowlat::net::write_be64(buffer.data() + 3, 0x1122334455667788ULL);
    EXPECT_EQ(lowlat::net::read_be64(buffer.data() + 3), 0x1122334455667788ULL);
}

TEST(ByteOrder, ZeroAndMaxValues)
{
    std::array<std::uint8_t, 8> buffer{};

    lowlat::net::write_be32(buffer.data(), 0);
    EXPECT_EQ(lowlat::net::read_be32(buffer.data()), 0u);

    lowlat::net::write_be32(buffer.data(), 0xFFFFFFFFu);
    EXPECT_EQ(lowlat::net::read_be32(buffer.data()), 0xFFFFFFFFu);
}

TEST(ByteSpan, BasicAccessors)
{
    const std::uint8_t raw[] = {1, 2, 3, 4, 5};
    const lowlat::ByteSpan span(raw, 5);

    EXPECT_EQ(span.size(), 5u);
    EXPECT_FALSE(span.empty());
    EXPECT_EQ(span.data(), raw);
    EXPECT_EQ(span[2], 3);
    EXPECT_EQ(span.begin(), raw);
    EXPECT_EQ(span.end(), raw + 5);
}

TEST(ByteSpan, FirstAndSubspanAreBoundsSafe)
{
    const std::uint8_t raw[] = {10, 20, 30, 40, 50};
    const lowlat::ByteSpan span(raw, 5);

    EXPECT_EQ(span.first(2).size(), 2u);
    EXPECT_EQ(span.first(2)[1], 20);
    EXPECT_TRUE(span.first(10).empty());

    const lowlat::ByteSpan middle = span.subspan(2, 2);
    EXPECT_EQ(middle.size(), 2u);
    EXPECT_EQ(middle[0], 30);
    EXPECT_EQ(middle[1], 40);

    // 请求长度超过剩余时截断到剩余长度
    EXPECT_EQ(span.subspan(2, 100).size(), 3u);
    // 起始越界返回空视图
    EXPECT_TRUE(span.subspan(100, 1).empty());
}

TEST(ByteSpan, DefaultConstructedIsEmpty)
{
    const lowlat::ByteSpan span;
    EXPECT_TRUE(span.empty());
    EXPECT_EQ(span.size(), 0u);
}
