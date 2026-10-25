#pragma once

#include <cstdint>
#include <string>

#include "capture/yuv_frame.hpp"

namespace lowlat {

// 采集源抽象。屏蔽"真实设备"与"合成源"的差异，发送流水线只依赖本接口。
class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    virtual bool start(std::string* error) = 0;
    virtual void stop() noexcept = 0;

    // 取下一帧。实时源会阻塞到下一帧到点；失败时返回 false 并写入 error。
    virtual bool next_frame(YuvFrame* out, std::string* error) = 0;

    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual int fps() const noexcept = 0;
    virtual const char* name() const noexcept = 0;

    // 下一帧到期的绝对时刻（steady_clock 微秒）。0 表示"无节拍"（由设备阻塞决定）。
    // 发送端用它在等帧的空档里阻塞收 NACK，而不是干等到 sleep 结束——
    // 否则重传会被拖到下一个帧周期，最多引入一个帧间隔的额外延迟。
    virtual std::int64_t next_frame_due_us() const noexcept { return 0; }

    // 合成源专用：叠加噪声幅度，其他源忽略
    virtual void set_noise_level(int /*level*/) noexcept {}
};

}  // namespace lowlat
