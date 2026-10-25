// LocalSender（单进程模式的发送端）验证。
//
// 重点验证暂停/恢复：界面上点一次 Pause 之后，必须能靠再点一次恢复。
// 这个路径没有 Qt 依赖，所以能直接放进测试二进制跑。

#include "ui/local_sender.hpp"

#include "capture/synthetic_source.hpp"
#include "codec/encoder.hpp"
#include "transport/udp_socket.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string make_sample_stream(int frames)
{
    const auto path =
        (std::filesystem::temp_directory_path() / "lowlat_local_sender_sample.h264").string();

    lowlat::SyntheticSource source(320, 240, 30, /*realtime=*/false);
    std::string error;
    if (!source.start(&error)) {
        return {};
    }

    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config config;
    config.width = 320;
    config.height = 240;
    config.fps = 30;
    config.bitrate_kbps = 400;
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
    return path;
}

// 等到条件成立或超时
template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace

TEST(LocalSender, RejectsEmptyInputPath)
{
    lowlat::LocalSender sender;
    lowlat::LocalSender::Options options;
    options.destination_port = 5000;

    std::string error;
    EXPECT_FALSE(sender.start(options, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LocalSender, RejectsMissingDestinationPort)
{
    lowlat::LocalSender sender;
    lowlat::LocalSender::Options options;
    options.input = "/tmp/whatever.mp4";

    std::string error;
    EXPECT_FALSE(sender.start(options, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LocalSender, SendsFramesFromFile)
{
    const std::string path = make_sample_stream(90);
    ASSERT_FALSE(path.empty());

    std::string error;
    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;

    lowlat::LocalSender sender;
    lowlat::LocalSender::Options options;
    options.input = path;
    options.destination_port = receiver.local_port();
    options.loop = true;
    options.fec = false;
    options.congestion_control = false;
    ASSERT_TRUE(sender.start(options, &error)) << error;

    EXPECT_TRUE(wait_for([&]() { return sender.frames_sent() >= 5; }, 3000));
    EXPECT_EQ(sender.frame_width(), 320);
    EXPECT_EQ(sender.frame_height(), 240);

    sender.stop();
    EXPECT_FALSE(sender.running());
}

TEST(LocalSender, PauseStopsFrameAdvanceAndResumeContinues)
{
    const std::string path = make_sample_stream(90);
    ASSERT_FALSE(path.empty());

    std::string error;
    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;

    lowlat::LocalSender sender;
    lowlat::LocalSender::Options options;
    options.input = path;
    options.destination_port = receiver.local_port();
    options.loop = true;
    options.fec = false;
    options.congestion_control = false;
    ASSERT_TRUE(sender.start(options, &error)) << error;

    ASSERT_TRUE(wait_for([&]() { return sender.frames_sent() >= 5; }, 3000));

    sender.set_paused(true);
    EXPECT_TRUE(sender.paused());

    // 给线程一点时间进入暂停循环，再取基准
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint64_t paused_at = sender.frames_sent();

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::uint64_t still_paused = sender.frames_sent();

    // 暂停期间最多允许 1 帧在途（正处在发送中的那一帧）
    EXPECT_LE(still_paused - paused_at, 1u) << "暂停后仍在推进帧";

    sender.set_paused(false);
    EXPECT_FALSE(sender.paused());

    // 恢复后必须重新开始推进——这是界面"再点一次"能继续的关键
    EXPECT_TRUE(wait_for([&]() { return sender.frames_sent() > still_paused + 3; }, 3000))
        << "恢复后没有继续发送，暂停/恢复是坏的";

    sender.stop();
}

TEST(LocalSender, SimulatedLossIsApplied)
{
    const std::string path = make_sample_stream(90);
    ASSERT_FALSE(path.empty());

    std::string error;
    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;

    lowlat::LocalSender sender;
    lowlat::LocalSender::Options options;
    options.input = path;
    options.destination_port = receiver.local_port();
    options.loop = true;
    options.fec = false;
    options.congestion_control = false;
    ASSERT_TRUE(sender.start(options, &error)) << error;

    sender.set_simulated_loss_permille(1000);
    EXPECT_EQ(sender.simulated_loss_permille(), 1000);

    EXPECT_TRUE(wait_for([&]() { return sender.simulated_drops() > 0; }, 3000))
        << "设置 100% 丢包后没有任何包被丢弃";

    sender.stop();
}
