#include "transport/udp_socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

TEST(UdpEndpoint, ParsesHostAndPort)
{
    lowlat::UdpEndpoint endpoint;
    std::string error;

    ASSERT_TRUE(lowlat::parse_endpoint("127.0.0.1:5000", &endpoint, &error)) << error;
    EXPECT_EQ(endpoint.address, "127.0.0.1");
    EXPECT_EQ(endpoint.port, 5000);
    EXPECT_TRUE(endpoint.valid());
    EXPECT_EQ(endpoint.to_string(), "127.0.0.1:5000");

    ASSERT_TRUE(lowlat::parse_endpoint(":6001", &endpoint, &error)) << error;
    EXPECT_EQ(endpoint.address, "");
    EXPECT_EQ(endpoint.port, 6001);
}

TEST(UdpEndpoint, RejectsMalformedInput)
{
    lowlat::UdpEndpoint endpoint;
    std::string error;

    EXPECT_FALSE(lowlat::parse_endpoint("127.0.0.1", &endpoint, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(lowlat::parse_endpoint("127.0.0.1:", &endpoint, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(lowlat::parse_endpoint("127.0.0.1:0", &endpoint, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(lowlat::parse_endpoint("127.0.0.1:70000", &endpoint, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(lowlat::parse_endpoint("127.0.0.1:abc", &endpoint, &error));
    EXPECT_FALSE(error.empty());
}

TEST(UdpSocket, OpensAndAssignsEphemeralPort)
{
    lowlat::UdpSocket socket;
    std::string error;

    ASSERT_TRUE(socket.open("127.0.0.1", 0, &error)) << error;
    EXPECT_TRUE(socket.is_open());
    EXPECT_GT(socket.local_port(), 0u);
    EXPECT_GE(socket.fd(), 0);

    socket.close();
    EXPECT_FALSE(socket.is_open());
}

TEST(UdpSocket, SendFailsWithoutDestination)
{
    lowlat::UdpSocket socket;
    std::string error;
    ASSERT_TRUE(socket.open("127.0.0.1", 0, &error)) << error;

    const std::uint8_t payload[] = {1, 2, 3};
    EXPECT_FALSE(socket.send(lowlat::ByteSpan(payload, sizeof(payload)), &error));
    EXPECT_FALSE(error.empty());
}

TEST(UdpSocket, RoundTripsSingleDatagram)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(1u * 1024u * 1024u, &error)) << error;

    lowlat::UdpSocket sender;
    ASSERT_TRUE(sender.open("127.0.0.1", 0, &error)) << error;

    lowlat::UdpEndpoint destination;
    destination.address = "127.0.0.1";
    destination.port = receiver.local_port();
    ASSERT_TRUE(sender.set_destination(destination, &error)) << error;

    const std::array<std::uint8_t, 5> payload{10, 20, 30, 40, 50};
    ASSERT_TRUE(sender.send(lowlat::ByteSpan(payload.data(), payload.size()), &error)) << error;

    std::array<std::uint8_t, 2048> buffer{};
    lowlat::UdpEndpoint from;
    std::ptrdiff_t received = 0;
    for (int attempt = 0; attempt < 100 && received == 0; ++attempt) {
        received = receiver.receive(buffer.data(), buffer.size(), &from, &error);
        ASSERT_GE(received, 0) << error;
    }

    ASSERT_EQ(received, static_cast<std::ptrdiff_t>(payload.size()));
    EXPECT_EQ(std::memcmp(buffer.data(), payload.data(), payload.size()), 0);
    EXPECT_EQ(from.address, "127.0.0.1");
    EXPECT_EQ(from.port, sender.local_port());
}

TEST(UdpSocket, ReceiveReturnsZeroWhenEmpty)
{
    lowlat::UdpSocket socket;
    std::string error;
    ASSERT_TRUE(socket.open("127.0.0.1", 0, &error)) << error;

    std::array<std::uint8_t, 64> buffer{};
    const std::ptrdiff_t received = socket.receive(buffer.data(), buffer.size(), nullptr, &error);
    EXPECT_EQ(received, 0);
    EXPECT_TRUE(error.empty());
}

TEST(UdpSocket, SendBatchDeliversAllPackets)
{
    std::string error;

    lowlat::UdpSocket receiver;
    ASSERT_TRUE(receiver.open("127.0.0.1", 0, &error)) << error;
    ASSERT_TRUE(receiver.set_receive_buffer(4u * 1024u * 1024u, &error)) << error;

    lowlat::UdpSocket sender;
    ASSERT_TRUE(sender.open("127.0.0.1", 0, &error)) << error;
    lowlat::UdpEndpoint destination;
    destination.address = "127.0.0.1";
    destination.port = receiver.local_port();
    ASSERT_TRUE(sender.set_destination(destination, &error)) << error;

    constexpr std::size_t kPackets = 64;
    constexpr std::size_t kSize = 1000;
    std::vector<std::vector<std::uint8_t>> payloads(kPackets);
    std::vector<lowlat::ByteSpan> spans;
    for (std::size_t i = 0; i < kPackets; ++i) {
        payloads[i].assign(kSize, static_cast<std::uint8_t>(i & 0xFF));
        payloads[i][0] = static_cast<std::uint8_t>(i);
        spans.push_back(lowlat::ByteSpan(payloads[i].data(), payloads[i].size()));
    }

    const lowlat::SendReport report = sender.send_batch(spans.data(), spans.size(), &error);
    ASSERT_GE(report.sent, 1u) << error;

    std::array<std::uint8_t, 2048> buffer{};
    std::size_t received_count = 0;
    for (int attempt = 0; attempt < 2000 && received_count < report.sent; ++attempt) {
        const std::ptrdiff_t received =
            receiver.receive(buffer.data(), buffer.size(), nullptr, &error);
        ASSERT_GE(received, 0) << error;
        if (received > 0) {
            ASSERT_EQ(received, static_cast<std::ptrdiff_t>(kSize));
            ASSERT_EQ(buffer[0], static_cast<std::uint8_t>(received_count & 0xFF));
            ++received_count;
        }
    }
    EXPECT_EQ(received_count, report.sent);
}

TEST(UdpSocket, MoveTransfersOwnership)
{
    std::string error;
    lowlat::UdpSocket socket;
    ASSERT_TRUE(socket.open("127.0.0.1", 0, &error)) << error;
    const std::uint16_t port = socket.local_port();

    lowlat::UdpSocket moved(std::move(socket));
    EXPECT_TRUE(moved.is_open());
    EXPECT_EQ(moved.local_port(), port);
    EXPECT_FALSE(socket.is_open());
}
