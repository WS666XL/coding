#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/byte_span.hpp"

namespace lowlat {

// H.264 NAL 单元类型（只列出本协议关心的几个）
enum class NalType : std::uint8_t {
    kSlice = 1,
    kIdr = 5,
    kSei = 6,
    kSps = 7,
    kPps = 8,
    kFuA = 28,
};

const char* nal_type_name(NalType type) noexcept;
NalType nal_type_of(ByteSpan nal) noexcept;
bool nal_is_keyframe(ByteSpan nal) noexcept;

// 扫描 Annex-B 起始码，返回每个 NAL 的裸数据（不含起始码）。
// 同时支持 3 字节（00 00 01）与 4 字节（00 00 00 01）起始码。
// 返回的 ByteSpan 指向输入缓冲，调用方需保证其生命周期。
std::vector<ByteSpan> split_annexb(const std::uint8_t* data, std::size_t size);

// 一个 RTP 载荷（不含 RTP 头）。FU-A 分片时 data 前两字节是 FU indicator/header。
struct PayloadSlice {
    std::vector<std::uint8_t> data;
    bool fu_a_start{false};
    bool fu_a_end{false};
};

// 发送侧：把一个完整 NAL 切成若干 RTP 载荷。
//   - 小于等于 max_payload 时按 Single NAL Unit Packet 发送（原样）
//   - 否则按 RFC 6184 的 FU-A 分片，首片置 S、末片置 E
//   - max_payload 必须 >= 3（FU indicator + FU header + 至少 1 字节负载）
bool fragment_nal(ByteSpan nal, std::size_t max_payload,
                  std::vector<PayloadSlice>* out, std::string* error);

// 接收侧：FU-A 重组。
//
// 约束：只维护"当前正在重组的一个 NAL"。如果上一个 NAL 尚未收齐就来了新的
// 起始片，直接丢弃旧的并计数（dropped_）——真正的按帧排序与超时策略属于
// 抖动缓冲（P5），此处刻意不做缓冲堆积。
class FuADepacketizer {
public:
    // 输入一个 RTP 载荷。produced 为 true 时 out_nal 是一条完整 NAL（含 NAL 头）。
    // 单 NAL 包会直接产出；FU-A 分片则会累积到末片才产出。
    bool feed(ByteSpan payload, std::uint32_t timestamp,
              std::vector<std::uint8_t>* out_nal, bool* produced, std::string* error);

    void reset() noexcept;

    std::uint64_t dropped_nals() const noexcept { return dropped_nals_; }
    std::uint64_t produced_nals() const noexcept { return produced_nals_; }
    bool assembling() const noexcept { return assembling_; }

private:
    std::vector<std::uint8_t> buffer_;
    std::uint32_t current_timestamp_{0};
    bool assembling_{false};
    std::uint64_t dropped_nals_{0};
    std::uint64_t produced_nals_{0};
};

}  // namespace lowlat
