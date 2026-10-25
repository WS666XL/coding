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
