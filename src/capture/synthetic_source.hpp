#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "capture/capture_source.hpp"
#include "common/frame_pool.hpp"

namespace lowlat {

// 合成测试源：生成彩条 + 移动竖条 + 帧序号二进制的 I420 图像。
//
// 无摄像头时的兜底，也是编码调参和自动化测试的唯一可靠输入。
// realtime == false 时不按帧率等待，用于单元测试。
class SyntheticSource final : public CaptureSource {
public:
    // noise_level: 叠加到 Y 平面的随机噪声幅度（0 表示不加）。
    //
    // 为什么需要噪声：彩条画面太平坦，x264 无论目标码率设多少都只编出
    // 约 100 kbps（已经贴着 QP 上限）。这种情况下码率控制环控制的变量
    // 对实际输出没有影响，压测"码率自适应"就会得到假结果。
    // 加噪声可以造出真正需要高码率的内容。
    SyntheticSource(int width, int height, int fps, bool realtime = true, int noise_level = 0);

    bool start(std::string* error) override;
    void stop() noexcept override;
    bool next_frame(YuvFrame* out, std::string* error) override;

    int width() const noexcept override { return width_; }
    int height() const noexcept override { return height_; }
    int fps() const noexcept override { return fps_; }
    const char* name() const noexcept override { return "synthetic"; }
    std::int64_t next_frame_due_us() const noexcept override;
    void set_noise_level(int level) noexcept override { noise_level_ = level; }

    // 当前已产出的帧数，供测试断言使用。
    std::uint32_t produced() const noexcept { return frame_index_; }

private:
    int width_;
    int height_;
    int fps_;
    bool realtime_;
    int noise_level_{0};
    std::uint32_t noise_state_{0x9E3779B9u};
    bool running_{false};
    std::uint32_t frame_index_{0};
    std::chrono::steady_clock::time_point start_time_{};
    std::shared_ptr<FramePool> pool_;
};

}  // namespace lowlat
