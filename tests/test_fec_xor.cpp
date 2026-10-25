#include "protocol/fec_xor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

// 造一个内容与长度都可追踪的“数据包”
std::vector<std::uint8_t> make_packet(std::uint16_t sequence, std::size_t size)
{
    std::vector<std::uint8_t> packet(size);
    for (std::size_t i = 0; i < size; ++i) {
        packet[i] = static_cast<std::uint8_t>((sequence * 7u + i * 13u) & 0xFFu);
    }
    return packet;
}

struct GroupResult {
    std::vector<std::vector<std::uint8_t>> packets;
    std::vector<std::uint8_t> repair;
    std::uint16_t base{0};
};

// 编码一组包，返回数据包与修复包
GroupResult build_group(std::uint16_t base, std::size_t group_size, std::size_t first_size)
{
    lowlat::XorFecEncoder::Config config;
    config.group_size = group_size;
    config.max_parity_bytes = 1500;
    lowlat::XorFecEncoder encoder(config);

    GroupResult result;
    result.base = base;
    std::string error;
    bool produced = false;

    for (std::size_t i = 0; i < group_size; ++i) {
        auto packet = make_packet(static_cast<std::uint16_t>(base + i),
                                  first_size + i * 37);
        result.packets.push_back(packet);
        encoder.add(static_cast<std::uint16_t>(base + i),
                    lowlat::ByteSpan(packet.data(), packet.size()), &result.repair, &produced,
                    &error);
    }
    EXPECT_TRUE(produced);
    return result;
}

}  // namespace

TEST(XorFec, EncoderEmitsRepairOnlyWhenGroupFull)
{
    lowlat::XorFecEncoder::Config config;
    config.group_size = 4;
    lowlat::XorFecEncoder encoder(config);

    std::vector<std::uint8_t> repair;
    bool produced = false;
    std::string error;

    for (std::uint16_t i = 0; i < 3; ++i) {
        const auto packet = make_packet(i, 100);
        ASSERT_TRUE(encoder.add(i, lowlat::ByteSpan(packet.data(), packet.size()), &repair,
                                &produced, &error))
            << error;
        EXPECT_FALSE(produced);
        EXPECT_EQ(encoder.pending(), i + 1);
    }

    const auto last = make_packet(3, 120);
    ASSERT_TRUE(encoder.add(3, lowlat::ByteSpan(last.data(), last.size()), &repair, &produced,
                            &error))
        << error;
    EXPECT_TRUE(produced);
    EXPECT_EQ(encoder.pending(), 0u);
    EXPECT_EQ(encoder.groups_emitted(), 1u);
    EXPECT_FALSE(repair.empty());
}

TEST(XorFec, RecoversSingleMissingPacketAtEveryPosition)
{
    constexpr std::size_t kGroupSize = 4;
    const auto group = build_group(100, kGroupSize, 90);

    for (std::size_t missing = 0; missing < kGroupSize; ++missing) {
        lowlat::XorFecDecoder decoder(64);
        for (std::size_t i = 0; i < kGroupSize; ++i) {
            if (i == missing) {
                continue;
            }
            decoder.push_data(static_cast<std::uint16_t>(group.base + i),
                              lowlat::ByteSpan(group.packets[i].data(),
                                               group.packets[i].size()));
        }

        std::vector<std::uint8_t> recovered;
        bool ok = false;
        std::string error;
        ASSERT_TRUE(decoder.recover_with(
            lowlat::ByteSpan(group.repair.data(), group.repair.size()), &recovered, &ok, &error))
            << error;
        ASSERT_TRUE(ok) << "missing=" << missing;
        EXPECT_EQ(recovered, group.packets[missing]) << "missing=" << missing;
        EXPECT_EQ(decoder.stats().recovered_packets, 1u);
    }
}

TEST(XorFec, CannotRecoverTwoMissingPackets)
{
    constexpr std::size_t kGroupSize = 4;
    const auto group = build_group(200, kGroupSize, 64);

    lowlat::XorFecDecoder decoder(64);
    for (std::size_t i = 2; i < kGroupSize; ++i) {
        decoder.push_data(static_cast<std::uint16_t>(group.base + i),
                          lowlat::ByteSpan(group.packets[i].data(), group.packets[i].size()));
    }

    std::vector<std::uint8_t> recovered;
    bool ok = false;
    std::string error;
    ASSERT_TRUE(decoder.recover_with(
        lowlat::ByteSpan(group.repair.data(), group.repair.size()), &recovered, &ok, &error))
        << error;
    EXPECT_FALSE(ok);
    EXPECT_EQ(decoder.stats().repairs_unusable, 1u);
    EXPECT_EQ(decoder.stats().recovered_packets, 0u);
}

TEST(XorFec, NothingMissingMeansNothingToRecover)
{
    constexpr std::size_t kGroupSize = 4;
    const auto group = build_group(300, kGroupSize, 80);

    lowlat::XorFecDecoder decoder(64);
    for (std::size_t i = 0; i < kGroupSize; ++i) {
        decoder.push_data(static_cast<std::uint16_t>(group.base + i),
                          lowlat::ByteSpan(group.packets[i].data(), group.packets[i].size()));
    }

    std::vector<std::uint8_t> recovered;
    bool ok = false;
    std::string error;
    ASSERT_TRUE(decoder.recover_with(
        lowlat::ByteSpan(group.repair.data(), group.repair.size()), &recovered, &ok, &error))
        << error;
    EXPECT_FALSE(ok);
    EXPECT_EQ(decoder.stats().repairs_used, 0u);
}

TEST(XorFec, RepairHeaderIsBigEndianAndCarriesLengths)
{
    constexpr std::size_t kGroupSize = 3;
    const auto group = build_group(0x1234, kGroupSize, 100);

    ASSERT_GE(group.repair.size(), 4u + 2u * kGroupSize);
    // base_sequence 网络序
    EXPECT_EQ(group.repair[0], 0x12);
    EXPECT_EQ(group.repair[1], 0x34);
    EXPECT_EQ(group.repair[2], static_cast<std::uint8_t>(kGroupSize));
    EXPECT_EQ(group.repair[3], 0u);

    // 长度表逐项对应
    for (std::size_t i = 0; i < kGroupSize; ++i) {
        const std::uint16_t length =
            static_cast<std::uint16_t>((group.repair[4 + 2 * i] << 8) | group.repair[4 + 2 * i + 1]);
        EXPECT_EQ(length, group.packets[i].size()) << "i=" << i;
    }
}

TEST(XorFec, ConsecutiveGroupsDoNotMix)
{
    lowlat::XorFecEncoder::Config config;
    config.group_size = 3;
    config.max_parity_bytes = 1500;
    lowlat::XorFecEncoder encoder(config);

    std::vector<std::vector<std::uint8_t>> repairs;
    std::vector<std::vector<std::uint8_t>> packets;
    std::string error;

    for (std::uint16_t i = 0; i < 6; ++i) {
        auto packet = make_packet(i, 100 + i * 10);
        packets.push_back(packet);
        std::vector<std::uint8_t> repair;
        bool produced = false;
        ASSERT_TRUE(encoder.add(i, lowlat::ByteSpan(packet.data(), packet.size()), &repair,
                                &produced, &error))
            << error;
        if (produced) {
            repairs.push_back(repair);
        }
    }
    ASSERT_EQ(repairs.size(), 2u);

    // 第二组（序列 3,4,5）丢第 4 号包
    lowlat::XorFecDecoder decoder(64);
    decoder.push_data(3, lowlat::ByteSpan(packets[3].data(), packets[3].size()));
    decoder.push_data(5, lowlat::ByteSpan(packets[5].data(), packets[5].size()));

    std::vector<std::uint8_t> recovered;
    bool ok = false;
    ASSERT_TRUE(decoder.recover_with(lowlat::ByteSpan(repairs[1].data(), repairs[1].size()),
                                     &recovered, &ok, &error))
        << error;
    ASSERT_TRUE(ok);
    EXPECT_EQ(recovered, packets[4]);
}

TEST(XorFec, HandlesSequenceWraparound)
{
    constexpr std::size_t kGroupSize = 4;
    const std::uint16_t base = 65534;  // 组内序列：65534, 65535, 0, 1
    const auto group = build_group(base, kGroupSize, 70);

    // 丢掉 0 号包（在组内是第 2 个）
    constexpr std::size_t kMissing = 2;
    lowlat::XorFecDecoder decoder(64);
    for (std::size_t i = 0; i < kGroupSize; ++i) {
        if (i == kMissing) {
            continue;
        }
        decoder.push_data(static_cast<std::uint16_t>(base + i),
                          lowlat::ByteSpan(group.packets[i].data(), group.packets[i].size()));
    }

    std::vector<std::uint8_t> recovered;
    bool ok = false;
    std::string error;
    ASSERT_TRUE(decoder.recover_with(
        lowlat::ByteSpan(group.repair.data(), group.repair.size()), &recovered, &ok, &error))
        << error;
    ASSERT_TRUE(ok);
    EXPECT_EQ(recovered, group.packets[kMissing]);
}

TEST(XorFec, RejectsOversizedRepair)
{
    lowlat::XorFecEncoder::Config config;
    config.group_size = 8;
    config.max_parity_bytes = 64;  // 头部就要 4+16=20 字节，装不下 100 字节负载
    lowlat::XorFecEncoder encoder(config);

    std::vector<std::uint8_t> repair;
    bool produced = false;
    std::string error;
    bool failed = false;

    for (std::uint16_t i = 0; i < 8; ++i) {
        const auto packet = make_packet(i, 100);
        if (!encoder.add(i, lowlat::ByteSpan(packet.data(), packet.size()), &repair, &produced,
                         &error)) {
            failed = true;
            break;
        }
    }
    EXPECT_TRUE(failed);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(encoder.pending(), 0u);  // 失败后组已作废
}

TEST(XorFec, DecoderWindowEvictionPreventsRecovery)
{
    constexpr std::size_t kGroupSize = 4;
    const auto group = build_group(400, kGroupSize, 50);

    // 窗口只有 2：4 个包里序列号奇偶相同的会互相覆盖
    lowlat::XorFecDecoder decoder(2);
    for (std::size_t i = 1; i < kGroupSize; ++i) {
        decoder.push_data(static_cast<std::uint16_t>(group.base + i),
                          lowlat::ByteSpan(group.packets[i].data(), group.packets[i].size()));
    }

    std::vector<std::uint8_t> recovered;
    bool ok = false;
    std::string error;
    ASSERT_TRUE(decoder.recover_with(
        lowlat::ByteSpan(group.repair.data(), group.repair.size()), &recovered, &ok, &error))
        << error;
    EXPECT_FALSE(ok);
    EXPECT_GT(decoder.stats().repairs_unusable, 0u);
}

TEST(XorFec, RejectsEmptyAndShortRepair)
{
    lowlat::XorFecDecoder decoder;
    std::vector<std::uint8_t> recovered;
    bool ok = false;
    std::string error;
    const std::uint8_t dummy = 0;

    EXPECT_FALSE(decoder.recover_with(lowlat::ByteSpan(&dummy, 0), &recovered, &ok, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    const std::uint8_t too_short[3] = {0, 0, 1};
    EXPECT_FALSE(decoder.recover_with(lowlat::ByteSpan(too_short, 3), &recovered, &ok, &error));
    EXPECT_FALSE(error.empty());
}

TEST(XorFec, EncoderRejectsEmptyPacket)
{
    lowlat::XorFecEncoder encoder;
    std::vector<std::uint8_t> repair;
    bool produced = false;
    std::string error;
    const std::uint8_t dummy = 0;

    EXPECT_FALSE(encoder.add(0, lowlat::ByteSpan(&dummy, 0), &repair, &produced, &error));
    EXPECT_FALSE(error.empty());
}
