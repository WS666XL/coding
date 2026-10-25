// StreamReceiver 集成测试：用真实的 StreamSender 通过 loopback UDP 发帧，
// 验证"打包 → 传输 → 重组 → 按帧交付"整条链路。
// 这是把接收流水线从 receiver_main 抽出来之后必须补的测试。

#include "transport/stream_receiver.hpp"
#include "transport/stream_sender.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> make_nal(std::uint8_t header, std::size_t size, std::uint8_t seed)
{
    std::vector<std::uint8_t> nal(size);
    nal[0] = header;
    for (std::size_t i = 1; i < size; ++i) {
        nal[i] = static_cast<std::uint8_t>((i * 31u + seed) & 0xFFu);
    }
    return nal;
}

struct ReceiverFixture {
    lowlat::StreamReceiver receiver;
    lowlat::StreamSender sender;
    bool ok{false};
};

bool setup(ReceiverFixture* fixture, int hold_ms, std::string* error)
{
    lowlat::StreamReceiver::Options options;
    options.bind_address = "127.0.0.1";
    options.bind_port = 0;
    options.frame_hold_ms = hold_ms;
    options.enable_nack = false;
    if (!fixture->receiver.open(options, error)) {
        return false;
    }

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = fixture->receiver.local_port();
    return fixture->sender.open(config, error);
}

// 反复 poll 直到拿到 expected 帧或超时
std::vector<lowlat::ReceivedFrame> collect(lowlat::StreamReceiver* receiver,
                                           std::size_t expected, int max_rounds = 200)
{
    std::vector<lowlat::ReceivedFrame> all;
    std::string error;
    for (int i = 0; i < max_rounds && all.size() < expected; ++i) {
        std::vector<lowlat::ReceivedFrame> batch;
        receiver->poll(5, &batch, &error);
        for (auto& frame : batch) {
            all.push_back(std::move(frame));
        }
    }
    return all;
}

}  // namespace

TEST(StreamReceiver, RejectsUseBeforeOpen)
{
    lowlat::StreamReceiver receiver;
    EXPECT_FALSE(receiver.is_open());

    std::vector<lowlat::ReceivedFrame> frames;
    std::string error;
    EXPECT_FALSE(receiver.poll(0, &frames, &error));
    EXPECT_FALSE(error.empty());
}

TEST(StreamReceiver, DeliversCompleteFrameWithOriginalNals)
{
    ReceiverFixture fixture;
    std::string error;
    ASSERT_TRUE(setup(&fixture, 0, &error)) << error;

    const auto sps = make_nal(0x67, 20, 1);
    const auto pps = make_nal(0x68, 8, 2);
    const auto idr = make_nal(0x65, 3000, 3);  // 超过 MTU，会被 FU-A 分片
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(sps.data(), sps.size()),
                                             lowlat::ByteSpan(pps.data(), pps.size()),
                                             lowlat::ByteSpan(idr.data(), idr.size())};

    ASSERT_TRUE(fixture.sender.send_frame(nals, 42, 90000, 123456, &error)) << error;

    const auto frames = collect(&fixture.receiver, 1);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].frame_index, 42u);
    EXPECT_EQ(frames[0].capture_ts_us, 123456u);

    // NAL 内容与顺序必须和发送端一致（含分片再重组的那一个）
    ASSERT_EQ(frames[0].nals.size(), 3u);
    EXPECT_EQ(frames[0].nals[0], sps);
    EXPECT_EQ(frames[0].nals[1], pps);
    EXPECT_EQ(frames[0].nals[2], idr);

    const auto stats = fixture.receiver.snapshot();
    EXPECT_EQ(stats.distinct_frames, 1u);
    EXPECT_GT(stats.packets, 0u);
    EXPECT_GT(stats.nals_emitted, 0u);
    EXPECT_EQ(stats.malformed, 0u);
    EXPECT_EQ(stats.frame_gaps, 0u);
}

TEST(StreamReceiver, DeliversMultipleFramesInOrder)
{
    ReceiverFixture fixture;
    std::string error;
    ASSERT_TRUE(setup(&fixture, 0, &error)) << error;

    const auto nal = make_nal(0x41, 400, 7);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};

    for (std::uint32_t frame = 0; frame < 5; ++frame) {
        ASSERT_TRUE(fixture.sender.send_frame(nals, frame, frame * 3000, 1000 + frame, &error))
            << error;
    }

    const auto frames = collect(&fixture.receiver, 5);
    ASSERT_EQ(frames.size(), 5u);
    for (std::uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(frames[i].frame_index, i);
        EXPECT_EQ(frames[i].capture_ts_us, 1000u + i);
        ASSERT_EQ(frames[i].nals.size(), 1u);
        EXPECT_EQ(frames[i].nals[0], nal);
    }

    const auto stats = fixture.receiver.snapshot();
    EXPECT_EQ(stats.distinct_frames, 5u);
    EXPECT_EQ(stats.frame_gaps, 0u);
    EXPECT_EQ(stats.sequence_breaks, 0u);
}

TEST(StreamReceiver, FlushEmitsHeldFrameImmediately)
{
    ReceiverFixture fixture;
    std::string error;
    // 保持窗口设得很大：正常情况下这一帧不会在 poll 里立刻交付
    ASSERT_TRUE(setup(&fixture, 5000, &error)) << error;

    const auto nal = make_nal(0x41, 200, 5);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};
    ASSERT_TRUE(fixture.sender.send_frame(nals, 9, 27000, 0, &error)) << error;

    // 先收包但不期待交付
    std::vector<lowlat::ReceivedFrame> frames;
    for (int i = 0; i < 20 && frames.empty(); ++i) {
        fixture.receiver.poll(5, &frames, &error);
    }
    EXPECT_TRUE(frames.empty()) << "保持窗口未到期，不应提前交付";

    // flush 必须把它立刻吐出来
    frames.clear();
    fixture.receiver.flush(&frames);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frame_index, 9u);
    ASSERT_EQ(frames.front().nals.size(), 1u);
    EXPECT_EQ(frames.front().nals[0], nal);
}

TEST(StreamReceiver, RecordsLatencyWhenCaptureTimestampPresent)
{
    ReceiverFixture fixture;
    std::string error;
    ASSERT_TRUE(setup(&fixture, 0, &error)) << error;

    // 采集时间戳设为"当前时刻"的低 32 位，延迟应接近 0 而不是天文数字
    const std::int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
    const auto capture_ts = static_cast<std::uint32_t>(now_us);

    const auto nal = make_nal(0x41, 100, 3);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};
    ASSERT_TRUE(fixture.sender.send_frame(nals, 1, 3000, capture_ts, &error)) << error;

    const auto frames = collect(&fixture.receiver, 1);
    ASSERT_EQ(frames.size(), 1u);

    const auto stats = fixture.receiver.snapshot();
    ASSERT_EQ(stats.latency_samples, 1u);
    EXPECT_LT(stats.latency_p50_ms, 1000.0);
}

// 界面里"换文件 / 重新开始"会销毁旧发送端、新建一个。
// 新发送端的序列号从 0 重新开始，如果接收端不做换流检测，
// 会把所有包判定为"序列号倒退的重传包"，画面永远不再推进。
TEST(StreamReceiver, AcceptsFramesFromANewSenderOnSamePort)
{
    std::string error;

    lowlat::StreamReceiver receiver;
    lowlat::StreamReceiver::Options options;
    options.bind_address = "127.0.0.1";
    options.bind_port = 0;
    options.frame_hold_ms = 0;
    ASSERT_TRUE(receiver.open(options, &error)) << error;

    const auto nal = make_nal(0x41, 400, 1);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};

    auto run_sender = [&](std::uint32_t ssrc, std::uint32_t first_frame) {
        lowlat::StreamSender::Config config;
        config.destination.address = "127.0.0.1";
        config.destination.port = receiver.local_port();
        config.ssrc = ssrc;

        lowlat::StreamSender sender;
        if (!sender.open(config, &error)) {
            return false;
        }
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (!sender.send_frame(nals, first_frame + i, (first_frame + i) * 3000, 1000, &error)) {
                return false;
            }
        }
        return true;
    };

    // 第一条流
    ASSERT_TRUE(run_sender(0x11111111u, 0));
    const auto first = collect(&receiver, 3);
    ASSERT_EQ(first.size(), 3u) << "第一条流没有收到帧";

    // 换一个新发送端（模拟界面里换文件），SSRC 不同、序列号从 0 开始
    ASSERT_TRUE(run_sender(0x22222222u, 100));
    const auto second = collect(&receiver, 3);
    ASSERT_EQ(second.size(), 3u) << "换流后帧不再推进——换流检测失效";
    EXPECT_EQ(second.front().frame_index, 100u);
}
