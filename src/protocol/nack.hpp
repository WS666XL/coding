#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/byte_span.hpp"

namespace lowlat {

// NACK 请求（反向通道控制报文）。
//
// 载荷是裸格式，不带 RTP-lite 头：
//   [0..1] count   请求的序列号个数（网络序）
//   [2..]  count × uint16 序列号（网络序）
//
// 为什么不套 RTP 头：NACK 是反方向的轻量控制包，套 16 字节头纯属浪费；
// 发送端收到 UDP 载荷按本格式解析即可。这也是自研协议相对标准 RTP/RTCP
// 的一处简化——代价是不能与标准 RTCP 端点互通。
struct NackRequest {
    static constexpr std::size_t kMaxSequences = 32;
    static constexpr std::size_t kHeaderBytes = 2;

    std::vector<std::uint16_t> sequences;

    std::size_t serialized_size() const noexcept
    {
        return kHeaderBytes + 2 * sequences.size();
    }

    bool empty() const noexcept { return sequences.empty(); }
};

// 写入 out（至少 serialized_size() 字节），返回写入字节数。
// sequences 超过 kMaxSequences 时只序列化前 kMaxSequences 个。
std::size_t serialize_nack(const NackRequest& request, std::uint8_t* out) noexcept;

// 解析。长度不足或 count 超限时返回 false。空请求（count==0）解析成功但 empty() 为真。
bool parse_nack(ByteSpan input, NackRequest* out) noexcept;

}  // namespace lowlat
