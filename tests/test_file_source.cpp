// 文件采集源的测试。
//
// 测试用的样本码流由本项目自己的编码器现场生成（Annex-B 裸流），
// 不依赖 ffmpeg 命令行，也不往仓库里塞二进制素材。
// FFmpeg 的 h264 裸流 demuxer 能直接读这种文件，正好覆盖文件源主路径。

#include "capture/file_source.hpp"
#include "capture/synthetic_source.hpp"
#include "codec/encoder.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFps = 30;

// 生成一段裸 H.264 文件，返回路径
std::string make_sample_stream(int frames)
{
    const auto path =
        (std::filesystem::temp_directory_path() / "lowlat_file_source_sample.h264").string();

    lowlat::SyntheticSource source(kWidth, kHeight, kFps, /*realtime=*/false);
    std::string error;
    if (!source.start(&error)) {
        return {};
    }

    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config config;
    config.width = kWidth;
    config.height = kHeight;
    config.fps = kFps;
    config.bitrate_kbps = 500;
    config.keyint_sec = 1;
    if (!encoder.open(config, &error)) {
        return {};
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return {};
    }

    for (int i = 0; i < frames; ++i) {
        lowlat::YuvFrame frame;
        if (!source.next_frame(&frame, &error)) {
            return {};
        }
        std::vector<lowlat::Packet> packets;
        if (!encoder.encode(frame, &packets, &error)) {
            return {};
        }
        for (const auto& packet : packets) {
            output.write(reinterpret_cast<const char*>(packet.data.data()),
                         static_cast<std::streamsize>(packet.data.size()));
        }
    }
    output.close();
    return path;
}

}  // namespace

TEST(FileSource, RejectsMissingFile)
{
    lowlat::FileSource source("/nonexistent/definitely-not-here.mp4", 0, false, false);
    std::string error;
    EXPECT_FALSE(source.start(&error));
    EXPECT_FALSE(error.empty());
}

TEST(FileSource, RejectsEmptyPath)
{
    lowlat::FileSource source("", 0, false, false);
    std::string error;
    EXPECT_FALSE(source.start(&error));
}

TEST(FileSource, OpensStreamAndReportsDimensions)
{
    const std::string path = make_sample_stream(10);
    ASSERT_FALSE(path.empty());

    lowlat::FileSource source(path, 0, false, false);
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;

    // 分辨率以文件为准，不是调用方指定的
    EXPECT_EQ(source.width(), kWidth);
    EXPECT_EQ(source.height(), kHeight);
    // 裸 H.264 ES 不带帧率信息，demuxer 会退回猜测值（实测 25），
    // 因此这里只断言"有一个合理的正数"，不写死 30。
    // 需要精确帧率时用 --fps 覆盖。
    EXPECT_GT(source.fps(), 0);
    EXPECT_LE(source.fps(), 120);
    EXPECT_STREQ(source.name(), "file");
}

TEST(FileSource, ProducesAllFramesThenStops)
{
    constexpr int kFrames = 15;
    const std::string path = make_sample_stream(kFrames);
    ASSERT_FALSE(path.empty());

    lowlat::FileSource source(path, 0, /*loop=*/false, /*realtime=*/false);
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;

    int produced = 0;
    lowlat::YuvFrame frame;
    while (source.next_frame(&frame, &error)) {
        ASSERT_TRUE(frame.valid());
        EXPECT_EQ(frame.width, kWidth);
        EXPECT_EQ(frame.height, kHeight);
        ++produced;
        if (produced > kFrames + 5) {
            FAIL() << "输出帧数超过文件实际帧数，可能是结束判断有误";
            break;
        }
    }

    EXPECT_EQ(produced, kFrames);
}

TEST(FileSource, LoopRestartsAfterEnd)
{
    constexpr int kFrames = 10;
    const std::string path = make_sample_stream(kFrames);
    ASSERT_FALSE(path.empty());

    lowlat::FileSource source(path, 0, /*loop=*/true, /*realtime=*/false);
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;

    // 要的帧数超过文件长度，只有循环生效才能全部拿到
    constexpr int kRequested = 25;
    lowlat::YuvFrame frame;
    int produced = 0;
    while (produced < kRequested) {
        if (!source.next_frame(&frame, &error)) {
            break;
        }
        ASSERT_TRUE(frame.valid());
        ++produced;
    }
    EXPECT_EQ(produced, kRequested);
}

TEST(FileSource, HonoursExplicitFpsOverride)
{
    const std::string path = make_sample_stream(5);
    ASSERT_FALSE(path.empty());

    lowlat::FileSource source(path, 15, false, false);  // 显式要求 15fps
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;
    EXPECT_EQ(source.fps(), 15);
}

TEST(FileSource, FrameTimestampsAreMonotonic)
{
    const std::string path = make_sample_stream(6);
    ASSERT_FALSE(path.empty());

    lowlat::FileSource source(path, 0, false, false);
    std::string error;
    ASSERT_TRUE(source.start(&error)) << error;

    std::int64_t previous = 0;
    lowlat::YuvFrame frame;
    while (source.next_frame(&frame, &error)) {
        EXPECT_GT(frame.capture_ts_us, previous);
        previous = frame.capture_ts_us;
    }
}

// 实时源识别：暂停策略依赖它。
//
// 实时源（udp://）暂停时必须继续消费，否则上游缓冲堆积；
// 本地文件暂停时必须停住，不能偷偷前进。两者行为相反，
// 所以这个判定不能错。
TEST(FileSource, RecognizesLiveUrlSources)
{
    lowlat::FileSource live("udp://0.0.0.0:5100?fifo_size=1000000", 30, false, true);
    EXPECT_TRUE(live.is_live());

    lowlat::FileSource rtp("rtp://127.0.0.1:6000", 30, false, true);
    EXPECT_TRUE(rtp.is_live());

    lowlat::FileSource local("/tmp/some_file.mp4", 30, false, true);
    EXPECT_FALSE(local.is_live());

    // 相对路径也不能被误判
    lowlat::FileSource relative("out/demo.mp4", 30, false, true);
    EXPECT_FALSE(relative.is_live());
}
