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

    // 暂停恢复后重新对齐节拍基准。
    //
    // 节拍是 start_time + n*period 的绝对时基，暂停一段时间后所有 deadline
    // 都已经过期，恢复时会连续猛发一大堆帧去"追赶"，瞬间打满链路。
    // 暂停结束调用本方法把基准平移到当前时刻。
    virtual void reset_pacing() noexcept {}

    // 是否为实时源（网络流、摄像头）。
    //
    // 实时源在暂停期间**必须继续消费**：上游的 UDP 缓冲 / ffmpeg fifo 会持
    // 续攒数据，如果不读，恢复时会从几秒前的积压继续播，画面永远落后。
    // 文件源则相反——暂停就该停住，不能偷偷前进。
    virtual bool is_live() const noexcept { return false; }

    // 仅 V4L2 源使用：指定优先协商的像素格式，其他源忽略
    virtual void set_preferred_format(const std::string& /*name*/) {}
};

}  // namespace lowlat
