#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/byte_span.hpp"
#include "protocol/nack.hpp"

namespace lowlat {

// 反向控制通道的报文统一格式。首字节是类型，后面是各自的载荷。
//
//   NACK:      [0]=1  [1..2]=count  [3..]  count × uint16 序列号
//   FEEDBACK:  [0]=2  [1]=count  [2..3]=loss_permille  [4..] count × (capture_us, arrival_us)
//
// 为什么要加类型字节：早期 NACK 是裸格式（首字节就是 count），
// 加入带反馈后无法区分；count 的取值范围恰好会和类型值重叠，靠启发式判断不可靠。
//
// 时间戳用 uint32 微秒（约 71 分钟回绕）。跨主机时两端绝对值不同，
// 但延迟梯度只关心**差值**，常数偏移会被抵消，回绕用无符号差值也天然正确。
enum class ControlType : std::uint8_t {
    Nack = 1,
    Feedback = 2,
};

struct DelaySample {
    std::uint32_t capture_us{0};
    std::uint32_t arrival_us{0};
};

// 接收端每隔一段时间回传一批时序样本与丢包率，发送端据此估计带宽。
struct FeedbackReport {
    static constexpr std::size_t kMaxSamples = 8;

    std::vector<DelaySample> samples;
    std::uint16_t loss_permille{0};  // 0..1000，避免浮点

    std::size_t serialized_size() const noexcept { return 4 + 8 * samples.size(); }
};

// 读首字节判断类型；空包或未知类型返回 std::nullopt 语义（用 bool 返回）
bool peek_control_type(ByteSpan input, ControlType* type) noexcept;

std::size_t serialize_nack_control(const NackRequest& request, std::uint8_t* out) noexcept;
bool parse_nack_control(ByteSpan input, NackRequest* out) noexcept;

std::size_t serialize_feedback(const FeedbackReport& report, std::uint8_t* out) noexcept;
bool parse_feedback(ByteSpan input, FeedbackReport* out) noexcept;

}  // namespace lowlat
