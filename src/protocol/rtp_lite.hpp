#pragma once

#include <cstddef>
#include <cstdint>

#include "common/byte_span.hpp"

namespace lowlat {

// RTP 风格的 12 字节固定头，后随 8 字节自定义扩展
// （4 字节 frame_index + 4 字节采集时间戳）。
//
// 与 RFC 3550 的刻意差异（面试时要能说清楚为什么这样简化）：
//   1. 不使用 CSRC 列表，CC 恒为 0 —— 单 SSRC 会话用不到
//   2. X 位表示"存在 8 字节扩展"，而不是标准的扩展头结构
//      （标准扩展是 16 位 profile + 16 位 length + 数据）
//   3. 只有一种 payload type（96 = H.264），不做负载类型协商
//   4. P 位（padding）恒为 0，本协议不做尾部填充
//
// 为什么要把采集时间戳放进头里：端到端延迟只能在收端算，而收端只有拿到
// "这一帧是什么时候采集的"才能算。两端都跑在同一台 Linux 上时
// CLOCK_MONOTONIC 是系统级时钟，跨进程可比，所以直接存低 32 位微秒即可
// （约 71 分钟回绕一次，用 uint32 差值计算天然正确）。
// 注意：这套口径只在同主机成立，跨机部署需要 PTP/NTP 同步才能用。
struct RtpLiteHeader {
    static constexpr std::size_t kBaseSize = 12;
    static constexpr std::size_t kExtensionSize = 8;
    static constexpr std::uint8_t kPayloadTypeH264 = 96;
    static constexpr std::uint8_t kPayloadTypeFec = 97;  // 修复包，载荷为 XOR FEC 组
    static constexpr std::uint8_t kVersion = 2;

    std::uint8_t version{kVersion};
    std::uint8_t payload_type{kPayloadTypeH264};
    bool marker{false};
    bool has_frame_index{true};
    std::uint16_t sequence{0};
    std::uint32_t timestamp{0};  // 90 kHz 时钟
    std::uint32_t ssrc{0};
    std::uint32_t frame_index{0};
    std::uint32_t capture_ts_us{0};  // steady_clock 微秒的低 32 位

    std::size_t serialized_size() const noexcept
    {
        return kBaseSize + (has_frame_index ? kExtensionSize : 0);
    }
};

// 写入 out（至少 serialized_size() 字节），返回写入的字节数。
std::size_t serialize_rtp_lite(const RtpLiteHeader& header, std::uint8_t* out) noexcept;

// 解析。长度不足或版本不符时返回 false，且不修改 out。
bool parse_rtp_lite(ByteSpan input, RtpLiteHeader* out) noexcept;

}  // namespace lowlat
