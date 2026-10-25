#include "ui/local_sender.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "capture/capture_factory.hpp"
#include "common/log.hpp"
#include "protocol/h264_fragmenter.hpp"

namespace lowlat {
namespace {

constexpr int kMaxPacketsPerPoll = 64;
constexpr int kPollTimeoutMs = 1;

}  // namespace

LocalSender::LocalSender() = default;

LocalSender::~LocalSender()
{
    stop();
}

bool LocalSender::start(const Options& options, std::string* error)
{
    if (running_.load(std::memory_order_relaxed)) {
        if (error != nullptr) {
            *error = "发送线程已在运行";
        }
        return false;
    }
    if (options.input.empty()) {
        if (error != nullptr) {
            *error = "未指定媒体文件";
        }
        return false;
    }
    if (options.destination_port == 0) {
        if (error != nullptr) {
            *error = "目标端口未设置";
        }
        return false;
    }

    options_ = options;

    // 1) 采集源（文件）：分辨率与帧率都由文件决定
    CaptureOptions capture_options;
    capture_options.source = "file";
    capture_options.input = options.input;
    capture_options.loop = options.loop;
    capture_options.realtime = true;
    capture_options.fps = 0;  // 沿用文件帧率

    std::string create_error;
    source_ = create_capture_source(capture_options, &create_error);

    // 实时源的延迟口径必须说清楚：本地拿到的帧已经被上游缓冲过，
    // 面板上的"采集→上屏"只覆盖本进程解码之后的链路，不含上游采集与网络。
    // 不说清楚的话，用户会看到"指标 79ms 但画面明显滞后"。
    if (source_ != nullptr && source_->is_live()) {
        LOWLAT_LOG_WARN("实时源：面板的端到端延迟只覆盖本进程解码之后的链路，"
                        "不含上游采集与网络缓冲");
    }
    if (!source_) {
        if (error != nullptr) {
            *error = create_error;
        }
        return false;
    }
    if (!source_->start(error)) {
        source_.reset();
        return false;
    }

    frame_width_.store(source_->width());
    frame_height_.store(source_->height());

    // 2) 编码器：必须跟采集源的实际输出尺寸一致
    encoder_ = std::make_unique<H264Encoder>();
    H264Encoder::Config encoder_config;
    encoder_config.width = source_->width();
    encoder_config.height = source_->height();
    encoder_config.fps = source_->fps();
    encoder_config.bitrate_kbps = options.bitrate_kbps;
    if (!encoder_->open(encoder_config, error)) {
        source_->stop();
        source_.reset();
        return false;
    }
    current_bitrate_kbps_.store(options.bitrate_kbps);

    // 3) 发送器
    StreamSender::Config sender_config;
    sender_config.destination.address = options.destination_address;
    sender_config.destination.port = options.destination_port;
    sender_config.enable_fec = options.fec;
    sender_config.fec_group_size = 8;
    sender_config.enable_congestion_control = options.congestion_control;
    sender_config.start_bitrate_kbps = options.bitrate_kbps;
    sender_config.min_bitrate_kbps = options.min_bitrate_kbps;
    sender_config.max_bitrate_kbps = options.max_bitrate_kbps;

    sender_ = std::make_unique<StreamSender>();
    if (!sender_->open(sender_config, error)) {
        encoder_.reset();
        source_->stop();
        source_.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(error_mutex_);
        error_.clear();
    }
    frames_sent_.store(0);
    stop_.store(false);
    running_.store(true);
    worker_ = std::thread([this]() { run(); });
    return true;
}

void LocalSender::set_paused(bool paused) noexcept
{
    const bool was = paused_.exchange(paused, std::memory_order_relaxed);
    if (was && !paused && source_) {
        // 恢复时把节拍基准拉回当前时刻，否则会为"补上"暂停期间的帧而猛发一波
        source_->reset_pacing();
    }
}

void LocalSender::stop()
{
    if (!running_.load(std::memory_order_relaxed) && !worker_.joinable()) {
        return;
    }
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    encoder_.reset();
    sender_.reset();
    if (source_) {
        source_->stop();
        source_.reset();
    }
}

void LocalSender::run()
{
    std::vector<Packet> packets;
    std::vector<ByteSpan> nals;
    std::uint32_t frame_index = 0;
    const int fps = source_->fps() > 0 ? source_->fps() : 30;

    while (!stop_.load(std::memory_order_relaxed)) {
        if (paused_.load(std::memory_order_relaxed)) {
            if (source_ != nullptr && source_->is_live()) {
                // 实时源不能"不读"。
                //
                // 暂停期间 UDP 缓冲与 ffmpeg fifo 仍在收数据（1MB fifo 在
                // 1.5Mbps 下能存 5 秒），恢复后从积压处继续播，画面就永远
                // 落后几秒——而且因为每帧都打上"当前时刻"的时间戳，
                // 面板上的延迟指标还显示正常。
                // 这里继续读取并丢弃，保证恢复时看到的是当下。
                YuvFrame discarded;
                std::string ignored;
                if (!source_->next_frame(&discarded, &ignored)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::string error;

        YuvFrame frame;
        if (!source_->next_frame(&frame, &error)) {
            if (!error.empty()) {
                std::lock_guard<std::mutex> guard(error_mutex_);
                error_ = error;
            }
            break;  // 播完且未开循环，或真的出错
        }

        // 拥塞控制：目标码率变化超过 5% 才重配编码器
        if (options_.congestion_control) {
            const int target = sender_->target_bitrate_kbps();
            const int current = encoder_->bitrate_kbps();
            if (current > 0 && std::abs(target - current) * 100 >= current * 5) {
                std::string cc_error;
                if (encoder_->set_bitrate(target, &cc_error)) {
                    current_bitrate_kbps_.store(target);
                }
            }
        }

        packets.clear();
        if (!encoder_->encode(frame, &packets, &error)) {
            std::lock_guard<std::mutex> guard(error_mutex_);
            error_ = error;
            break;
        }

        if (!packets.empty()) {
            nals.clear();
            for (const auto& packet : packets) {
                const auto spans = split_annexb(packet.data.data(), packet.data.size());
                nals.insert(nals.end(), spans.begin(), spans.end());
            }
            if (!nals.empty()) {
                const std::uint64_t scaled = static_cast<std::uint64_t>(frame_index) * 90000ull /
                                             static_cast<std::uint64_t>(fps);
                std::string send_error;
                if (!sender_->send_frame(nals, frame_index, static_cast<std::uint32_t>(scaled),
                                         frame.capture_ts_us, &send_error)) {
                    LOWLAT_LOG_EVERY_MS(1000, warn, "本机发送未完全成功: {}", send_error);
                }
            }
        }

        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        ++frame_index;

        // 用等下一帧的空档处理 NACK 与拥塞反馈，否则重传要拖一个帧周期
        std::string poll_error;
        (void)sender_->poll_incoming(kMaxPacketsPerPoll, kPollTimeoutMs, &poll_error);
    }

    running_.store(false);
}

void LocalSender::set_simulated_loss_permille(int permille) noexcept
{
    if (sender_) {
        sender_->set_simulated_loss_permille(permille);
    }
}

int LocalSender::simulated_loss_permille() const noexcept
{
    return sender_ ? sender_->simulated_loss_permille() : 0;
}

std::uint64_t LocalSender::simulated_drops() const noexcept
{
    return sender_ ? sender_->simulated_drops() : 0;
}

std::string LocalSender::last_error() const
{
    std::lock_guard<std::mutex> guard(error_mutex_);
    return error_;
}

std::uint64_t LocalSender::frames_sent() const noexcept
{
    return frames_sent_.load(std::memory_order_relaxed);
}

int LocalSender::current_bitrate_kbps() const noexcept
{
    return current_bitrate_kbps_.load(std::memory_order_relaxed);
}

int LocalSender::frame_width() const noexcept
{
    return frame_width_.load(std::memory_order_relaxed);
}

int LocalSender::frame_height() const noexcept
{
    return frame_height_.load(std::memory_order_relaxed);
}

}  // namespace lowlat
