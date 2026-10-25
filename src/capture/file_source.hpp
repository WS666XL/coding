#pragma once

#include <memory>
#include <string>

#include "capture/capture_source.hpp"

namespace lowlat {

// 文件采集源：把任意媒体文件解码成 I420 帧，按帧率节奏输出。
//
// 存在的理由：这台开发机是 WSL2，拿不到 /dev/video*，V4L2 采集写了也验证不了。
// 有文件源之后，任何一段 mp4/mkv/avi 都能当作"摄像头"，把
// 采集 -> 编码 -> 打包 -> 传输 -> 重组 -> 解码 -> 渲染 整条链路跑满。
//
// 输出的分辨率以文件为准（不是调用方指定的），因此发送端必须在 start() 之后
// 用 width()/height() 重新配置编码器。
class FileSource : public CaptureSource {
public:
    // fps <= 0 表示沿用文件本身的帧率；loop 为 true 时播完自动重头开始。
    FileSource(std::string path, int fps, bool loop, bool realtime);
    ~FileSource() override;

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    bool start(std::string* error) override;
    void stop() noexcept override;
    bool next_frame(YuvFrame* out, std::string* error) override;

    int width() const noexcept override;
    int height() const noexcept override;
    int fps() const noexcept override;
    const char* name() const noexcept override { return "file"; }

    std::int64_t next_frame_due_us() const noexcept override;
    void reset_pacing() noexcept override;

    // 文件路径（日志用）
    const std::string& path() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lowlat
