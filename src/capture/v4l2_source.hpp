#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/frame_pool.hpp"

#include "capture/capture_source.hpp"
#include "capture/v4l2_converter.hpp"

namespace lowlat {

// V4L2 单平面视频采集：V4L2_PIX_FMT_YUV420 + V4L2_MEMORY_MMAP 零拷贝。
//
// 零拷贝策略
//   DQBUF 拿到缓冲区后，直接把 mmap 映射区包装成 shared_ptr，自定义 deleter 在
//   最后一个引用释放时把该缓冲区重新 QBUF 回驱动。全程没有 memcpy，帧对象可以
//   跨线程传递，代价是必须在缓冲区用尽前释放（缓冲区数量即最大在途帧数）。
//
// 资源生命周期
//   fd / mmap 映射 / 流状态统一由内部 Device 持有，Device 被所有在途帧共同引用。
//   即使 V4l2Source 先销毁，已借出的帧依然可以安全读取，直到最后一个帧释放后
//   才 munmap 并 close，避免 use-after-munmap。
//
// 前置要求
//   驱动必须返回紧凑布局（bytesperline == width）。v4l2loopback 与绝大多数
//   UVC 摄像头的 YUV420 输出都满足该条件；不满足时直接报错而不静默错位。
class V4l2Source final : public CaptureSource {
public:
    V4l2Source(std::string device, int width, int height, int fps,
               std::uint32_t buffer_count = 4);
    ~V4l2Source() override;

    V4l2Source(const V4l2Source&) = delete;
    V4l2Source& operator=(const V4l2Source&) = delete;

    bool start(std::string* error) override;
    void stop() noexcept override;
    bool next_frame(YuvFrame* out, std::string* error) override;

    int width() const noexcept override;
    int height() const noexcept override;
    int fps() const noexcept override { return fps_; }
    const char* name() const noexcept override { return "v4l2"; }

    const std::string& device() const noexcept { return device_; }

    // 指定优先协商的像素格式（"yuv420" / "yuyv" / "mjpg"），空表示自动。
    // 自动顺序为 YUV420 > MJPEG > YUYV：MJPEG 排在 YUYV 前面是因为
    // 同样 1280x720 下 YUYV 往往只有 10fps（未压缩 4:2:2 带宽太高），
    // 而 MJPEG 能跑满 30fps——实测的笔记本内置摄像头就是这样。
    void set_preferred_format(const std::string& name) override;
    // 实际协商到的格式名（YUV420 / YUYV / MJPG）
    const char* pixel_format_name() const noexcept { return converter_.format_name(); }
    std::uint32_t buffer_count() const noexcept { return buffer_count_; }

    // 归还缓冲区失败的累计次数，用于判断驱动是否异常。
    std::uint64_t requeue_failures() const noexcept;

private:
    struct Device;

    std::shared_ptr<Device> device_handle_;
    std::string device_;
    int requested_width_;
    int requested_height_;
    int fps_;
    std::string preferred_format_;
    std::uint32_t buffer_count_;
    int actual_width_{0};
    int actual_height_{0};
    // 格式协商结果。YUV420 且 stride==width 时走零拷贝，其余格式需要转换拷贝。
    V4l2Converter converter_;
    std::shared_ptr<FramePool> pool_;
    std::uint32_t frame_index_{0};
    bool streaming_{false};
};

}  // namespace lowlat
