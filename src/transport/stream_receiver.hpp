#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lowlat {

// 一个已经重排、去重、去掉 FU-A 头之后的完整帧
struct ReceivedFrame {
    std::uint32_t frame_index{0};
    std::uint32_t capture_ts_us{0};
    // NAL 按发送顺序排列，不含 Annex-B 起始码
    std::vector<std::vector<std::uint8_t>> nals;
};

// 接收侧全部统计量（累计值，非增量）
struct ReceiverStats {
    std::uint64_t packets{0};
    std::uint64_t bytes{0};
    std::uint64_t malformed{0};
    std::uint64_t distinct_frames{0};
    std::uint64_t frame_gaps{0};
    std::uint64_t sequence_breaks{0};
    std::uint64_t nals_emitted{0};

    std::uint64_t fec_recovered_packets{0};
    std::uint64_t nals_from_fec{0};
    std::uint64_t retransmits_received{0};
    std::uint64_t nals_dropped_late{0};
    std::uint64_t duplicate_nals_dropped{0};

    std::uint64_t nack_sent{0};
    std::uint64_t nack_sequences_requested{0};
    std::size_t pending_nack{0};

    std::uint64_t feedback_sent{0};

    double jitter_us{0.0};
    double srtt_us{0.0};
    double min_rtt_us{0.0};
    std::uint64_t rtt_samples{0};

    int depth_ms{0};
    int depth_min_ms{0};
    int depth_max_ms{0};

    std::size_t latency_samples{0};
    double latency_avg_ms{0.0};
    double latency_p50_ms{0.0};
    double latency_p99_ms{0.0};
    double latency_max_ms{0.0};
};

// 接收流水线：收包 → 解析 → FU-A 重组 → FEC 恢复 → NACK 请求 → 抖动缓冲 → 按帧交付。
//
// 从 receiver_main 抽出来的目的是让"落盘"和"渲染"共用同一条链路：
//   - headless 客户端把 poll() 得到的帧写文件
//   - Qt 客户端把同样的帧送进解码器再上屏
// 两边行为完全一致，不会出现"命令行能播、界面不能播"的分叉。
class StreamReceiver {
public:
    struct Options {
        std::string bind_address{"0.0.0.0"};
        std::uint16_t bind_port{5000};
        int rcvbuf_mb{8};

        bool enable_nack{false};
        int nack_delay_ms{8};

        // 拥塞控制反馈：把每帧的 (采集时刻, 到达时刻) 与丢包率回传给发送端
        bool enable_feedback{false};
        int feedback_interval_ms{300};

        bool adaptive_buffer{false};
        int frame_hold_ms{60};
        double jitter_k{3.0};
        int depth_min_ms{20};
        int depth_max_ms{120};
        int rtt_margin_ms{10};
    };

    StreamReceiver();
    ~StreamReceiver();

    StreamReceiver(const StreamReceiver&) = delete;
    StreamReceiver& operator=(const StreamReceiver&) = delete;

    bool open(const Options& options, std::string* error);
    void close() noexcept;
    bool is_open() const noexcept;
    std::uint16_t local_port() const noexcept;

    // 收包并产出已就绪的帧。
    // timeout_ms <= 0 表示不等待；返回 false 仅代表不可恢复的错误。
    bool poll(int timeout_ms, std::vector<ReceivedFrame>* frames, std::string* error);

    // 把还压在缓冲里的帧全部取出（退出前调用）。
    void flush(std::vector<ReceivedFrame>* frames);

    // 完整统计快照。内部要计算延迟分位数（排序窗口），不要在每轮 poll 里调用。
    ReceiverStats snapshot() const;

    // 轻量计数：只读原子量，供主循环判断退出条件
    std::uint64_t received_frames() const noexcept;
    std::uint64_t received_packets() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lowlat
