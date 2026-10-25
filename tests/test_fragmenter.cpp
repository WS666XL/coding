#include "protocol/h264_fragmenter.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> make_nal(std::uint8_t header, std::size_t size)
{
    std::vector<std::uint8_t> nal(size);
    nal[0] = header;
    for (std::size_t i = 1; i < size; ++i) {
        nal[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    return nal;
}

// 走一遍"分片 -> 重组"，返回重组出的 NAL。
bool roundtrip(const std::vector<std::uint8_t>& nal, std::size_t max_payload,
               std::vector<std::uint8_t>* reassembled, std::size_t* slice_count)
{
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;
    if (!lowlat::fragment_nal(lowlat::ByteSpan(nal.data(), nal.size()), max_payload, &slices,
                              &error)) {
        return false;
    }
    if (slice_count != nullptr) {
        *slice_count = slices.size();
    }

    lowlat::FuADepacketizer depacketizer;
    bool got = false;
    for (const auto& slice : slices) {
        std::vector<std::uint8_t> produced;
        bool produced_flag = false;
        if (!depacketizer.feed(lowlat::ByteSpan(slice.data.data(), slice.data.size()), 1000,
                               &produced, &produced_flag, &error)) {
            return false;
        }
        if (produced_flag) {
            *reassembled = std::move(produced);
            got = true;
        }
    }
    return got;
}

}  // namespace

TEST(AnnexB, SplitsThreeByteStartCodes)
{
    const std::uint8_t data[] = {0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x68, 0xBB, 0xCC};
    const auto nals = lowlat::split_annexb(data, sizeof(data));

    ASSERT_EQ(nals.size(), 2u);
    EXPECT_EQ(nals[0].size(), 2u);
    EXPECT_EQ(nals[0][0], 0x67);
    EXPECT_EQ(nals[1].size(), 3u);
    EXPECT_EQ(nals[1][0], 0x68);
}

TEST(AnnexB, SplitsFourByteStartCodes)
{
    const std::uint8_t data[] = {0, 0, 0, 1, 0x67, 0x11, 0, 0, 0, 1, 0x68, 0x22, 0x33};
    const auto nals = lowlat::split_annexb(data, sizeof(data));

    ASSERT_EQ(nals.size(), 2u);
    EXPECT_EQ(nals[0].size(), 2u);
    EXPECT_EQ(nals[1].size(), 3u);
}

TEST(AnnexB, SplitsMixedStartCodesAndStripsTrailingZeros)
{
    // 第二个 NAL 前的 4 字节起始码会带来一个额外的 0，必须剥离
    const std::uint8_t data[] = {0, 0, 0, 1, 0x67, 0x00, 0, 0, 0, 1, 0x68, 0x99};
    const auto nals = lowlat::split_annexb(data, sizeof(data));

    ASSERT_EQ(nals.size(), 2u);
    EXPECT_EQ(nals[0].size(), 1u);
    EXPECT_EQ(nals[0][0], 0x67);
    EXPECT_EQ(nals[1].size(), 2u);
    EXPECT_EQ(nals[1][0], 0x68);
}

TEST(AnnexB, ReturnsEmptyForNoStartCode)
{
    const std::uint8_t data[] = {0x11, 0x22, 0x33, 0x44};
    EXPECT_TRUE(lowlat::split_annexb(data, sizeof(data)).empty());
    EXPECT_TRUE(lowlat::split_annexb(nullptr, 0).empty());
}

TEST(AnnexB, ClassifiesNalTypes)
{
    const std::uint8_t idr[] = {0x65, 0x00};
    const std::uint8_t sps[] = {0x67, 0x00};
    const std::uint8_t slice[] = {0x41, 0x00};

    EXPECT_EQ(lowlat::nal_type_of(lowlat::ByteSpan(idr, 2)), lowlat::NalType::kIdr);
    EXPECT_EQ(lowlat::nal_type_of(lowlat::ByteSpan(sps, 2)), lowlat::NalType::kSps);
    EXPECT_TRUE(lowlat::nal_is_keyframe(lowlat::ByteSpan(idr, 2)));
    EXPECT_TRUE(lowlat::nal_is_keyframe(lowlat::ByteSpan(sps, 2)));
    EXPECT_FALSE(lowlat::nal_is_keyframe(lowlat::ByteSpan(slice, 2)));
}

TEST(Fragmenter, SmallNalIsSinglePacket)
{
    const auto nal = make_nal(0x65, 100);
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;

    ASSERT_TRUE(lowlat::fragment_nal(lowlat::ByteSpan(nal.data(), nal.size()), 1200, &slices,
                                     &error))
        << error;

    ASSERT_EQ(slices.size(), 1u);
    EXPECT_FALSE(slices[0].fu_a_start);
    EXPECT_FALSE(slices[0].fu_a_end);
    EXPECT_EQ(slices[0].data, nal);
}

TEST(Fragmenter, LargeNalProducesFuASlices)
{
    const auto nal = make_nal(0x65, 5000);
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;

    ASSERT_TRUE(lowlat::fragment_nal(lowlat::ByteSpan(nal.data(), nal.size()), 1200, &slices,
                                     &error))
        << error;

    EXPECT_GT(slices.size(), 1u);
    EXPECT_TRUE(slices.front().fu_a_start);
    EXPECT_FALSE(slices.front().fu_a_end);
    EXPECT_TRUE(slices.back().fu_a_end);
    EXPECT_FALSE(slices.back().fu_a_start);

    for (const auto& slice : slices) {
        EXPECT_LE(slice.data.size(), 1200u);
        // FU indicator 的类型字段必须是 28
        EXPECT_EQ(slice.data[0] & 0x1Fu, 28u);
    }

    // 首片置 S 位，末片置 E 位
    EXPECT_EQ(slices.front().data[1] & 0x80u, 0x80u);
    EXPECT_EQ(slices.back().data[1] & 0x40u, 0x40u);
}

TEST(Fragmenter, ReassemblesBackToOriginalAcrossSizes)
{
    const std::size_t sizes[] = {1, 2, 3, 1198, 1199, 1200, 1201, 1202, 5000, 20000};

    for (const std::size_t size : sizes) {
        const auto nal = make_nal(0x41, size);
        std::vector<std::uint8_t> reassembled;
        std::size_t slice_count = 0;

        ASSERT_TRUE(roundtrip(nal, 1200, &reassembled, &slice_count)) << "size=" << size;
        EXPECT_EQ(reassembled, nal) << "size=" << size;
        EXPECT_GE(slice_count, 1u) << "size=" << size;
    }
}

TEST(Fragmenter, RejectsTooSmallMaxPayload)
{
    const auto nal = make_nal(0x41, 10);
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;

    EXPECT_FALSE(lowlat::fragment_nal(lowlat::ByteSpan(nal.data(), nal.size()), 2, &slices,
                                      &error));
    EXPECT_FALSE(error.empty());
}

TEST(Fragmenter, RejectsEmptyNal)
{
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;
    const std::uint8_t dummy = 0;

    EXPECT_FALSE(lowlat::fragment_nal(lowlat::ByteSpan(&dummy, 0), 1200, &slices, &error));
    EXPECT_FALSE(error.empty());
}

TEST(Depacketizer, DropsIncompleteFuAWhenNewStartArrives)
{
    const auto nal = make_nal(0x65, 3000);
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;
    ASSERT_TRUE(lowlat::fragment_nal(lowlat::ByteSpan(nal.data(), nal.size()), 1200, &slices,
                                     &error));

    lowlat::FuADepacketizer depacketizer;

    // 只喂首片（缺中间和末片），再喂另一帧的首片
    std::vector<std::uint8_t> produced;
    bool produced_flag = false;
    ASSERT_TRUE(depacketizer.feed(
        lowlat::ByteSpan(slices[0].data.data(), slices[0].data.size()), 1000, &produced,
        &produced_flag, &error));
    EXPECT_FALSE(produced_flag);
    EXPECT_TRUE(depacketizer.assembling());

    ASSERT_TRUE(depacketizer.feed(
        lowlat::ByteSpan(slices[0].data.data(), slices[0].data.size()), 2000, &produced,
        &produced_flag, &error));
    EXPECT_FALSE(produced_flag);
    EXPECT_EQ(depacketizer.dropped_nals(), 1u);
    EXPECT_EQ(depacketizer.produced_nals(), 0u);
}

TEST(Depacketizer, DropsFragmentWithMismatchedTimestamp)
{
    const auto nal = make_nal(0x65, 3000);
    std::string error;
    std::vector<lowlat::PayloadSlice> slices;
    ASSERT_TRUE(lowlat::fragment_nal(lowlat::ByteSpan(nal.data(), nal.size()), 1200, &slices,
                                     &error));
    ASSERT_GE(slices.size(), 3u);

    lowlat::FuADepacketizer depacketizer;
    std::vector<std::uint8_t> produced;
    bool produced_flag = false;

    ASSERT_TRUE(depacketizer.feed(
        lowlat::ByteSpan(slices[0].data.data(), slices[0].data.size()), 1000, &produced,
        &produced_flag, &error));
    // 中间片带不同时间戳，应被丢弃
    ASSERT_TRUE(depacketizer.feed(
        lowlat::ByteSpan(slices[1].data.data(), slices[1].data.size()), 9999, &produced,
        &produced_flag, &error));
    EXPECT_FALSE(produced_flag);
    EXPECT_EQ(depacketizer.dropped_nals(), 1u);
    EXPECT_FALSE(depacketizer.assembling());
}

TEST(Depacketizer, SingleNalPacketIsDeliveredDirectly)
{
    const auto nal = make_nal(0x41, 64);
    lowlat::FuADepacketizer depacketizer;

    std::vector<std::uint8_t> produced;
    bool produced_flag = false;
    std::string error;
    ASSERT_TRUE(depacketizer.feed(lowlat::ByteSpan(nal.data(), nal.size()), 1000, &produced,
                                  &produced_flag, &error));
    EXPECT_TRUE(produced_flag);
    EXPECT_EQ(produced, nal);
    EXPECT_EQ(depacketizer.produced_nals(), 1u);
}

TEST(Depacketizer, RejectsEmptyPayload)
{
    lowlat::FuADepacketizer depacketizer;
    std::vector<std::uint8_t> produced;
    bool produced_flag = false;
    std::string error;
    const std::uint8_t dummy = 0;

    EXPECT_FALSE(depacketizer.feed(lowlat::ByteSpan(&dummy, 0), 0, &produced, &produced_flag,
                                   &error));
    EXPECT_FALSE(error.empty());
}
