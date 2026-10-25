#include "ui/receiver_thread.hpp"

#include <chrono>
#include <mutex>
#include <utility>

#include "common/log.hpp"

namespace lowlat {
namespace {

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 待解码帧的时间戳配对队列上限，防止长时间无人消费时无限增长
constexpr std::size_t kMaxPendingTimestamps = 256;

}  // namespace

ReceiverThread::ReceiverThread() = default;

ReceiverThread::~ReceiverThread()
{
    stop();
}

bool ReceiverThread::start(const StreamReceiver::Options& options, std::string* error)
{
    if (running_.load(std::memory_order_relaxed)) {
        if (error != nullptr) {
            *error = "接收线程已在运行";
        }
        return false;
    }

    if (!receiver_.open(options, error)) {
        return false;
    }
    if (!decoder_.open(error)) {
        receiver_.close();
        return false;
    }

    options_ = options;
    pending_capture_ts_.clear();
    stop_.store(false);
    failed_.store(false);
    decoded_frames_.store(0);
    decode_errors_.store(0);
    frames_received_.store(0);
    {
        std::lock_guard<std::mutex> guard(error_mutex_);
        error_.clear();
    }

    running_.store(true);
    worker_ = std::thread([this]() { run(); });
    return true;
}

void ReceiverThread::stop()
{
    if (!running_.load(std::memory_order_relaxed) && !worker_.joinable()) {
        return;
    }
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    decoder_.close();
    receiver_.close();
}

void ReceiverThread::run()
{
    std::vector<ReceivedFrame> frames;

    while (!stop_.load(std::memory_order_relaxed)) {
        frames.clear();

        std::string error;
        if (!receiver_.poll(20, &frames, &error)) {
            {
                std::lock_guard<std::mutex> guard(error_mutex_);
                error_ = error;
            }
            failed_.store(true);
            LOWLAT_LOG_ERROR("接收线程异常退出: {}", error);
            break;
        }

        for (auto& frame : frames) {
            frames_received_.fetch_add(1, std::memory_order_relaxed);

            // 采集时间戳入队，与解码输出按先进先出配对
            pending_capture_ts_.push_back(frame.capture_ts_us);
            while (pending_capture_ts_.size() > kMaxPendingTimestamps) {
                pending_capture_ts_.pop_front();
            }

            std::vector<H264Decoder::Frame> decoded;
            for (const auto& nal : frame.nals) {
                if (!decoder_.feed_nal(ByteSpan(nal.data(), nal.size()), &decoded, &error)) {
                    decode_errors_.fetch_add(1, std::memory_order_relaxed);
                    LOWLAT_LOG_EVERY_MS(1000, warn, "解码失败: {}", error);
                    break;
                }
            }

            for (auto& decoded_frame : decoded) {
                DecodedFrame entry;
                entry.capture_ts_us =
                    pending_capture_ts_.empty() ? 0 : pending_capture_ts_.front();
                if (!pending_capture_ts_.empty()) {
                    pending_capture_ts_.pop_front();
                }
                entry.frame = std::move(decoded_frame);
                entry.ready_ts_us = steady_now_us();
                slot_.push(std::move(entry));

                decoded_frames_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    running_.store(false);
}

bool ReceiverThread::failed() const
{
    return failed_.load(std::memory_order_relaxed);
}

std::string ReceiverThread::last_error() const
{
    std::lock_guard<std::mutex> guard(error_mutex_);
    return error_;
}

std::uint16_t ReceiverThread::local_port() const noexcept
{
    return receiver_.local_port();
}

ReceiverStats ReceiverThread::stats() const
{
    return receiver_.snapshot();
}

std::uint64_t ReceiverThread::decoded_frames() const noexcept
{
    return decoded_frames_.load(std::memory_order_relaxed);
}

std::uint64_t ReceiverThread::decode_errors() const noexcept
{
    return decode_errors_.load(std::memory_order_relaxed);
}

std::uint64_t ReceiverThread::frames_received() const noexcept
{
    return frames_received_.load(std::memory_order_relaxed);
}

}  // namespace lowlat
