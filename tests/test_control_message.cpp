// 反向控制通道的报文编解码。
//
// 这一层容易出隐蔽 bug：类型字节加错位置会让发送端把 NACK 当成反馈、
// 或把反馈当成 NACK（然后按"序号"去重传一堆垃圾包）。

#include "protocol/control_message.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

TEST(ControlMessage, PeekDistinguishesTypes)
{
    lowlat::ControlType type{};

    const std::uint8_t nack_bytes[1] = {static_cast<std::uint8_t>(lowlat::ControlType::Nack)};
    ASSERT_TRUE(lowlat::peek_control_type(lowlat::ByteSpan(nack_bytes, 1), &type));
    EXPECT_EQ(type, lowlat::ControlType::Nack);

    const std::uint8_t feedback_bytes[1] = {
        static_cast<std::uint8_t>(lowlat::ControlType::Feedback)};
    ASSERT_TRUE(lowlat::peek_control_type(lowlat::ByteSpan(feedback_bytes, 1), &type));
    EXPECT_EQ(type, lowlat::ControlType::Feedback);
}

TEST(ControlMessage, PeekRejectsEmptyAndUnknown)
{
    lowlat::ControlType type{};
    const std::uint8_t dummy = 0;

    EXPECT_FALSE(lowlat::peek_control_type(lowlat::ByteSpan(&dummy, 0), &type));
    EXPECT_FALSE(lowlat::peek_control_type(lowlat::ByteSpan(&dummy, 1), &type));  // 0 不是合法类型

    const std::uint8_t unknown[1] = {99};
    EXPECT_FALSE(lowlat::peek_control_type(lowlat::ByteSpan(unknown, 1), &type));
}

TEST(ControlMessage, NackRoundTripWithTypePrefix)
{
    lowlat::NackRequest request;
    request.sequences = {10, 20, 30};

    std::vector<std::uint8_t> buffer(request.serialized_size() + 1, 0);
    const std::size_t written = lowlat::serialize_nack_control(request, buffer.data());

    EXPECT_EQ(written, 1u + 2u + 2u * 3u);
    EXPECT_EQ(buffer[0], static_cast<std::uint8_t>(lowlat::ControlType::Nack));
    EXPECT_EQ(buffer[1], 0x00);  // count 高字节
    EXPECT_EQ(buffer[2], 0x03);  // count 低字节

    lowlat::NackRequest parsed;
    ASSERT_TRUE(lowlat::parse_nack_control(lowlat::ByteSpan(buffer.data(), written), &parsed));
    EXPECT_EQ(parsed.sequences, request.sequences);
}

TEST(ControlMessage, FeedbackRoundTrip)
{
    lowlat::FeedbackReport report;
    report.loss_permille = 37;
    report.samples.push_back({1000, 51000});
    report.samples.push_back({34000, 84000});

    std::vector<std::uint8_t> buffer(report.serialized_size(), 0);
    const std::size_t written = lowlat::serialize_feedback(report, buffer.data());

    EXPECT_EQ(written, 4u + 8u * 2u);
    EXPECT_EQ(buffer[0], static_cast<std::uint8_t>(lowlat::ControlType::Feedback));
    EXPECT_EQ(buffer[1], 2);  // count

    lowlat::FeedbackReport parsed;
    ASSERT_TRUE(lowlat::parse_feedback(lowlat::ByteSpan(buffer.data(), written), &parsed));
    EXPECT_EQ(parsed.loss_permille, 37);
    ASSERT_EQ(parsed.samples.size(), 2u);
    EXPECT_EQ(parsed.samples[0].capture_us, 1000u);
    EXPECT_EQ(parsed.samples[0].arrival_us, 51000u);
    EXPECT_EQ(parsed.samples[1].capture_us, 34000u);
    EXPECT_EQ(parsed.samples[1].arrival_us, 84000u);
}

TEST(ControlMessage, FeedbackTruncatesBeyondMaxSamples)
{
    lowlat::FeedbackReport report;
    for (std::size_t i = 0; i < 50; ++i) {
        report.samples.push_back({static_cast<std::uint32_t>(i),
                                  static_cast<std::uint32_t>(i + 100)});
    }

    std::vector<std::uint8_t> buffer(4 + 8 * 50, 0);
    const std::size_t written = lowlat::serialize_feedback(report, buffer.data());
    EXPECT_EQ(written, 4u + 8u * lowlat::FeedbackReport::kMaxSamples);

    lowlat::FeedbackReport parsed;
    ASSERT_TRUE(lowlat::parse_feedback(lowlat::ByteSpan(buffer.data(), written), &parsed));
    EXPECT_EQ(parsed.samples.size(), lowlat::FeedbackReport::kMaxSamples);
}

TEST(ControlMessage, CrossTypeParsingIsRejected)
{
    // 把反馈报文当 NACK 解析必须失败，反之亦然
    lowlat::FeedbackReport report;
    report.samples.push_back({1, 2});
    std::vector<std::uint8_t> buffer(report.serialized_size(), 0);
    const std::size_t written = lowlat::serialize_feedback(report, buffer.data());

    lowlat::NackRequest nack;
    EXPECT_FALSE(lowlat::parse_nack_control(lowlat::ByteSpan(buffer.data(), written), &nack));

    lowlat::NackRequest request;
    request.sequences = {7};
    std::vector<std::uint8_t> nack_buffer(request.serialized_size() + 1, 0);
    const std::size_t nack_written = lowlat::serialize_nack_control(request, nack_buffer.data());

    lowlat::FeedbackReport parsed_report;
    EXPECT_FALSE(
        lowlat::parse_feedback(lowlat::ByteSpan(nack_buffer.data(), nack_written), &parsed_report));
}

TEST(ControlMessage, RejectsTruncatedPayloads)
{
    lowlat::FeedbackReport report;
    report.samples.push_back({1, 2});
    report.samples.push_back({3, 4});

    std::vector<std::uint8_t> buffer(report.serialized_size(), 0);
    const std::size_t written = lowlat::serialize_feedback(report, buffer.data());

    lowlat::FeedbackReport parsed;
    // 少一个字节就应当拒绝，不能读越界
    EXPECT_FALSE(
        lowlat::parse_feedback(lowlat::ByteSpan(buffer.data(), written - 1), &parsed));

    lowlat::NackRequest nack;
    EXPECT_FALSE(lowlat::parse_nack_control(lowlat::ByteSpan(buffer.data(), 1), &nack));
}

TEST(ControlMessage, EmptyFeedbackIsValid)
{
    lowlat::FeedbackReport report;
    report.loss_permille = 0;

    std::vector<std::uint8_t> buffer(report.serialized_size(), 0);
    const std::size_t written = lowlat::serialize_feedback(report, buffer.data());
    EXPECT_EQ(written, 4u);

    lowlat::FeedbackReport parsed;
    ASSERT_TRUE(lowlat::parse_feedback(lowlat::ByteSpan(buffer.data(), written), &parsed));
    EXPECT_TRUE(parsed.samples.empty());
}
