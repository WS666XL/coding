#include "capture/synthetic_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>

namespace lowlat {
namespace {

constexpr int kBarCount = 8;

// BT.601 彩条（Limited Range）的 YUV 分量
constexpr std::uint8_t kBarY[kBarCount] = {235, 210, 170, 145, 106, 81, 41, 16};
constexpr std::uint8_t kBarU[kBarCount] = {128, 16, 166, 54, 202, 90, 240, 128};
constexpr std::uint8_t kBarV[kBarCount] = {128, 146, 16, 34, 222, 240, 110, 128};

std::int64_t steady_now_us() noexcept
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void draw_color_bars(std::uint8_t* y_plane, std::uint8_t* u_plane, std::uint8_t* v_plane,
                     int width, int height, std::uint32_t frame_index)
{
    const int bar_width = std::max(1, width / kBarCount);
    const int cw = width / 2;
    const int ch = height / 2;

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = y_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            const int bar = std::min(x / bar_width, kBarCount - 1);
            row[x] = kBarY[bar];
        }
    }

    for (int y = 0; y < ch; ++y) {
        std::uint8_t* u_row = u_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw);
        std::uint8_t* v_row = v_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw);
        for (int x = 0; x < cw; ++x) {
            const int bar = std::min((x * 2) / bar_width, kBarCount - 1);
            u_row[x] = kBarU[bar];
            v_row[x] = kBarV[bar];
        }
    }

    // 移动竖条：位置随帧号推进。若帧序错乱或丢帧，肉眼即可看出跳跃。
    const int half = 2;
    const int pos = static_cast<int>(frame_index % static_cast<std::uint32_t>(width));
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = y_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int d = -half; d <= half; ++d) {
            const int x = pos + d;
            if (x >= 0 && x < width) {
                row[x] = 235;
            }
        }
    }
    // 竖条的色度置中性，避免出现彩色描边
    const int cpos = pos / 2;
    for (int y = 0; y < ch; ++y) {
        std::uint8_t* u_row = u_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw);
        std::uint8_t* v_row = v_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(cw);
        for (int d = -1; d <= 1; ++d) {
            const int x = cpos + d;
            if (x >= 0 && x < cw) {
                u_row[x] = 128;
                v_row[x] = 128;
            }
        }
    }

    // 左上角用 16 个黑白方块编码帧号的低 16 位，便于逐帧核对。
    constexpr int kBlock = 12;
    constexpr int kGap = 2;
    constexpr int kLeft = 8;
    constexpr int kTop = 8;
    for (int bit = 0; bit < 16; ++bit) {
        const bool on = ((frame_index >> (15 - bit)) & 1u) != 0u;
        const int bx = kLeft + bit * (kBlock + kGap);
        const std::uint8_t value = on ? 235 : 16;
        for (int dy = 0; dy < kBlock; ++dy) {
            const int y = kTop + dy;
            if (y >= height) {
                break;
            }
            std::uint8_t* row =
                y_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            for (int dx = 0; dx < kBlock; ++dx) {
                const int x = bx + dx;
                if (x < width) {
                    row[x] = value;
                }
            }
        }
    }
}

}  // namespace

SyntheticSource::SyntheticSource(int width, int height, int fps, bool realtime, int noise_level)
    : width_(width), height_(height), fps_(fps), realtime_(realtime), noise_level_(noise_level)
{
    if (noise_level_ < 0) {
        noise_level_ = 0;
    }
    if (noise_level_ > 127) {
        noise_level_ = 127;
    }
}

bool SyntheticSource::start(std::string* error)
{
    if (width_ <= 0 || height_ <= 0 || (width_ % 2 != 0) || (height_ % 2 != 0)) {
        if (error != nullptr) {
            *error = "合成源要求宽高为正偶数";
        }
        return false;
    }
    if (fps_ <= 0) {
        if (error != nullptr) {
            *error = "合成源要求 fps > 0";
        }
        return false;
    }

    const std::size_t need = static_cast<std::size_t>(width_) *
                             static_cast<std::size_t>(height_) * 3u / 2u;
    pool_ = FramePool::create(need, 4);
    if (!pool_) {
        if (error != nullptr) {
            *error = "帧池创建失败";
        }
        return false;
    }

    frame_index_ = 0;
    start_time_ = std::chrono::steady_clock::now();
    running_ = true;
    return true;
}

void SyntheticSource::stop() noexcept
{
    running_ = false;
}

std::int64_t SyntheticSource::next_frame_due_us() const noexcept
{
    if (!realtime_ || !running_ || fps_ <= 0) {
        return 0;
    }
    const auto period = std::chrono::microseconds(1000000 / fps_);
    const auto deadline = start_time_ + period * static_cast<std::int64_t>(frame_index_);
    return std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch())
        .count();
}

bool SyntheticSource::next_frame(YuvFrame* out, std::string* error)
{
    if (!running_) {
        if (error != nullptr) {
            *error = "合成源未启动";
        }
        return false;
    }
    if (out == nullptr) {
        if (error != nullptr) {
            *error = "next_frame 输出指针为空";
        }
        return false;
    }

    if (realtime_) {
        const std::chrono::microseconds period(1000000 / fps_);
        const auto deadline = start_time_ + period * static_cast<std::int64_t>(frame_index_);
        std::this_thread::sleep_until(deadline);
    }

    auto buffer = pool_->acquire();
    if (!buffer) {
        if (error != nullptr) {
            *error = "帧池耗尽";
        }
        return false;
    }

    draw_color_bars(buffer.get(), buffer.get() + static_cast<std::size_t>(width_) * height_,
                    buffer.get() + static_cast<std::size_t>(width_) * height_ +
                        static_cast<std::size_t>(width_ / 2) * (height_ / 2),
                    width_, height_, frame_index_);

                    // 叠加噪声：彩条本身太平坦，x264 会一直贴着 QP 上限输出约 100kbps，
                    // 码率控制就失去意义。加噪后内容真正需要目标码率。
                    // 用 xorshift 而不是 rand()：可复现，且不引入全局状态。
                    if (noise_level_ > 0) {
                    std::uint8_t* luma = buffer.get();
                    const std::size_t luma_size = static_cast<std::size_t>(width_) * height_;
                    for (std::size_t i = 0; i < luma_size; ++i) {
                    noise_state_ ^= noise_state_ << 13;
                    noise_state_ ^= noise_state_ >> 17;
                    noise_state_ ^= noise_state_ << 5;
                    // 映射到 [-noise_level_, +noise_level_]
                    const int delta =
                    static_cast<int>(noise_state_ % (2u * static_cast<unsigned>(noise_level_) + 1u)) -
                    noise_level_;
                    int value = static_cast<int>(luma[i]) + delta;
                    if (value < 16) {
                    value = 16;
                    } else if (value > 235) {
                    value = 235;
                    }
                    luma[i] = static_cast<std::uint8_t>(value);
                    }
                    }

                    *out = YuvFrame(frame_index_, steady_now_us(), width_, height_, std::move(buffer));
    ++frame_index_;
    return true;
}

void SyntheticSource::reset_pacing() noexcept
{
    if (fps_ <= 0) {
        return;
    }
    const auto period = std::chrono::microseconds(1000000 / fps_);
    start_time_ = std::chrono::steady_clock::now() -
                  period * static_cast<std::int64_t>(frame_index_);
}

}  // namespace lowlat
