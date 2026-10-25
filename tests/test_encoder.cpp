// 编码器验收测试：把 x264 输出的 Annex-B 码流交给 libavcodec 解码，
// 断言帧数、分辨率、像素格式全部匹配。这等价于播放器能正常解码（无花屏）。

#include "capture/synthetic_source.hpp"
#include "codec/encoder.hpp"
#include "codec/ffmpeg_headers.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

struct DecodeSummary {
    int frames{0};
    int width{0};
    int height{0};
    int pix_fmt{-1};
};

class H264TestDecoder {
public:
    ~H264TestDecoder() { close(); }

    bool open(std::string* error)
    {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) {
            *error = "未找到 H.264 解码器";
            return false;
        }
        context_ = avcodec_alloc_context3(codec);
        if (context_ == nullptr) {
            *error = "avcodec_alloc_context3 失败";
            return false;
        }
        if (avcodec_open2(context_, codec, nullptr) < 0) {
            *error = "avcodec_open2 失败";
            return false;
        }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (frame_ == nullptr || packet_ == nullptr) {
            *error = "AVFrame/AVPacket 分配失败";
            return false;
        }
        return true;
    }

    void close() noexcept
    {
        if (packet_ != nullptr) {
            av_packet_free(&packet_);
        }
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
        }
        if (context_ != nullptr) {
            avcodec_free_context(&context_);
        }
    }

    bool feed(const std::vector<std::uint8_t>& access_unit, DecodeSummary* summary,
              std::string* error)
    {
        if (av_new_packet(packet_, static_cast<int>(access_unit.size())) < 0) {
            *error = "av_new_packet 失败";
            return false;
        }
        std::memcpy(packet_->data, access_unit.data(), access_unit.size());

        const int sent = avcodec_send_packet(context_, packet_);
        av_packet_unref(packet_);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            *error = "avcodec_send_packet 失败";
            return false;
        }
        return drain(summary, error);
    }

    bool flush(DecodeSummary* summary, std::string* error)
    {
        const int sent = avcodec_send_packet(context_, nullptr);
        if (sent < 0 && sent != AVERROR_EOF) {
            *error = "解码器 flush 失败";
            return false;
        }
        return drain(summary, error);
    }

private:
    bool drain(DecodeSummary* summary, std::string* error)
    {
        while (true) {
            const int received = avcodec_receive_frame(context_, frame_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                return true;
            }
            if (received < 0) {
                *error = "avcodec_receive_frame 失败";
                return false;
            }
            if (summary->frames == 0) {
                summary->width = frame_->width;
                summary->height = frame_->height;
                summary->pix_fmt = frame_->format;
            }
            ++summary->frames;
            av_frame_unref(frame_);
        }
    }

    AVCodecContext* context_{nullptr};
    AVFrame* frame_{nullptr};
    AVPacket* packet_{nullptr};
};

// 按帧号把 NAL 聚合成 access unit 后喂给解码器，与真实播放器的行为一致。
bool decode_all(const std::vector<lowlat::Packet>& packets, DecodeSummary* summary,
                std::string* error)
{
    H264TestDecoder decoder;
    if (!decoder.open(error)) {
        return false;
    }

    std::size_t index = 0;
    while (index < packets.size()) {
        const std::uint32_t current = packets[index].frame_index;
        std::vector<std::uint8_t> access_unit;
        while (index < packets.size() && packets[index].frame_index == current) {
            access_unit.insert(access_unit.end(), packets[index].data.begin(),
                               packets[index].data.end());
            ++index;
        }
        if (access_unit.empty()) {
            *error = "空的 access unit";
            return false;
        }
        if (!decoder.feed(access_unit, summary, error)) {
            return false;
        }
    }
    return decoder.flush(summary, error);
}

struct Fixture {
    std::unique_ptr<lowlat::SyntheticSource> source;
    std::unique_ptr<lowlat::H264Encoder> encoder;
    std::vector<lowlat::Packet> packets;
};

bool build_fixture(int width, int height, int fps, int frames, Fixture* fixture,
                   std::string* error)
{
    fixture->source = std::make_unique<lowlat::SyntheticSource>(width, height, fps, false);
    if (!fixture->source->start(error)) {
        return false;
    }

    lowlat::H264Encoder::Config config;
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.bitrate_kbps = 800;
    config.keyint_sec = 1;

    fixture->encoder = std::make_unique<lowlat::H264Encoder>();
    if (!fixture->encoder->open(config, error)) {
        return false;
    }

    for (int i = 0; i < frames; ++i) {
        lowlat::YuvFrame frame;
        if (!fixture->source->next_frame(&frame, error)) {
            return false;
        }
        if (!fixture->encoder->encode(frame, &fixture->packets, error)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(H264Encoder, EncodesAndDecodesBackWithMatchingProperties)
{
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr int kFps = 30;
    constexpr int kFrames = 30;

    Fixture fixture;
    std::string error;
    ASSERT_TRUE(build_fixture(kWidth, kHeight, kFps, kFrames, &fixture, &error)) << error;

    ASSERT_FALSE(fixture.packets.empty());
    for (const auto& packet : fixture.packets) {
        ASSERT_FALSE(packet.data.empty());
        // Annex-B 起始码
        ASSERT_GE(packet.data.size(), 4u);
        const bool four_byte = packet.data[0] == 0 && packet.data[1] == 0 &&
                               packet.data[2] == 0 && packet.data[3] == 1;
        const bool three_byte = packet.data[0] == 0 && packet.data[1] == 0 && packet.data[2] == 1;
        ASSERT_TRUE(four_byte || three_byte);
    }

    DecodeSummary summary;
    ASSERT_TRUE(decode_all(fixture.packets, &summary, &error)) << error;

    EXPECT_EQ(summary.frames, kFrames);
    EXPECT_EQ(summary.width, kWidth);
    EXPECT_EQ(summary.height, kHeight);
    EXPECT_EQ(summary.pix_fmt, AV_PIX_FMT_YUV420P);
}

TEST(H264Encoder, FirstAccessUnitStartsWithIdr)
{
    Fixture fixture;
    std::string error;
    ASSERT_TRUE(build_fixture(160, 120, 30, 5, &fixture, &error)) << error;

    ASSERT_FALSE(fixture.packets.empty());
    bool has_idr = false;
    for (const auto& packet : fixture.packets) {
        if (packet.frame_index == 0 && packet.keyframe) {
            has_idr = true;
        }
        if (packet.frame_index > 0) {
            break;
        }
    }
    EXPECT_TRUE(has_idr);
}

TEST(H264Encoder, RejectsOddDimensions)
{
    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config config;
    config.width = 321;
    config.height = 240;
    config.fps = 30;
    config.bitrate_kbps = 500;

    std::string error;
    EXPECT_FALSE(encoder.open(config, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(encoder.is_open());
}

TEST(H264Encoder, BitrateReconfigKeepsStreamDecodable)
{
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;
    constexpr int kFrames = 20;

    Fixture fixture;
    std::string error;
    ASSERT_TRUE(build_fixture(kWidth, kHeight, 30, kFrames, &fixture, &error)) << error;

    ASSERT_TRUE(fixture.encoder->set_bitrate(400, &error)) << error;
    EXPECT_EQ(fixture.encoder->bitrate_kbps(), 400);

    for (int i = 0; i < 10; ++i) {
        lowlat::YuvFrame frame;
        ASSERT_TRUE(fixture.source->next_frame(&frame, &error)) << error;
        ASSERT_TRUE(fixture.encoder->encode(frame, &fixture.packets, &error)) << error;
    }

    DecodeSummary summary;
    ASSERT_TRUE(decode_all(fixture.packets, &summary, &error)) << error;
    EXPECT_EQ(summary.frames, kFrames + 10);
    EXPECT_EQ(summary.width, kWidth);
    EXPECT_EQ(summary.height, kHeight);
}

TEST(H264Encoder, RuntimeBitrateReconfigActuallyChangesOutput)
{
    // 之前的重配测试只断言了返回值，没有验证输出真的变小——
    // 这等于没测。这里用高熵内容比较重配前后的实际码流大小。
    //
    // 必须用噪声源：彩条画面太平坦，任何目标码率都会编成差不多的大小，
    // 观察不到差异。
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    constexpr int kFramesPerSegment = 40;

    lowlat::SyntheticSource source(kWidth, kHeight, 30, /*realtime=*/false, /*noise=*/60);
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;

    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config config;
    config.width = kWidth;
    config.height = kHeight;
    config.fps = 30;
    config.bitrate_kbps = 3000;
    config.keyint_sec = 2;
    ASSERT_TRUE(encoder.open(config, &error)) << error;

    auto measure = [&](int frames) -> std::size_t {
        std::size_t total = 0;
        for (int i = 0; i < frames; ++i) {
            lowlat::YuvFrame frame;
            EXPECT_TRUE(source.next_frame(&frame, &error)) << error;
            std::vector<lowlat::Packet> packets;
            EXPECT_TRUE(encoder.encode(frame, &packets, &error)) << error;
            for (const auto& packet : packets) {
                total += packet.data.size();
            }
        }
        return total;
    };

    // 先跑一段让码率控制器进入稳态
    measure(30);
    const std::size_t high_segment = measure(kFramesPerSegment);

    ASSERT_TRUE(encoder.set_bitrate(300, &error)) << error;
    // 重配后控制器有惯性（VBV 缓冲、帧级 QP 平滑），丢掉前几帧再统计
    measure(10);
    const std::size_t low_segment = measure(kFramesPerSegment);

    EXPECT_LT(low_segment, high_segment)
        << "重配到 300kbps 后码流没有变小，说明 x264_encoder_reconfig 未生效"
        << "（high=" << high_segment << " low=" << low_segment << "）";
    // 至少要有明显差距，不能只是噪声
    EXPECT_LT(static_cast<double>(low_segment), static_cast<double>(high_segment) * 0.7);
}

TEST(H264Encoder, FlushSucceedsOnConfiguredEncoder)
{
    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config config;
    config.width = 160;
    config.height = 120;
    config.fps = 30;
    config.bitrate_kbps = 500;

    std::string error;
    ASSERT_TRUE(encoder.open(config, &error)) << error;

    std::vector<lowlat::Packet> packets;
    EXPECT_TRUE(encoder.flush(&packets, &error)) << error;
}

TEST(SyntheticSource, ProducesEvenSizedFramesInOrder)
{
    lowlat::SyntheticSource source(64, 48, 30, false);
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;

    for (std::uint32_t expected = 0; expected < 5; ++expected) {
        lowlat::YuvFrame frame;
        ASSERT_TRUE(source.next_frame(&frame, &error)) << error;
        EXPECT_EQ(frame.frame_index, expected);
        EXPECT_EQ(frame.width, 64);
        EXPECT_EQ(frame.height, 48);
        EXPECT_TRUE(frame.valid());
        EXPECT_EQ(frame.byte_size(), 64u * 48u * 3u / 2u);
        EXPECT_GT(frame.capture_ts_us, 0);
    }
    source.stop();

    lowlat::YuvFrame frame;
    EXPECT_FALSE(source.next_frame(&frame, &error));
}

TEST(SyntheticSource, RejectsOddDimensions)
{
    lowlat::SyntheticSource source(65, 48, 30, false);
    std::string error;
    EXPECT_FALSE(source.start(&error));
    EXPECT_FALSE(error.empty());
}
