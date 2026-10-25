#include "protocol/rtp_lite.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

TEST(RtpLite, SerializeParseRoundTrip)
{
    lowlat::RtpLiteHeader original;
    original.marker = true;
    original.payload_type = lowlat::RtpLiteHeader::kPayloadTypeH264;
    original.sequence = 0x1234;
    original.timestamp = 0xDEADBEEF;
    original.ssrc = 0x01020304;
    original.frame_index = 900;
    original.capture_ts_us = 0x11223344;

    std::array<std::uint8_t, 24> buffer{};
    const std::size_t written = lowlat::serialize_rtp_lite(original, buffer.data());
    EXPECT_EQ(written, 20u);
    EXPECT_EQ(written, original.serialized_size());

    lowlat::RtpLiteHeader parsed;
    ASSERT_TRUE(lowlat::parse_rtp_lite(lowlat::ByteSpan(buffer.data(), written), &parsed));

    EXPECT_EQ(parsed.version, 2);
    EXPECT_EQ(parsed.payload_type, original.payload_type);
    EXPECT_TRUE(parsed.marker);
    EXPECT_TRUE(parsed.has_frame_index);
    EXPECT_EQ(parsed.sequence, original.sequence);
    EXPECT_EQ(parsed.timestamp, original.timestamp);
    EXPECT_EQ(parsed.ssrc, original.ssrc);
    EXPECT_EQ(parsed.frame_index, original.frame_index);
    EXPECT_EQ(parsed.capture_ts_us, original.capture_ts_us);
}

TEST(RtpLite, ByteLayoutIsBigEndian)
{
    lowlat::RtpLiteHeader header;
    header.marker = true;
    header.sequence = 0x1234;
    header.timestamp = 0x89ABCDEF;
    header.ssrc = 0x01020304;
    header.frame_index = 0x0A0B0C0D;
    header.capture_ts_us = 0xDEADBEEF;

    std::array<std::uint8_t, 24> buffer{};
    ASSERT_EQ(lowlat::serialize_rtp_lite(header, buffer.data()), 20u);

    // byte 0: V=2 (10) << 6 | X=1 (0x10) = 0x90
    EXPECT_EQ(buffer[0], 0x90);
    // byte 1: M=1 (0x80) | PT=96 (0x60)
    EXPECT_EQ(buffer[1], 0xE0);
    EXPECT_EQ(buffer[2], 0x12);
    EXPECT_EQ(buffer[3], 0x34);
    EXPECT_EQ(buffer[4], 0x89);
    EXPECT_EQ(buffer[5], 0xAB);
    EXPECT_EQ(buffer[6], 0xCD);
    EXPECT_EQ(buffer[7], 0xEF);
    EXPECT_EQ(buffer[8], 0x01);
    EXPECT_EQ(buffer[9], 0x02);
    EXPECT_EQ(buffer[10], 0x03);
    EXPECT_EQ(buffer[11], 0x04);
    EXPECT_EQ(buffer[12], 0x0A);
    EXPECT_EQ(buffer[13], 0x0B);
    EXPECT_EQ(buffer[14], 0x0C);
    EXPECT_EQ(buffer[15], 0x0D);
    // 采集时间戳紧随 frame_index，占 16..19
    EXPECT_EQ(buffer[16], 0xDE);
    EXPECT_EQ(buffer[17], 0xAD);
    EXPECT_EQ(buffer[18], 0xBE);
    EXPECT_EQ(buffer[19], 0xEF);
}

TEST(RtpLite, WithoutFrameIndexIsTwelveBytes)
{
    lowlat::RtpLiteHeader header;
    header.has_frame_index = false;
    header.sequence = 7;
    header.timestamp = 42;
    header.ssrc = 99;
    header.frame_index = 12345;  // 不应被序列化

    std::array<std::uint8_t, 16> buffer{};
    ASSERT_EQ(lowlat::serialize_rtp_lite(header, buffer.data()), 12u);

    lowlat::RtpLiteHeader parsed;
    ASSERT_TRUE(lowlat::parse_rtp_lite(lowlat::ByteSpan(buffer.data(), 12), &parsed));
    EXPECT_FALSE(parsed.has_frame_index);
    EXPECT_EQ(parsed.frame_index, 0u);
    EXPECT_EQ(parsed.capture_ts_us, 0u);
    EXPECT_EQ(parsed.sequence, 7u);
}

TEST(RtpLite, RejectsShortBuffer)
{
    std::array<std::uint8_t, 11> buffer{};
    buffer[0] = 0x80;  // V=2, X=0

    lowlat::RtpLiteHeader parsed;
    EXPECT_FALSE(lowlat::parse_rtp_lite(lowlat::ByteSpan(buffer.data(), buffer.size()), &parsed));

    // 声明了 X 位但只有 12 字节，也应当拒绝
    std::array<std::uint8_t, 12> truncated{};
    truncated[0] = 0x90;
    EXPECT_FALSE(lowlat::parse_rtp_lite(lowlat::ByteSpan(truncated.data(), truncated.size()),
                                        &parsed));

    // 扩展区不完整（只有 16 字节，缺采集时间戳）同样拒绝
    std::array<std::uint8_t, 16> partial{};
    partial[0] = 0x90;
    EXPECT_FALSE(lowlat::parse_rtp_lite(lowlat::ByteSpan(partial.data(), partial.size()),
                                        &parsed));
}

TEST(RtpLite, RejectsWrongVersion)
{
    std::array<std::uint8_t, 12> buffer{};
    buffer[0] = 0x40;  // V=1

    lowlat::RtpLiteHeader parsed;
    EXPECT_FALSE(lowlat::parse_rtp_lite(lowlat::ByteSpan(buffer.data(), buffer.size()), &parsed));
}

TEST(RtpLite, RejectsNullOutput)
{
    std::array<std::uint8_t, 12> buffer{};
    buffer[0] = 0x80;
    EXPECT_FALSE(lowlat::parse_rtp_lite(lowlat::ByteSpan(buffer.data(), buffer.size()), nullptr));
    EXPECT_EQ(lowlat::serialize_rtp_lite(lowlat::RtpLiteHeader{}, nullptr), 0u);
}
