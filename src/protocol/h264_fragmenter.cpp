#include "protocol/h264_fragmenter.hpp"

#include <algorithm>

namespace lowlat {
namespace {

constexpr std::uint8_t kFuAType = 28;
constexpr std::uint8_t kFuAStartBit = 0x80;
constexpr std::uint8_t kFuAEndBit = 0x40;

}  // namespace

const char* nal_type_name(NalType type) noexcept
{
    switch (type) {
    case NalType::kSlice:
        return "slice";
    case NalType::kIdr:
        return "idr";
    case NalType::kSei:
        return "sei";
    case NalType::kSps:
        return "sps";
    case NalType::kPps:
        return "pps";
    case NalType::kFuA:
        return "fu-a";
    }
    return "other";
}

NalType nal_type_of(ByteSpan nal) noexcept
{
    if (nal.empty()) {
        return NalType::kSlice;
    }
    return static_cast<NalType>(nal[0] & 0x1Fu);
}

bool nal_is_keyframe(ByteSpan nal) noexcept
{
    if (nal.empty()) {
        return false;
    }
    const std::uint8_t type = nal[0] & 0x1Fu;
    return type == static_cast<std::uint8_t>(NalType::kIdr) ||
           type == static_cast<std::uint8_t>(NalType::kSps) ||
           type == static_cast<std::uint8_t>(NalType::kPps);
}

std::vector<ByteSpan> split_annexb(const std::uint8_t* data, std::size_t size)
{
    std::vector<ByteSpan> nals;
    if (data == nullptr || size < 4) {
        return nals;
    }

    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    std::size_t nal_begin = kNone;
    std::size_t i = 0;

    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (nal_begin != kNone) {
                // 4 字节起始码会让上一个 NAL 尾部多出一个 0，这里一并剥掉
                std::size_t end = i;
                while (end > nal_begin && data[end - 1] == 0) {
                    --end;
                }
                if (end > nal_begin) {
                    nals.push_back(ByteSpan(data + nal_begin, end - nal_begin));
                }
            }
            i += 3;
            nal_begin = i;
            continue;
        }
        ++i;
    }

    if (nal_begin != kNone && nal_begin < size) {
        nals.push_back(ByteSpan(data + nal_begin, size - nal_begin));
    }

    return nals;
}

bool fragment_nal(ByteSpan nal, std::size_t max_payload,
                  std::vector<PayloadSlice>* out, std::string* error)
{
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "fragment_nal 输出指针为空";
        }
        return false;
    }
    if (nal.empty()) {
        if (error != nullptr) {
            *error = "fragment_nal 收到空 NAL";
        }
        return false;
    }
    if (max_payload < 3) {
        if (error != nullptr) {
            *error = "max_payload 必须 >= 3（FU indicator + FU header + 1 字节负载）";
        }
        return false;
    }

    if (nal.size() <= max_payload) {
        PayloadSlice single;
        single.data.assign(nal.begin(), nal.end());
        out->push_back(std::move(single));
        return true;
    }

    const std::uint8_t nal_header = nal[0];
    // FU indicator：F 与 NRI 原样保留，类型改成 28
    const std::uint8_t fu_indicator =
        static_cast<std::uint8_t>((nal_header & 0xE0u) | kFuAType);
    const std::uint8_t original_type = static_cast<std::uint8_t>(nal_header & 0x1Fu);

    const std::size_t chunk = max_payload - 2;
    std::size_t offset = 1;  // NAL 头已经并入 FU header，不重复发送

    while (offset < nal.size()) {
        const std::size_t take = std::min(chunk, nal.size() - offset);
        const bool is_first = (offset == 1);
        const bool is_last = (offset + take >= nal.size());

        PayloadSlice slice;
        slice.data.reserve(2 + take);
        slice.data.push_back(fu_indicator);

        std::uint8_t fu_header = original_type;
        if (is_first) {
            fu_header = static_cast<std::uint8_t>(fu_header | kFuAStartBit);
        }
        if (is_last) {
            fu_header = static_cast<std::uint8_t>(fu_header | kFuAEndBit);
        }
        slice.data.push_back(fu_header);
        slice.data.insert(slice.data.end(), nal.begin() + static_cast<std::ptrdiff_t>(offset),
                          nal.begin() + static_cast<std::ptrdiff_t>(offset + take));

        slice.fu_a_start = is_first;
        slice.fu_a_end = is_last;
        out->push_back(std::move(slice));

        offset += take;
    }

    return true;
}

bool FuADepacketizer::feed(ByteSpan payload, std::uint32_t timestamp,
                           std::vector<std::uint8_t>* out_nal, bool* produced,
                           std::string* error)
{
    if (out_nal == nullptr || produced == nullptr) {
        if (error != nullptr) {
            *error = "depacketizer 输出指针为空";
        }
        return false;
    }
    *produced = false;

    if (payload.empty()) {
        if (error != nullptr) {
            *error = "depacketizer 收到空载荷";
        }
        return false;
    }

    const std::uint8_t type = payload[0] & 0x1Fu;

    if (type == kFuAType) {
        if (payload.size() < 3) {
            if (error != nullptr) {
                *error = "FU-A 载荷少于 3 字节";
            }
            return false;
        }

        const std::uint8_t fu_header = payload[1];
        const bool start = (fu_header & kFuAStartBit) != 0;
        const bool end = (fu_header & kFuAEndBit) != 0;
        const std::uint8_t original_type = static_cast<std::uint8_t>(fu_header & 0x1Fu);

        if (start) {
            if (assembling_) {
                // 上一个 NAL 没收齐就来了新的起始片，丢弃旧数据
                ++dropped_nals_;
            }
            buffer_.clear();
            buffer_.push_back(static_cast<std::uint8_t>((payload[0] & 0xE0u) | original_type));
            current_timestamp_ = timestamp;
            assembling_ = true;
        } else {
            if (!assembling_ || timestamp != current_timestamp_) {
                // 中途丢片或跨帧乱序：丢弃本次重组
                ++dropped_nals_;
                buffer_.clear();
                assembling_ = false;
                return true;
            }
        }

        buffer_.insert(buffer_.end(),
                       payload.begin() + 2, payload.end());

        if (end) {
            *out_nal = std::move(buffer_);
            buffer_.clear();
            assembling_ = false;
            *produced = true;
            ++produced_nals_;
        }
        return true;
    }

    // 单 NAL Unit Packet：直接交付
    if (assembling_) {
        ++dropped_nals_;
        buffer_.clear();
        assembling_ = false;
    }
    out_nal->assign(payload.begin(), payload.end());
    *produced = true;
    ++produced_nals_;
    return true;
}

void FuADepacketizer::reset() noexcept
{
    buffer_.clear();
    buffer_.shrink_to_fit();
    assembling_ = false;
    current_timestamp_ = 0;
    dropped_nals_ = 0;
    produced_nals_ = 0;
}

}  // namespace lowlat
