#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#include "codec/decoder.hpp"

namespace lowlat {

// 解码线程 → UI 线程的帧传递。
//
// 刻意做成"只保留最新一帧"而不是队列：
//   1. 实时视频的目标是低延迟，积压的旧帧没有播放价值
//   2. 队列一旦积压，延迟会单调增长且不可恢复；覆盖式则天然有界
//   3. UI 刷新率（60Hz）通常高于到达帧率（30fps），不需要缓冲多帧
//
// 代价是"解码快于渲染"时会丢帧——这是有意为之，并且可被统计（dropped）。
struct DecodedFrame {
    H264Decoder::Frame frame;
    std::uint32_t frame_index{0};
    std::int64_t capture_ts_us{0};  // 发送端采集时刻（steady_clock 微秒低 32 位）
    std::int64_t ready_ts_us{0};    // 本机解码完成的时刻
};

class FrameSlot {
public:
    void push(DecodedFrame frame)
    {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (pending_.has_value()) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            pending_ = std::move(frame);
        }
        pushed_.fetch_add(1, std::memory_order_relaxed);
    }

    // 取走当前最新帧；没有新帧返回 false
    bool take(DecodedFrame* out)
    {
        if (out == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        if (!pending_.has_value()) {
            return false;
        }
        *out = std::move(*pending_);
        pending_.reset();
        return true;
    }

    bool has_pending() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return pending_.has_value();
    }

    std::uint64_t pushed() const noexcept { return pushed_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    void clear()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        pending_.reset();
    }

private:
    mutable std::mutex mutex_;
    std::optional<DecodedFrame> pending_;
    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace lowlat
