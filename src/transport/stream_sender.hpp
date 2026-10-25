#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/byte_span.hpp"
#include "protocol/bandwidth_estimator.hpp"
#include "protocol/control_message.hpp"
#include "protocol/fec_xor.hpp"
#include "protocol/rtp_lite.hpp"
#include "transport/udp_socket.hpp"

namespace lowlat {

// 最近若干包的环形缓存，供 NACK 重传使用。
// 以 sequence % capacity 定位槽位，并用 sequence 本身校验，天然处理回绕。
class RetransmitCache {
public:
    explicit RetransmitCache(std::size_t capacity = 1024);

    void store(std::uint16_t sequence, ByteSpan packet);

    // 未命中返回 nullptr（包太旧已被覆盖）
    const std::vector<std::uint8_t>* find(std::uint16_t sequence) const noexcept;

    std::size_t capacity() const noexcept { return slots_.size(); }

private:
    std::vector<std::vector<std::uint8_t>> slots_;
    std::vector<std::uint16_t> sequences_;
    std::vector<bool> valid_;
};

// 把一帧的 NAL 打包成 RTP-lite 包并批量发出。
//
// 职责边界：只负责"NAL -> 线上字节"，不关心 NAL 从哪来（编码器给的还是文件读的），
// 因此可以直接单测。采集、编码、统计留在调用方。
class StreamSender {
public:
    struct Config {
        UdpEndpoint destination;
        std::uint32_t ssrc{0x4C4C5601u};  // "LLV" + 版本
        std::size_t max_payload{1200};    // 含 16 字节头，留足 MTU 余量
        std::size_t send_buffer_bytes{4u * 1024u * 1024u};
        bool enable_fec{false};           // 打开后每组额外发 1 个修复包
        std::size_t fec_group_size{8};
        std::size_t retransmit_capacity{1024};  // NACK 缓存包数

        // 拥塞控制：接收端回传时序样本，发送端据此调整目标码率
        bool enable_congestion_control{false};
        int start_bitrate_kbps{2000};
        int min_bitrate_kbps{300};
        int max_bitrate_kbps{4000};
    };

    bool open(const Config& config, std::string* error);
    void close() noexcept;

    bool is_open() const noexcept { return socket_.is_open(); }
    std::uint16_t local_port() const noexcept { return socket_.local_port(); }

    // nals 必须按发送顺序给出（SPS/PPS/IDR/slice）。
    // timestamp 单位为 90kHz；同一帧内所有包共用同一个 timestamp。
    // 帧内最后一个包置 marker 位。
    // capture_ts_us: 采集时刻（steady_clock 微秒低 32 位），原样写进包头供收端算延迟
    bool send_frame(const std::vector<ByteSpan>& nals, std::uint32_t frame_index,
                    std::uint32_t timestamp, std::uint32_t capture_ts_us, std::string* error);

    std::uint64_t packets_sent() const noexcept { return packets_sent_; }
    std::uint64_t bytes_sent() const noexcept { return bytes_sent_; }
    std::uint64_t would_block_events() const noexcept { return would_block_events_; }
    std::uint64_t packets_dropped() const noexcept { return packets_dropped_; }
    std::uint64_t fec_packets_sent() const noexcept { return fec_packets_sent_; }
    std::uint64_t fec_group_failures() const noexcept { return fec_group_failures_; }

    // 处理反向通道上的 NACK：从缓存取包原样重发（保留原序列号）。
    //
    // timeout_ms > 0 时会阻塞等待最多这么久——发送端应当把"等下一帧"的空档
    // 交给它，而不是 sleep 到帧到期才轮询一次。否则重传最多被拖一个帧间隔，
    // 这个人工延迟会污染接收端的 RTT 估计。
    // 返回本轮处理的 NACK 报文个数。
    std::size_t poll_incoming(std::size_t max_datagrams, int timeout_ms, std::string* error);

    std::uint64_t nack_received() const noexcept { return nack_received_; }
    std::uint64_t retransmit_packets_sent() const noexcept { return retransmit_packets_sent_; }
    std::uint64_t nack_unserviceable() const noexcept { return nack_unserviceable_; }

    // 模拟丢包：在发送前按比例丢弃，用于演示/压测。
    //
    // 为什么不用 tc netem：那需要 root 与独立 netns，界面里没法按个按钮就生效。
    // 在发送端丢弃的好处是完全进程内可控，且链路之后的行为与真实丢包一致——
    // 收端看到的是序列号缺口，照样触发 NACK 与 FEC 恢复。
    // 区别只在于丢包发生在"上行末端"而不是链路中间，对恢复机制而言没有差别。
    void set_simulated_loss_permille(int permille) noexcept;
    int simulated_loss_permille() const noexcept { return simulated_loss_permille_; }
    std::uint64_t simulated_drops() const noexcept { return simulated_drops_; }

    // 拥塞控制产出的目标码率（kbps）。关闭拥塞控制时返回配置的起始码率。
    int target_bitrate_kbps() const noexcept;
    std::uint64_t feedback_received() const noexcept { return feedback_received_; }
    BandwidthEstimator::State bandwidth_state() const noexcept { return estimator_.state(); }
    std::int64_t last_window_delay_us() const noexcept { return estimator_.last_window_delay_us(); }
    std::uint16_t next_sequence() const noexcept { return sequence_; }

private:
    UdpSocket socket_;
    Config config_{};
    XorFecEncoder fec_encoder_{};
    std::uint64_t fec_packets_sent_{0};
    std::uint64_t fec_group_failures_{0};

    RetransmitCache cache_{};
    std::uint64_t nack_received_{0};
    std::uint64_t retransmit_packets_sent_{0};
    std::uint64_t nack_unserviceable_{0};

    BandwidthEstimator estimator_{};
    std::uint64_t feedback_received_{0};

    int simulated_loss_permille_{0};
    std::uint64_t simulated_drops_{0};
    std::uint32_t rng_state_{0x2545F491u};

    // 返回 [0, bound) 的伪随机数。用 xorshift 而不是 rand()：
    // 可复现、无全局状态，也不会和别的库抢种子。
    std::uint32_t next_random(std::uint32_t bound) noexcept;
    std::int64_t unwrap_send_us_{-1};
    std::int64_t unwrap_arrival_us_{-1};

    std::uint16_t sequence_{0};
    std::uint64_t packets_sent_{0};
    std::uint64_t bytes_sent_{0};
    std::uint64_t would_block_events_{0};
    std::uint64_t packets_dropped_{0};

    // 复用缓冲，避免每帧重新分配
    std::vector<std::vector<std::uint8_t>> buffers_;
    std::vector<ByteSpan> spans_;
};

}  // namespace lowlat
