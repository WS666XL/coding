#include "transport/stream_receiver.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <poll.h>
#include <unordered_map>
#include <utility>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "protocol/fec_xor.hpp"
#include "protocol/h264_fragmenter.hpp"
#include "protocol/control_message.hpp"
#include "protocol/jitter_buffer.hpp"
#include "protocol/nack.hpp"
#include "protocol/nack_scheduler.hpp"
#include "protocol/rtp_lite.hpp"
#include "protocol/rtt_estimator.hpp"
#include "transport/udp_socket.hpp"

namespace lowlat {
namespace {

constexpr int kMaxPacketsPerPoll = 512;
constexpr std::size_t kMaxBufferedFrames = 8;
constexpr std::size_t kMaxTrackedNacks = 512;

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct StreamReceiver::Impl {
    struct FrameBuf {
        std::uint32_t frame_index{0};
        std::uint32_t capture_ts_us{0};
        std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> nals;
        std::vector<std::uint16_t> seen_sequences;  // FEC 与重传可能送来同一个包
        std::chrono::steady_clock::time_point deadline;
    };

    Options options;
    UdpSocket socket;
    FuADepacketizer depacketizer;
    XorFecDecoder fec_decoder{128};
    NackScheduler nack_scheduler{8, NackRequest::kMaxSequences};
    JitterEstimator jitter;
    BufferDepthPolicy depth_policy{BufferDepthPolicy::Config{}};
    RttEstimator rtt;
    Stats latency_stats;

    UdpEndpoint sender_endpoint;
    bool have_sender_endpoint{false};
    std::unordered_map<std::uint16_t, std::int64_t> nack_sent_at_us;

    std::deque<FrameBuf> frame_bufs;

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

    // 拥塞控制反馈
    std::vector<DelaySample> delay_samples;
    std::uint64_t feedback_sent{0};
    std::uint64_t loss_window_packets{0};
    std::uint64_t loss_window_breaks{0};
    std::int64_t last_feedback_us{0};

    std::uint32_t last_frame_index{0};
    bool have_frame{false};
    std::uint16_t last_sequence{0};
    bool have_sequence{false};

    int current_depth_ms{60};
    int depth_min_seen{0};
    int depth_max_seen{0};
    bool depth_recorded{false};

    FrameBuf make_frame_buf(std::uint32_t index, std::uint32_t capture_ts_us);
    void record_depth();

    // 当前 poll/flush 调用的输出目标。用成员而不是层层传参，
    // 是因为帧边界、缓冲上限、到期冲刷三个位置都会产帧。
    std::vector<ReceivedFrame>* output{nullptr};

    void emit_frame(FrameBuf& frame);
    void flush_ready_frames(bool force);

    void handle_data_wire(ByteSpan wire, bool recovered);
    void process_datagram(ByteSpan wire, const UdpEndpoint& from);
    void maybe_send_nack();
    void maybe_send_feedback(bool force);
};

StreamReceiver::Impl::FrameBuf StreamReceiver::Impl::make_frame_buf(std::uint32_t index,
                                                                  std::uint32_t capture_ts_us)
{
    FrameBuf created;
    created.frame_index = index;
    created.capture_ts_us = capture_ts_us;
    created.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(current_depth_ms);
    record_depth();
    return created;
}

void StreamReceiver::Impl::record_depth()
{
    if (!depth_recorded) {
        depth_min_seen = current_depth_ms;
        depth_max_seen = current_depth_ms;
        depth_recorded = true;
        return;
    }
    depth_min_seen = std::min(depth_min_seen, current_depth_ms);
    depth_max_seen = std::max(depth_max_seen, current_depth_ms);
}

void StreamReceiver::Impl::emit_frame(FrameBuf& frame)
{
    if (output == nullptr) {
        frame.nals.clear();
        return;
    }

    // 按序列号（模 2^16 比较）排序，恢复帧内正确顺序
    std::sort(frame.nals.begin(), frame.nals.end(),
              [](const std::pair<std::uint16_t, std::vector<std::uint8_t>>& a,
                 const std::pair<std::uint16_t, std::vector<std::uint8_t>>& b) {
                  return static_cast<std::int16_t>(a.first - b.first) < 0;
              });

    ReceivedFrame received;
    received.frame_index = frame.frame_index;
    received.capture_ts_us = frame.capture_ts_us;
    received.nals.reserve(frame.nals.size());
    for (auto& item : frame.nals) {
        received.nals.push_back(std::move(item.second));
        ++nals_emitted;
    }
    frame.nals.clear();

    // 端到端延迟：两端同主机，CLOCK_MONOTONIC 通用；uint32 差值天然处理回绕
    if (frame.capture_ts_us != 0) {
        const std::uint32_t now_low32 = static_cast<std::uint32_t>(steady_now_us());
        const std::uint32_t delta_us = now_low32 - frame.capture_ts_us;
        latency_stats.record_latency_us(static_cast<std::int64_t>(delta_us));
    }

    output->push_back(std::move(received));
}

void StreamReceiver::Impl::flush_ready_frames(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    while (!frame_bufs.empty()) {
        if (!force && now < frame_bufs.front().deadline) {
            break;
        }
        emit_frame(frame_bufs.front());
        frame_bufs.pop_front();
    }
}

void StreamReceiver::Impl::handle_data_wire(ByteSpan wire, bool recovered)
{
    RtpLiteHeader header;
    if (!parse_rtp_lite(wire, &header)) {
        ++malformed;
        return;
    }

    const std::int16_t sequence_delta =
        have_sequence ? static_cast<std::int16_t>(header.sequence - last_sequence) : 1;
    // 序列号向后跳 = 重传或乱序包。它属于已经统计过的旧帧，
    // 绝不能参与帧号统计，否则 distinct_frames 会被提前凑满。
    const bool is_retransmit = have_sequence && (sequence_delta <= 0);

    if (!recovered && !is_retransmit) {
        // 抖动估计只吃按序到达的正常包；重传与恢复包的到达时刻是人为的
        jitter.update(steady_now_us(), header.timestamp);
        if (options.adaptive_buffer) {
            current_depth_ms = depth_policy.compute_depth_ms(
                jitter.jitter_us(), rtt.has_estimate() ? rtt.smoothed_us() : 0.0);
        }

        if (have_frame) {
            if (header.frame_index != last_frame_index) {
                if (header.frame_index > last_frame_index + 1) {
                    frame_gaps += header.frame_index - last_frame_index - 1;
                }
                ++distinct_frames;
                flush_ready_frames(false);
                frame_bufs.push_back(make_frame_buf(header.frame_index, header.capture_ts_us));
                // 防止长时间卡住导致内存膨胀：超限时强制交付最旧的帧，
                // 而不是静默丢弃——丢帧要能被上层统计到。
                while (frame_bufs.size() > kMaxBufferedFrames) {
                    emit_frame(frame_bufs.front());
                    frame_bufs.pop_front();
                }
            }
        } else {
            ++distinct_frames;
            have_frame = true;
            frame_bufs.push_back(make_frame_buf(header.frame_index, header.capture_ts_us));
        }
        last_frame_index = header.frame_index;

        if (!have_sequence) {
            have_sequence = true;
            last_sequence = header.sequence;
        } else if (sequence_delta == 1) {
            last_sequence = header.sequence;
        } else {
            // 向前跳：中间那些序列号就是丢了的数据包
            sequence_breaks += static_cast<std::uint64_t>(sequence_delta - 1);
            if (options.enable_nack) {
                const std::int64_t now_us = steady_now_us();
                for (std::int16_t offset = 1; offset < sequence_delta; ++offset) {
                    nack_scheduler.note_missing(
                        static_cast<std::uint16_t>(last_sequence + offset), now_us);
                }
            }
            last_sequence = header.sequence;
        }
    } else if (is_retransmit) {
        ++retransmits_received;
        // 只有"我们真的请求过"的包才算 RTT 样本
        const auto sent = nack_sent_at_us.find(header.sequence);
        if (sent != nack_sent_at_us.end()) {
            const double sample_us = static_cast<double>(steady_now_us() - sent->second);
            if (sample_us > 0.0 && sample_us < 1000000.0) {
                rtt.update(sample_us);
            }
            nack_sent_at_us.erase(sent);
        }
    }

    // 拥塞控制采样：只在每帧最后一个包（marker）记录一次。
    // 若每个包都记，同一帧内 capture_ts 不变而到达时刻在走，
    // 会被误判成"链路持续堆积"。
    if (header.marker && options.enable_feedback && !recovered && !is_retransmit) {
        DelaySample sample;
        sample.capture_us = header.capture_ts_us;
        sample.arrival_us = static_cast<std::uint32_t>(steady_now_us());
        delay_samples.push_back(sample);
        if (delay_samples.size() >= FeedbackReport::kMaxSamples) {
            maybe_send_feedback(true);
        }
    }

    const std::size_t header_size = header.serialized_size();
    if (wire.size() <= header_size) {
        ++malformed;
        return;
    }

    std::vector<std::uint8_t> nal;
    bool produced = false;
    if (!depacketizer.feed(wire.subspan(header_size, wire.size() - header_size),
                           header.timestamp, &nal, &produced, nullptr)) {
        ++malformed;
        return;
    }
    if (!produced) {
        return;
    }

    FrameBuf* target = nullptr;
    for (auto it = frame_bufs.rbegin(); it != frame_bufs.rend(); ++it) {
        if (it->frame_index == header.frame_index) {
            target = &(*it);
            break;
        }
    }
    if (target == nullptr) {
        // 所属帧已经交付，晚到的 NAL 无处安放
        ++nals_dropped_late;
        return;
    }
    if (std::find(target->seen_sequences.begin(), target->seen_sequences.end(),
                  header.sequence) != target->seen_sequences.end()) {
        ++duplicate_nals_dropped;
        return;
    }
    target->seen_sequences.push_back(header.sequence);
    target->nals.emplace_back(header.sequence, std::move(nal));
    if (recovered) {
        ++nals_from_fec;
    }
}

void StreamReceiver::Impl::process_datagram(ByteSpan wire, const UdpEndpoint& from)
{
    ++packets;
    bytes += wire.size();

    if (!have_sender_endpoint && from.valid()) {
        // 反向通道（NACK）需要发送端地址，从首个数据报的源地址学习
        sender_endpoint = from;
        have_sender_endpoint = true;
    }

    RtpLiteHeader header;
    if (!parse_rtp_lite(wire, &header)) {
        ++malformed;
        return;
    }

    if (header.payload_type == RtpLiteHeader::kPayloadTypeFec) {
        const std::size_t fec_header = header.serialized_size();
        if (wire.size() <= fec_header) {
            ++malformed;
            return;
        }
        std::vector<std::uint8_t> recovered_wire;
        bool recovered_ok = false;
        if (!fec_decoder.recover_with(wire.subspan(fec_header, wire.size() - fec_header),
                                      &recovered_wire, &recovered_ok, nullptr)) {
            ++malformed;
            return;
        }
        if (recovered_ok) {
            ++fec_recovered_packets;
            handle_data_wire(ByteSpan(recovered_wire.data(), recovered_wire.size()), true);
        }
        return;
    }

    fec_decoder.push_data(header.sequence, wire);
    handle_data_wire(wire, false);
}

void StreamReceiver::Impl::maybe_send_nack()
{
    if (!options.enable_nack || !have_sender_endpoint) {
        return;
    }

    const std::int64_t now_us = steady_now_us();
    std::vector<std::uint16_t> batch = nack_scheduler.collect(
        now_us, [this](std::uint16_t sequence) { return !fec_decoder.contains(sequence); });
    if (batch.empty()) {
        return;
    }

    NackRequest request;
    request.sequences = batch;

    // 控制通道统一带 1 字节类型前缀
    std::vector<std::uint8_t> payload(request.serialized_size() + 1);
    serialize_nack_control(request, payload.data());

    std::string ignored;
    if (socket.send_to(sender_endpoint, ByteSpan(payload.data(), payload.size()), &ignored)) {
        ++nack_sent;
        nack_sequences_requested += request.sequences.size();
        for (const std::uint16_t sequence : request.sequences) {
            if (nack_sent_at_us.size() < kMaxTrackedNacks) {
                nack_sent_at_us[sequence] = now_us;
            }
        }
    }
}

void StreamReceiver::Impl::maybe_send_feedback(bool force)
{
    if (!options.enable_feedback || !have_sender_endpoint || delay_samples.empty()) {
        return;
    }

    const std::int64_t now_us = steady_now_us();
    if (!force && delay_samples.size() < FeedbackReport::kMaxSamples &&
        now_us - last_feedback_us < static_cast<std::int64_t>(options.feedback_interval_ms) * 1000) {
        return;
    }

    FeedbackReport report;
    report.samples = delay_samples;

    // 窗口丢包率：新增的序列断裂数 / 新增包数。
    // 用序列断裂而不是 FEC 统计，是因为断裂直接反映"该到没到"。
    const std::uint64_t new_packets = packets - loss_window_packets;
    const std::uint64_t new_breaks = sequence_breaks - loss_window_breaks;
    double ratio = new_packets > 0 ? static_cast<double>(new_breaks) /
                                         static_cast<double>(new_packets)
                                   : 0.0;
    if (ratio > 1.0) {
        ratio = 1.0;
    }
    report.loss_permille = static_cast<std::uint16_t>(ratio * 1000.0);

    loss_window_packets = packets;
    loss_window_breaks = sequence_breaks;
    last_feedback_us = now_us;

    std::vector<std::uint8_t> payload(report.serialized_size());
    serialize_feedback(report, payload.data());

    std::string ignored;
    if (socket.send_to(sender_endpoint, ByteSpan(payload.data(), payload.size()), &ignored)) {
        ++feedback_sent;
    }
    delay_samples.clear();
}

StreamReceiver::StreamReceiver() : impl_(std::make_unique<Impl>()) {}

StreamReceiver::~StreamReceiver()
{
    close();
}

bool StreamReceiver::open(const Options& options, std::string* error)
{
    close();
    impl_ = std::make_unique<Impl>();
    impl_->options = options;

    if (!impl_->socket.open(options.bind_address, options.bind_port, error)) {
        return false;
    }
    if (options.rcvbuf_mb > 0) {
        std::string buffer_error;
        if (!impl_->socket.set_receive_buffer(
                static_cast<std::size_t>(options.rcvbuf_mb) * 1024u * 1024u, &buffer_error)) {
            LOWLAT_LOG_WARN("设置接收缓冲区失败（继续运行）: {}", buffer_error);
        }
    }

    impl_->nack_scheduler =
        NackScheduler(options.nack_delay_ms, NackRequest::kMaxSequences);

    BufferDepthPolicy::Config depth_config;
    depth_config.min_ms = options.depth_min_ms;
    depth_config.max_ms = options.depth_max_ms;
    depth_config.jitter_multiplier = options.jitter_k;
    depth_config.rtt_margin_ms = options.rtt_margin_ms;
    impl_->depth_policy = BufferDepthPolicy(depth_config);

    impl_->current_depth_ms = options.frame_hold_ms;
    impl_->have_frame = false;
    impl_->have_sequence = false;
    impl_->have_sender_endpoint = false;
    return true;
}

void StreamReceiver::close() noexcept
{
    if (impl_ != nullptr) {
        impl_->socket.close();
    }
    impl_ = std::make_unique<Impl>();
}

bool StreamReceiver::is_open() const noexcept
{
    return impl_ != nullptr && impl_->socket.is_open();
}

std::uint16_t StreamReceiver::local_port() const noexcept
{
    return impl_ != nullptr ? impl_->socket.local_port() : 0;
}

bool StreamReceiver::poll(int timeout_ms, std::vector<ReceivedFrame>* frames, std::string* error)
{
    if (!is_open()) {
        if (error != nullptr) {
            *error = "接收器尚未打开";
        }
        return false;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    impl_->output = frames;

    struct pollfd pfd;
    pfd.fd = impl_->socket.fd();
    pfd.events = POLLIN;
    pfd.revents = 0;

    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) {
            return true;
        }
        if (error != nullptr) {
            *error = std::string("poll 失败: ") + std::strerror(errno);
        }
        return false;
    }

    if (ready > 0) {
        std::uint8_t buffer[65536];
        // 一次唤醒后批量读，但要设上限：持续有数据时不能饿死上层
        for (int i = 0; i < kMaxPacketsPerPoll; ++i) {
            UdpEndpoint from;
            const std::ptrdiff_t received =
                impl_->socket.receive(buffer, sizeof(buffer), &from, error);
            if (received < 0) {
                return false;
            }
            if (received == 0) {
                break;
            }
            impl_->process_datagram(
                ByteSpan(buffer, static_cast<std::size_t>(received)), from);
        }
    }

    impl_->maybe_send_nack();
    impl_->maybe_send_feedback(false);
    impl_->flush_ready_frames(false);

    impl_->output = nullptr;
    return true;
}

void StreamReceiver::flush(std::vector<ReceivedFrame>* frames)
{
    if (impl_ == nullptr) {
        return;
    }
    impl_->output = frames;
    impl_->flush_ready_frames(true);
    impl_->output = nullptr;
}

std::uint64_t StreamReceiver::received_frames() const noexcept
{
    return impl_ != nullptr ? impl_->distinct_frames : 0;
}

std::uint64_t StreamReceiver::received_packets() const noexcept
{
    return impl_ != nullptr ? impl_->packets : 0;
}

ReceiverStats StreamReceiver::snapshot() const
{
    ReceiverStats stats;
    if (impl_ == nullptr) {
        return stats;
    }

    stats.packets = impl_->packets;
    stats.bytes = impl_->bytes;
    stats.malformed = impl_->malformed;
    stats.distinct_frames = impl_->distinct_frames;
    stats.frame_gaps = impl_->frame_gaps;
    stats.sequence_breaks = impl_->sequence_breaks;
    stats.nals_emitted = impl_->nals_emitted;

    stats.fec_recovered_packets = impl_->fec_recovered_packets;
    stats.nals_from_fec = impl_->nals_from_fec;
    stats.retransmits_received = impl_->retransmits_received;
    stats.nals_dropped_late = impl_->nals_dropped_late;
    stats.duplicate_nals_dropped = impl_->duplicate_nals_dropped;

    stats.nack_sent = impl_->nack_sent;
    stats.nack_sequences_requested = impl_->nack_sequences_requested;
    stats.pending_nack = impl_->nack_scheduler.pending();
    stats.feedback_sent = impl_->feedback_sent;

    stats.jitter_us = impl_->jitter.jitter_us();
    stats.srtt_us = impl_->rtt.smoothed_us();
    stats.min_rtt_us = impl_->rtt.min_us();
    stats.rtt_samples = impl_->rtt.samples();

    stats.depth_ms = impl_->current_depth_ms;
    stats.depth_min_ms = impl_->depth_min_seen;
    stats.depth_max_ms = impl_->depth_max_seen;

    const StatsSnapshot latency = impl_->latency_stats.peek();
    stats.latency_samples = latency.latency_samples;
    stats.latency_avg_ms = latency.latency_avg_ms;
    stats.latency_p50_ms = latency.latency_p50_ms;
    stats.latency_p99_ms = latency.latency_p99_ms;
    stats.latency_max_ms = latency.latency_max_ms;

    return stats;
}

}  // namespace lowlat
