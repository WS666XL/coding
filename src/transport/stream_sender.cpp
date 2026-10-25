#include "transport/stream_sender.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <poll.h>

#include "protocol/h264_fragmenter.hpp"
#include "protocol/nack.hpp"

namespace lowlat {

namespace {

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 把交织的 32 位微秒时间戳展开成 64 位单调值。
// 跨反馈报文时如果恰好发生 2^32 回绕，直接相减会得到巨大的负值，
// 延迟梯度会被误判成"链路突然变快"，从而错误提速。
std::int64_t unwrap32(std::uint32_t value, std::int64_t* last) noexcept
{
    if (*last < 0) {
        *last = static_cast<std::int64_t>(value);
        return *last;
    }
    std::int64_t candidate =
        (*last & ~static_cast<std::int64_t>(0xFFFFFFFFLL)) | static_cast<std::int64_t>(value);
    if (candidate < *last - 0x80000000LL) {
        candidate += 0x100000000LL;
    } else if (candidate > *last + 0x80000000LL) {
        candidate -= 0x100000000LL;
    }
    *last = candidate;
    return candidate;
}

}  // namespace

RetransmitCache::RetransmitCache(std::size_t capacity)
    : slots_(capacity == 0 ? 1 : capacity),
      sequences_(capacity == 0 ? 1 : capacity, 0),
      valid_(capacity == 0 ? 1 : capacity, false)
{
}

void RetransmitCache::store(std::uint16_t sequence, ByteSpan packet)
{
    if (slots_.empty() || packet.empty()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(sequence) % slots_.size();
    slots_[index].assign(packet.begin(), packet.end());
    sequences_[index] = sequence;
    valid_[index] = true;
}

const std::vector<std::uint8_t>* RetransmitCache::find(std::uint16_t sequence) const noexcept
{
    if (slots_.empty()) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(sequence) % slots_.size();
    if (!valid_[index] || sequences_[index] != sequence) {
        return nullptr;
    }
    return &slots_[index];
}

bool StreamSender::open(const Config& config, std::string* error)
{
    close();

    if (!config.destination.valid()) {
        if (error != nullptr) {
            *error = "发送目标非法（端口为 0）";
        }
        return false;
    }
    if (config.max_payload < RtpLiteHeader::kBaseSize + RtpLiteHeader::kExtensionSize + 3) {
        if (error != nullptr) {
            *error = "max_payload 太小，装不下 16 字节头 + FU-A 头";
        }
        return false;
    }

    if (!socket_.open("", 0, error)) {
        return false;
    }
    if (!socket_.set_destination(config.destination, error)) {
        close();
        return false;
    }
    if (!socket_.set_send_buffer(config.send_buffer_bytes, error)) {
        close();
        return false;
    }

    // 修复包载荷预算 = 单个 UDP 载荷上限减去 RTP 头
    XorFecEncoder::Config fec_config;
    fec_config.group_size = config.fec_group_size;
    fec_config.max_parity_bytes =
        config.max_payload - RtpLiteHeader::kBaseSize - RtpLiteHeader::kExtensionSize;
    fec_encoder_ = XorFecEncoder(fec_config);

    cache_ = RetransmitCache(config.retransmit_capacity);

    BandwidthEstimator::Config estimator_config;
    estimator_config.start_bps = static_cast<std::int64_t>(config.start_bitrate_kbps) * 1000;
    estimator_config.min_bps = static_cast<std::int64_t>(config.min_bitrate_kbps) * 1000;
    estimator_config.max_bps = static_cast<std::int64_t>(config.max_bitrate_kbps) * 1000;
    estimator_ = BandwidthEstimator(estimator_config);
    feedback_received_ = 0;

    config_ = config;
    sequence_ = 0;
    packets_sent_ = 0;
    bytes_sent_ = 0;
    would_block_events_ = 0;
    simulated_loss_permille_ = 0;
    simulated_drops_ = 0;
    rng_state_ = 0x2545F491u;
    packets_dropped_ = 0;
    fec_packets_sent_ = 0;
    fec_group_failures_ = 0;
    nack_received_ = 0;
    retransmit_packets_sent_ = 0;
    nack_unserviceable_ = 0;
    unwrap_send_us_ = -1;
    unwrap_arrival_us_ = -1;
    return true;
}

void StreamSender::set_simulated_loss_permille(int permille) noexcept
{
    if (permille < 0) {
        permille = 0;
    }
    if (permille > 1000) {
        permille = 1000;
    }
    simulated_loss_permille_ = permille;
}

std::uint32_t StreamSender::next_random(std::uint32_t bound) noexcept
{
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 17;
    rng_state_ ^= rng_state_ << 5;
    return bound == 0 ? 0 : (rng_state_ % bound);
}

int StreamSender::target_bitrate_kbps() const noexcept
{
    if (!config_.enable_congestion_control) {
        return config_.start_bitrate_kbps;
    }
    return static_cast<int>((estimator_.target_bps() + 500) / 1000);
}

void StreamSender::close() noexcept
{
    socket_.close();
}

bool StreamSender::send_frame(const std::vector<ByteSpan>& nals, std::uint32_t frame_index,
                              std::uint32_t timestamp, std::uint32_t capture_ts_us,
                              std::string* error)
{
    if (!socket_.is_open()) {
        if (error != nullptr) {
            *error = "发送器尚未打开";
        }
        return false;
    }
    if (nals.empty()) {
        if (error != nullptr) {
            *error = "send_frame 收到空的 NAL 列表";
        }
        return false;
    }

    buffers_.clear();
    spans_.clear();

    // 先收集所有分片，才能确定哪一个是本帧最后一个包（用于置 marker 位）
    struct Pending {
        std::vector<std::uint8_t> payload;
    };
    std::vector<Pending> pending;

    for (const ByteSpan& nal : nals) {
        std::vector<PayloadSlice> slices;
        if (!fragment_nal(nal, config_.max_payload - RtpLiteHeader::kBaseSize -
                                  RtpLiteHeader::kExtensionSize,
                          &slices, error)) {
            return false;
        }
        for (auto& slice : slices) {
            Pending item;
            item.payload = std::move(slice.data);
            pending.push_back(std::move(item));
        }
    }

    if (pending.empty()) {
        if (error != nullptr) {
            *error = "分片结果为空";
        }
        return false;
    }

    std::vector<std::uint16_t> data_sequences;
    data_sequences.reserve(pending.size());
    std::uint16_t last_data_sequence = 0;

    buffers_.reserve(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        RtpLiteHeader header;
        header.has_frame_index = true;
        header.marker = (i + 1 == pending.size());
        header.sequence = sequence_;
        header.timestamp = timestamp;
        header.ssrc = config_.ssrc;
        header.frame_index = frame_index;
        header.capture_ts_us = capture_ts_us;

        std::vector<std::uint8_t> packet;
        packet.resize(header.serialized_size() + pending[i].payload.size());
        serialize_rtp_lite(header, packet.data());
        std::copy(pending[i].payload.begin(), pending[i].payload.end(),
                  packet.begin() + static_cast<std::ptrdiff_t>(header.serialized_size()));

        data_sequences.push_back(sequence_);
        last_data_sequence = sequence_;
        ++sequence_;  // uint16 自然回绕，符合 RTP 语义
        buffers_.push_back(std::move(packet));

        // 只缓存数据包：修复包本身不需要重传
        cache_.store(data_sequences.back(),
                     ByteSpan(buffers_.back().data(), buffers_.back().size()));
    }

    // FEC：按数据包顺序分组，组满时追加一个修复包。
    // 修复包紧跟在同组数据包之后入队，保证 sendmmsg 的发出顺序正确。
    if (config_.enable_fec) {
        // 局部 lambda：把一个修复载荷包装成 FEC 包追加到发送队列
        auto append_repair = [&](const std::vector<std::uint8_t>& repair_payload) {
            RtpLiteHeader header;
            header.has_frame_index = true;
            header.marker = false;
            header.payload_type = RtpLiteHeader::kPayloadTypeFec;
            // 修复包不占用数据序列号空间：复用同组最后一个数据包的序列号。
            // 这样接收端"序列号是否连续"仍然只反映真实的数据包丢失，
            // 不会被修复包插队误判为丢包。代价是线上出现重复序列号，
            // 但本协议用 payload_type 区分数据/修复，且不使用修复包的序列号。
            header.sequence = last_data_sequence;
            header.timestamp = timestamp;
            header.ssrc = config_.ssrc;
            header.frame_index = frame_index;
            header.capture_ts_us = capture_ts_us;

            std::vector<std::uint8_t> packet(header.serialized_size() + repair_payload.size());
            serialize_rtp_lite(header, packet.data());
            std::copy(repair_payload.begin(), repair_payload.end(),
                      packet.begin() + static_cast<std::ptrdiff_t>(header.serialized_size()));

            ++fec_packets_sent_;
            buffers_.push_back(std::move(packet));
        };

        const std::size_t data_count = buffers_.size();
        for (std::size_t i = 0; i < data_count; ++i) {
            std::vector<std::uint8_t> repair_payload;
            bool produced = false;
            if (!fec_encoder_.add(data_sequences[i],
                                  ByteSpan(buffers_[i].data(), buffers_[i].size()),
                                  &repair_payload, &produced, error)) {
                // 单组失败不应打断整帧：该组作废，数据包照常发送
                ++fec_group_failures_;
                if (error != nullptr) {
                    error->clear();
                }
                continue;
            }
            if (produced) {
                append_repair(repair_payload);
            }
        }

        // 帧边界：把未满的组也结算掉，保证修复包与本帧数据在同一批里发出，
        // 收端能在本帧内完成恢复，避免 NAL 顺序错乱。
        std::vector<std::uint8_t> tail_repair;
        bool tail_produced = false;
        if (fec_encoder_.flush(&tail_repair, &tail_produced, error) && tail_produced) {
            append_repair(tail_repair);
        } else if (error != nullptr) {
            error->clear();
        }
    }

    // 模拟丢包：必须在建 spans 之前把 buffers_ 里的元素删掉。
    // 注意缓存是在前面填的，所以被"丢"掉的包仍然可以被 NACK 重传回来，
    // 这与真实链路丢包后的可恢复性一致。
    if (simulated_loss_permille_ > 0 && !buffers_.empty()) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            if (next_random(1000) < static_cast<std::uint32_t>(simulated_loss_permille_)) {
                ++simulated_drops_;
                continue;
            }
            if (kept != i) {
                buffers_[kept] = std::move(buffers_[i]);
            }
            ++kept;
        }
        buffers_.resize(kept);
    }

    spans_.reserve(buffers_.size());
    for (const auto& buffer : buffers_) {
        spans_.push_back(ByteSpan(buffer.data(), buffer.size()));
    }

    const SendReport report = socket_.send_batch(spans_.data(), spans_.size(), error);
    packets_sent_ += report.sent;
    for (std::size_t i = 0; i < report.sent; ++i) {
        bytes_sent_ += spans_[i].size();
    }
    if (report.sent < spans_.size()) {
        packets_dropped_ += spans_.size() - report.sent;
        if (report.would_block) {
            ++would_block_events_;
        }
    }

    return report.sent == spans_.size();
}

std::size_t StreamSender::poll_incoming(std::size_t max_datagrams, int timeout_ms,
                                        std::string* error)
{
    if (!socket_.is_open() || max_datagrams == 0) {
        return 0;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    std::size_t handled = 0;
    std::uint8_t buffer[2048];
    const auto started = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < max_datagrams; ++i) {
        // 有超时预算就阻塞等，预算用完就转为非阻塞收尾
        int wait_ms = 0;
        if (timeout_ms > 0) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count();
            wait_ms = timeout_ms - static_cast<int>(elapsed_ms);
            if (wait_ms < 0) {
                wait_ms = 0;
            }
        }

        struct pollfd pfd;
        pfd.fd = socket_.fd();
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int ready = ::poll(&pfd, 1, wait_ms);
        if (ready <= 0) {
            break;  // 超时或出错：把时间还给上层
        }

        const std::ptrdiff_t received =
            socket_.receive(buffer, sizeof(buffer), nullptr, error);
        if (received < 0) {
            return handled;
        }
        if (received == 0) {
            break;
        }

        const ByteSpan span(buffer, static_cast<std::size_t>(received));
        ControlType control_type{};
        if (!peek_control_type(span, &control_type)) {
            continue;  // 未知报文：忽略，不影响发送
        }

        if (control_type == ControlType::Feedback) {
            FeedbackReport report;
            if (!parse_feedback(span, &report)) {
                continue;
            }
            ++feedback_received_;

            // 丢包率是千分比，换算成比例交给估计器
            estimator_.on_loss_report(static_cast<double>(report.loss_permille) / 1000.0,
                                      steady_now_us());

            for (const DelaySample& sample : report.samples) {
                const std::int64_t send_us = unwrap32(sample.capture_us, &unwrap_send_us_);
                const std::int64_t arrival_us = unwrap32(sample.arrival_us, &unwrap_arrival_us_);
                estimator_.on_sample(send_us, arrival_us, arrival_us);
            }
            ++handled;
            continue;
        }

        NackRequest request;
        if (!parse_nack_control(span, &request)) {
            continue;
        }
        if (request.empty()) {
            continue;
        }

        ++nack_received_;
        for (const std::uint16_t sequence : request.sequences) {
            const std::vector<std::uint8_t>* cached = cache_.find(sequence);
            if (cached == nullptr) {
                // 太旧的包已被环形缓存覆盖：放弃重传，让接收端走 FEC 或直接丢帧
                ++nack_unserviceable_;
                continue;
            }
            if (socket_.send(ByteSpan(cached->data(), cached->size()), error)) {
                ++retransmit_packets_sent_;
            } else {
                ++nack_unserviceable_;
            }
        }
        ++handled;
    }
    return handled;
}

}  // namespace lowlat
