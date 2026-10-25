#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/byte_span.hpp"

namespace lowlat {

// XOR FEC：把一组 K 个数据包按字节异或，产出 1 个修复包。
//
// 修复包载荷布局（本项目自定义，不是 RFC 5109 的 ULP FEC）：
//   [0..1]  base_sequence   组内第一个数据包的 RTP 序列号（网络序）
//   [2]     group_size      K
//   [3]     reserved        恒为 0
//   [4..]   K × uint16      组内每个数据包的原始长度（网络序）
//   [...]   parity          各数据包按 max_len 补零后的异或结果
//
// 为什么必须带长度表：数据包长度不一致，只有知道每包的确切字节数，恢复时才能
// 精确截断，否则还原出的包尾部会残留补零字节，导致 RTP 头之后的负载错位。
//
// 恢复能力：组内任意丢 1 个包可恢复；丢 2 个及以上无能为力。
// 这正是 Reed-Solomon 要解决的问题，作为后期加分项。
class XorFecEncoder {
public:
    struct Config {
        std::size_t group_size{8};
        std::size_t max_parity_bytes{1200};  // 修复包载荷上限，防止超出 MTU
    };

    // 不能写成 Config config = Config{}：默认成员初始化器在类作用域内尚未完整，
    // 用重载代替默认实参。
    XorFecEncoder();
    explicit XorFecEncoder(Config config);

    void reset() noexcept;

    // 加入一个数据包（完整线上字节，含 RTP 头）。
    // 组满时通过 repair 输出修复包并置 produced=true。
    bool add(std::uint16_t sequence, ByteSpan packet, std::vector<std::uint8_t>* repair,
             bool* produced, std::string* error);

    // 帧边界调用：把当前未满的组也生成修复包。
    // 不做这一步的话，组会跨帧，恢复出的包只能追加到后续帧，导致 NAL 顺序错乱。
    // 组内不足 2 个包时不生成（k=1 的修复包等于原包，纯属浪费带宽）。
    bool flush(std::vector<std::uint8_t>* repair, bool* produced, std::string* error);

    std::size_t pending() const noexcept { return lengths_.size(); }
    std::size_t group_size() const noexcept { return config_.group_size; }
    std::uint64_t groups_emitted() const noexcept { return groups_emitted_; }

private:
    Config config_;
    std::vector<std::uint16_t> lengths_;
    std::vector<std::uint8_t> parity_;
    std::uint16_t base_sequence_{0};
    std::uint64_t groups_emitted_{0};
};

// 接收侧：按序列号保存最近收到的数据包，用修复包恢复其中缺失的那一个。
class XorFecDecoder {
public:
    struct Stats {
        std::uint64_t repairs_used{0};
        std::uint64_t repairs_unusable{0};  // 无缺失 / 缺太多 / 包体异常
        std::uint64_t recovered_packets{0};
    };

    explicit XorFecDecoder(std::size_t window = 64);

    void reset() noexcept;

    // 保存一个收到的数据包（完整线上字节）。
    void push_data(std::uint16_t sequence, ByteSpan packet);

    // 该序列号是否仍在窗口内（用于判断 NACK 目标是不是"晚到而非丢失"）。
    bool contains(std::uint16_t sequence) const noexcept;

    // 用修复包尝试恢复。ok=true 表示成功恢复出一个包，内容写入 recovered。
    // 返回 false 仅表示修复包本身畸形；"无法恢复"通过 ok=false 表达，不算错误。
    bool recover_with(ByteSpan repair, std::vector<std::uint8_t>* recovered, bool* ok,
                      std::string* error);

    const Stats& stats() const noexcept { return stats_; }
    std::size_t window() const noexcept { return slots_.size(); }

private:
    struct Slot {
        bool valid{false};
        std::uint16_t sequence{0};
        std::vector<std::uint8_t> data;
    };

    bool lookup(std::uint16_t sequence, const std::vector<std::uint8_t>** out) const noexcept;

    std::vector<Slot> slots_;
    Stats stats_;
};

}  // namespace lowlat
