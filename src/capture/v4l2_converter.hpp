#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lowlat {

// V4L2 像素格式 -> 紧凑 I420 的转换器。
//
// 为什么需要：UVC 摄像头几乎不提供 YUV420，实测一台常见的笔记本内置摄像头
// 只给了 MJPG 与 YUYV 两种格式。原实现只接受 YUV420，任何真实摄像头都会在
// VIDIOC_S_FMT 阶段协商失败——等于"代码写了但接不上任何设备"。
//
// 四条路径：
//   1. YUV420 且 stride == width  -> 零拷贝，不经过本类（调用方直接包装 mmap 缓冲）
//   2. YUV420 但 stride > width   -> 逐行搬移，去掉 padding
//   3. YUYV                       -> 手写 2x2 下采样，不需要 libswscale
//   4. MJPG                       -> libavcodec 解码 + libswscale 转 I420
//
// 2~4 都需要一次拷贝（转换本身就要求写目标缓冲），这是格式不支持零拷贝的代价。
class V4l2Converter {
public:
    V4l2Converter();
    ~V4l2Converter();

    V4l2Converter(const V4l2Converter&) = delete;
    V4l2Converter& operator=(const V4l2Converter&) = delete;

    // v4l2_pixel_format 是 V4L2_PIX_FMT_* 常量。
    // bytes_per_line 是驱动返回的 bytesperline（可能大于 width，存在行填充）。
    bool configure(std::uint32_t v4l2_pixel_format, int width, int height,
                   std::size_t bytes_per_line, std::string* error);

    void reset() noexcept;

    // YUV420 且 stride == width 时为 true，此时调用方应走零拷贝路径。
    bool is_passthrough() const noexcept;

    // 转换后一帧 I420 的字节数：width * height * 3 / 2
    std::size_t output_size() const noexcept;

    // 供日志与排障使用，如 "YUYV" / "MJPG" / "YUV420"
    const char* format_name() const noexcept;

    // src 为驱动缓冲（size 字节），dst 至少 output_size() 字节。
    bool convert(const std::uint8_t* src, std::size_t size, std::uint8_t* dst,
                 std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lowlat
