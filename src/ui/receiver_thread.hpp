#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "codec/decoder.hpp"
#include "transport/stream_receiver.hpp"
#include "ui/frame_slot.hpp"

namespace lowlat {

// 收包 + 解码线程。
//
// 为什么把收包和解码放在同一个线程：
//   两者都是"数据一到就要尽快处理"的活，拆开只会多一次跨线程投递；
//   UI 线程则完全不碰网络和解码，只从 FrameSlot 取成品帧。
//
// 统计数据全部是原子量，UI 线程可随时读取，无需加锁。
class ReceiverThread {
public:
    ReceiverThread();
    ~ReceiverThread();

    ReceiverThread(const ReceiverThread&) = delete;
    ReceiverThread& operator=(const ReceiverThread&) = delete;

    bool start(const StreamReceiver::Options& options, std::string* error);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
    bool failed() const;
    std::string last_error() const;

    FrameSlot& slot() noexcept { return slot_; }
    const StreamReceiver::Options& options() const noexcept { return options_; }
    std::uint16_t local_port() const noexcept;

    // 接收侧统计快照（含延迟分位数，内部会排序，别高频调用）
    ReceiverStats stats() const;

    // 解码侧计数
    std::uint64_t decoded_frames() const noexcept;
    std::uint64_t decode_errors() const noexcept;
    std::uint64_t frames_received() const noexcept;

private:
    void run();

    StreamReceiver receiver_;
    StreamReceiver::Options options_;
    FrameSlot slot_;

    // 解码器只在这个线程里被使用，不需要额外加锁
    H264Decoder decoder_;
    // 一个待解码帧对应一个采集时间戳，解码输出按先进先出配对
    std::deque<std::uint32_t> pending_capture_ts_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};

    std::atomic<std::uint64_t> decoded_frames_{0};
    std::atomic<std::uint64_t> decode_errors_{0};
    std::atomic<std::uint64_t> frames_received_{0};

    mutable std::mutex error_mutex_;
    std::string error_;
};

}  // namespace lowlat
