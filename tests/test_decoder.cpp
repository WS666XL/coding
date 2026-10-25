// 解码器验收：用本项目的 x264 编码器产出真实码流，再交给 H264Decoder 解回来，
// 断言帧数、分辨率、平面指针全部正常。这条链路与收端即将使用的一致。

#include "capture/synthetic_source.hpp"
#include "codec/decoder.hpp"
#include "codec/encoder.hpp"
#include "protocol/h264_fragmenter.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

struct EncodedStream {
    std::vector<lowlat::Packet> packets;
    int width{0};
    int height{0};
    int frames{0};
};

EncodedStream encode_test_stream(int width, int height, int frames)
{
    EncodedStream stream;
    stream.width = width;
    stream.height = height;
    stream.frames = frames;

    lowlat::SyntheticSource source(width, height, 30, false);
    std::string error;
    EXPECT_TRUE(source.start(&error)) << error;

    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config config;
    config.width = width;
    config.height = height;
    config.fps = 30;
    config.bitrate_kbps = 1200;
    config.keyint_sec = 1;
    EXPECT_TRUE(encoder.open(config, &error)) << error;

    for (int i = 0; i < frames; ++i) {
        lowlat::YuvFrame frame;
        EXPECT_TRUE(source.next_frame(&frame, &error)) << error;
        EXPECT_TRUE(encoder.encode(frame, &stream.packets, &error)) << error;
    }
    source.stop();
    return stream;
}

}  // namespace

TEST(H264Decoder, RejectsUseBeforeOpen)
{
    lowlat::H264Decoder decoder;
    EXPECT_FALSE(decoder.is_open());

    std::vector<lowlat::H264Decoder::Frame> frames;
    std::string error;
    const std::uint8_t dummy[4] = {0, 0, 0, 1};

    EXPECT_FALSE(decoder.feed_annexb(lowlat::ByteSpan(dummy, sizeof(dummy)), &frames, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(decoder.flush(&frames, &error));
    EXPECT_FALSE(error.empty());
}

TEST(H264Decoder, OpensAndClosesIdempotently)
{
    lowlat::H264Decoder decoder;
    std::string error;

    ASSERT_TRUE(decoder.open(&error)) << error;
    EXPECT_TRUE(decoder.is_open());

    decoder.close();
    EXPECT_FALSE(decoder.is_open());

    decoder.close();
    EXPECT_FALSE(decoder.is_open());

    // 重新打开应当可以继续用
    ASSERT_TRUE(decoder.open(&error)) << error;
    EXPECT_TRUE(decoder.is_open());
}

TEST(H264Decoder, DecodesAnnexBStreamBackToOriginalFrameCount)
{
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr int kFrames = 10;

    const EncodedStream stream = encode_test_stream(kWidth, kHeight, kFrames);
    ASSERT_FALSE(stream.packets.empty());

    lowlat::H264Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(&error)) << error;

    std::vector<lowlat::H264Decoder::Frame> frames;
    for (const auto& packet : stream.packets) {
        // 编码器的输出已经带 Annex-B 起始码
        ASSERT_TRUE(decoder.feed_annexb(
            lowlat::ByteSpan(packet.data.data(), packet.data.size()), &frames, &error))
            << error;
    }
    ASSERT_TRUE(decoder.flush(&frames, &error)) << error;

    ASSERT_EQ(frames.size(), static_cast<std::size_t>(kFrames));
    for (const auto& frame : frames) {
        EXPECT_EQ(frame.width, kWidth);
        EXPECT_EQ(frame.height, kHeight);
        ASSERT_NE(frame.y, nullptr);
        ASSERT_NE(frame.u, nullptr);
        ASSERT_NE(frame.v, nullptr);
        ASSERT_NE(frame.owner, nullptr);
        // 解码输出的行宽通常 >= 图像宽度（对齐填充），不能假设紧凑
        EXPECT_GE(frame.stride_y, kWidth);
        EXPECT_GE(frame.stride_u, kWidth / 2);
        EXPECT_GE(frame.stride_v, kWidth / 2);
    }
    EXPECT_EQ(decoder.decoded_frames(), static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(decoder.send_errors(), 0u);
}

TEST(H264Decoder, DecodesFromIndividualNalUnits)
{
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;
    constexpr int kFrames = 6;

    const EncodedStream stream = encode_test_stream(kWidth, kHeight, kFrames);
    ASSERT_FALSE(stream.packets.empty());

    lowlat::H264Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(&error)) << error;

    std::vector<lowlat::H264Decoder::Frame> frames;
    std::size_t nal_count = 0;
    for (const auto& packet : stream.packets) {
        const auto nals = lowlat::split_annexb(packet.data.data(), packet.data.size());
        ASSERT_EQ(nals.size(), 1u);
        // 送进去的是裸 NAL（不含起始码），由解码器内部补
        ASSERT_TRUE(decoder.feed_nal(nals[0], &frames, &error)) << error;
        ++nal_count;
    }
    ASSERT_TRUE(decoder.flush(&frames, &error)) << error;

    EXPECT_GT(nal_count, 0u);
    EXPECT_EQ(frames.size(), static_cast<std::size_t>(kFrames));
    for (const auto& frame : frames) {
        EXPECT_EQ(frame.width, kWidth);
        EXPECT_EQ(frame.height, kHeight);
    }
}

TEST(H264Decoder, DecodedPlanesMatchEncodedContent)
{
    // 用合成源的固定内容做一次端到端一致性检查：
    // 解码后的第一帧应当能看出彩条的亮度阶梯（不是全黑或全白）。
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    const EncodedStream stream = encode_test_stream(kWidth, kHeight, 3);

    lowlat::H264Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(&error)) << error;

    std::vector<lowlat::H264Decoder::Frame> frames;
    for (const auto& packet : stream.packets) {
        ASSERT_TRUE(decoder.feed_annexb(
            lowlat::ByteSpan(packet.data.data(), packet.data.size()), &frames, &error))
            << error;
    }
    ASSERT_TRUE(decoder.flush(&frames, &error)) << error;
    ASSERT_FALSE(frames.empty());

    const auto& frame = frames.front();
    const int row = frame.height / 2;

    // 采样 8 条彩条中心点的亮度，应当互不相同（有阶梯）
    std::uint8_t samples[8] = {0};
    for (int i = 0; i < 8; ++i) {
        const int x = static_cast<int>((i + 0.5) * frame.width / 8);
        samples[i] = frame.y[row * frame.stride_y + x];
    }

    int distinct = 0;
    for (int i = 1; i < 8; ++i) {
        if (samples[i] != samples[i - 1]) {
            ++distinct;
        }
    }
    EXPECT_GE(distinct, 6) << "彩条亮度没有形成阶梯，解码结果可能损坏";
}

TEST(H264Decoder, FlushWithoutDataProducesNothing)
{
    lowlat::H264Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(&error)) << error;

    std::vector<lowlat::H264Decoder::Frame> frames;
    ASSERT_TRUE(decoder.flush(&frames, &error)) << error;
    EXPECT_TRUE(frames.empty());
    EXPECT_EQ(decoder.decoded_frames(), 0u);
}

TEST(H264Decoder, FeedNalRejectsEmptyInput)
{
    lowlat::H264Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(&error)) << error;

    std::vector<lowlat::H264Decoder::Frame> frames;
    const std::uint8_t dummy = 0;
    EXPECT_FALSE(decoder.feed_nal(lowlat::ByteSpan(&dummy, 0), &frames, &error));
    EXPECT_FALSE(error.empty());
}
