// StreamSender 验收：用一个真实的 UdpSocket 收包，把 RTP-lite 头解出来，
// 再用 FuADepacketizer 还原 NAL，断言与输入完全一致。
// 这条链路覆盖了"打包 -> 线上字节 -> 解包"的完整往返。

#include "protocol/h264_fragmenter.hpp"
#include "protocol/rtp_lite.hpp"
#include "transport/stream_sender.hpp"
#include "transport/udp_socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct ReceivedPacket {
    lowlat::RtpLiteHeader header;
    std::vector<std::uint8_t> payload;
};

// 非阻塞收包，轮询直到收满 expected 个或超时。
std::vector<ReceivedPacket> drain(lowlat::UdpSocket* socket, std::size_t expected)
{
    std::vector<ReceivedPacket> packets;
    std::array<std::uint8_t, 2048> buffer{};
    std::string error;

    for (int attempt = 0; attempt < 5000 && packets.size() < expected; ++attempt) {
        const std::ptrdiff_t received = socket->receive(buffer.data(), buffer.size(), nullptr, &error);
        if (received <= 0) {
            continue;
        }
        lowlat::RtpLiteHeader header;
        if (!lowlat::parse_rtp_lite(
                lowlat::ByteSpan(buffer.data(), static_cast<std::size_t>(received)), &header)) {
            continue;
        }
        ReceivedPacket packet;
        packet.header = header;
        const std::size_t header_size = header.serialized_size();
        packet.payload.assign(buffer.begin() + static_cast<std::ptrdiff_t>(header_size),
                              buffer.begin() + received);
        packets.push_back(std::move(packet));
    }
    return packets;
}

std::vector<std::uint8_t> make_nal(std::uint8_t header, std::size_t size)
{
    std::vector<std::uint8_t> nal(size);
    nal[0] = header;
    for (std::size_t i = 1; i < size; ++i) {
        nal[i] = static_cast<std::uint8_t>((i * 17u + 3u) & 0xFFu);
    }
    return nal;
}

}  // namespace

TEST(StreamSender, SendsSingleNalWithCorrectHeader)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(2u * 1024u * 1024u, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();
    config.ssrc = 0xAABBCCDDu;
    config.max_payload = 1200;

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;

    const auto nal = make_nal(0x41, 300);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};

    ASSERT_TRUE(sender.send_frame(nals, 7, 21000, 0, &error)) << error;
    EXPECT_EQ(sender.packets_sent(), 1u);
    EXPECT_EQ(sender.packets_dropped(), 0u);

    const auto packets = drain(&receiver, 1);
    ASSERT_EQ(packets.size(), 1u);

    EXPECT_EQ(packets[0].header.sequence, 0u);
    EXPECT_EQ(packets[0].header.timestamp, 21000u);
    EXPECT_EQ(packets[0].header.ssrc, 0xAABBCCDDu);
    EXPECT_EQ(packets[0].header.frame_index, 7u);
    EXPECT_TRUE(packets[0].header.marker);
    EXPECT_EQ(packets[0].payload, nal);
}

TEST(StreamSender, LargeNalIsFragmentedAndReassembles)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(4u * 1024u * 1024u, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();
    config.max_payload = 1200;

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;

    const auto nal = make_nal(0x65, 5000);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};
    ASSERT_TRUE(sender.send_frame(nals, 1, 3000, 0, &error)) << error;

    ASSERT_GT(sender.packets_sent(), 1u);
    const auto packets = drain(&receiver, sender.packets_sent());
    ASSERT_EQ(packets.size(), sender.packets_sent());

    // 序列号递增、只有最后一包带 marker、时间戳一致
    for (std::size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].header.sequence, static_cast<std::uint16_t>(i));
        EXPECT_EQ(packets[i].header.timestamp, 3000u);
        EXPECT_EQ(packets[i].header.frame_index, 1u);
        EXPECT_EQ(packets[i].header.marker, i + 1 == packets.size());
        EXPECT_LE(packets[i].payload.size(), 1200u - 16u);
    }

    lowlat::FuADepacketizer depacketizer;
    std::vector<std::uint8_t> reassembled;
    bool got = false;
    for (const auto& packet : packets) {
        std::vector<std::uint8_t> produced;
        bool produced_flag = false;
        ASSERT_TRUE(depacketizer.feed(
            lowlat::ByteSpan(packet.payload.data(), packet.payload.size()), packet.header.timestamp,
            &produced, &produced_flag, &error))
            << error;
        if (produced_flag) {
            reassembled = std::move(produced);
            got = true;
        }
    }
    ASSERT_TRUE(got);
    EXPECT_EQ(reassembled, nal);
}

TEST(StreamSender, MultiNalFrameMarksOnlyLastPacket)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(4u * 1024u * 1024u, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;

    const auto sps = make_nal(0x67, 20);
    const auto pps = make_nal(0x68, 8);
    const auto idr = make_nal(0x65, 3000);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(sps.data(), sps.size()),
                                             lowlat::ByteSpan(pps.data(), pps.size()),
                                             lowlat::ByteSpan(idr.data(), idr.size())};

    ASSERT_TRUE(sender.send_frame(nals, 42, 90000, 0, &error)) << error;
    const std::uint64_t total = sender.packets_sent();
    ASSERT_GT(total, 2u);

    const auto packets = drain(&receiver, total);
    ASSERT_EQ(packets.size(), total);

    std::size_t marker_count = 0;
    for (const auto& packet : packets) {
        if (packet.header.marker) {
            ++marker_count;
            EXPECT_EQ(&packet, &packets.back());
        }
        EXPECT_EQ(packet.header.frame_index, 42u);
        EXPECT_EQ(packet.header.timestamp, 90000u);
    }
    EXPECT_EQ(marker_count, 1u);
}

TEST(StreamSender, SequenceWrapsAcrossFrames)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(4u * 1024u * 1024u, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;

    const auto nal = make_nal(0x41, 200);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};

    for (std::uint32_t frame = 0; frame < 3; ++frame) {
        ASSERT_TRUE(sender.send_frame(nals, frame, frame * 3000, 0, &error)) << error;
    }
    EXPECT_EQ(sender.next_sequence(), 3u);

    const auto packets = drain(&receiver, 3);
    ASSERT_EQ(packets.size(), 3u);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].header.sequence, static_cast<std::uint16_t>(i));
        EXPECT_EQ(packets[i].header.frame_index, static_cast<std::uint32_t>(i));
    }
}

TEST(StreamSender, RejectsInvalidConfig)
{
    lowlat::StreamSender sender;
    std::string error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = 0;  // 非法
    EXPECT_FALSE(sender.open(config, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    config.destination.port = 5000;
    config.max_payload = 10;  // 装不下 16 字节头
    EXPECT_FALSE(sender.open(config, &error));
    EXPECT_FALSE(error.empty());
}

TEST(StreamSender, RejectsEmptyNalList)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;

    const std::vector<lowlat::ByteSpan> empty;
    EXPECT_FALSE(sender.send_frame(empty, 0, 0, 0, &error));
    EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------- 模拟丢包
//
// 界面上的"Loss: 5%"档位走的就是这条路径。这里验证它的边界行为：
// 1000‰ 必须一个包都不发出去，0‰ 必须一个都不丢。

TEST(StreamSender, SimulatedLossOfThousandDropsEverything)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;

    sender.set_simulated_loss_permille(1000);

    const auto nal = make_nal(0x41, 500);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};
    (void)sender.send_frame(nals, 1, 3000, 0, &error);

    EXPECT_EQ(sender.packets_sent(), 0u);
    EXPECT_GT(sender.simulated_drops(), 0u);
    EXPECT_TRUE(drain(&receiver, 1).empty());
}

TEST(StreamSender, SimulatedLossOfZeroSendsEverything)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;
    sender.set_simulated_loss_permille(0);

    const auto nal = make_nal(0x41, 500);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};
    ASSERT_TRUE(sender.send_frame(nals, 1, 3000, 0, &error)) << error;

    EXPECT_EQ(sender.packets_sent(), 1u);
    EXPECT_EQ(sender.simulated_drops(), 0u);
}

TEST(StreamSender, SimulatedLossIsClampedToValidRange)
{
    std::string error;
    lowlat::StreamSender sender;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = 9;  // 只用于打开，不需要真的可达
    ASSERT_TRUE(sender.open(config, &error)) << error;

    sender.set_simulated_loss_permille(-5);
    EXPECT_EQ(sender.simulated_loss_permille(), 0);

    sender.set_simulated_loss_permille(5000);
    EXPECT_EQ(sender.simulated_loss_permille(), 1000);
}

TEST(StreamSender, SimulatedLossIsApproximatelyRequestedRatio)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(4u * 1024u * 1024u, &error)) << error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = receiver.local_port();

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;
    sender.set_simulated_loss_permille(200);  // 20%

    // 发 100 帧，每帧 1 个包，统计实际丢弃比例
    const auto nal = make_nal(0x41, 200);
    const std::vector<lowlat::ByteSpan> nals{lowlat::ByteSpan(nal.data(), nal.size())};
    for (std::uint32_t i = 0; i < 100; ++i) {
        (void)sender.send_frame(nals, i, i * 3000, 0, &error);
    }

    const std::uint64_t drops = sender.simulated_drops();
    const std::uint64_t sent = sender.packets_sent();
    ASSERT_EQ(drops + sent, 100u);

    // 20% 目标，允许较宽的区间：样本只有 100 个，且用的是确定性 PRNG
    EXPECT_GE(drops, 8u);
    EXPECT_LE(drops, 34u);
}

// 未指定 SSRC 时每个发送端必须拿到不同的值。
//
// 界面里换文件会新建发送端；如果 SSRC 重复，接收端无法识别"这是新流"，
// 会把序列号从 0 重新开始的包全当作倒退的重传包，画面就冻住了。
TEST(StreamSender, AutoGeneratesDistinctSsrcPerInstance)
{
    std::string error;

    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = 9;  // 打开即可，不需要真的发出去

    lowlat::StreamSender first;
    ASSERT_TRUE(first.open(config, &error)) << error;
    lowlat::StreamSender second;
    ASSERT_TRUE(second.open(config, &error)) << error;

    EXPECT_NE(first.ssrc(), 0u);
    EXPECT_NE(second.ssrc(), 0u);
    EXPECT_NE(first.ssrc(), second.ssrc()) << "两次新建的发送端 SSRC 相同，换流无法被识别";
}

TEST(StreamSender, HonoursExplicitSsrc)
{
    std::string error;
    lowlat::StreamSender::Config config;
    config.destination.address = "127.0.0.1";
    config.destination.port = 9;
    config.ssrc = 0xDEADBEEFu;

    lowlat::StreamSender sender;
    ASSERT_TRUE(sender.open(config, &error)) << error;
    EXPECT_EQ(sender.ssrc(), 0xDEADBEEFu);
}
