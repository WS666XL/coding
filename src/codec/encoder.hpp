#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "capture/yuv_frame.hpp"

struct x264_t;

namespace lowlat {

// 编码器输出的一个 NAL 单元（b_annexb 模式下 payload 已含起始码）。
struct Packet {
    std::vector<std::uint8_t> data;
    std::int64_t pts_us{0};
    std::uint32_t frame_index{0};
    bool keyframe{false};
};

// libx264 封装。固定零延迟配置：无 B 帧、无 lookahead、切片多线程，
// 保证"输入一帧 -> 立即输出"的实时语义。
class H264Encoder {
public:
    struct Config {
        int width{1280};
        int height{720};
        int fps{30};
        int bitrate_kbps{2000};
        int keyint_sec{2};
        std::string preset{"veryfast"};
        std::string tune{"zerolatency"};
    };

    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool open(const Config& config, std::string* error);
    void close() noexcept;

    bool is_open() const noexcept { return handle_ != nullptr; }

    // 编码一帧，结果追加到 out。零延迟配置下通常当帧即产出。
    bool encode(const YuvFrame& frame, std::vector<Packet>* out, std::string* error);

    // 冲刷编码器内部残留（零延迟配置下一般为空）。
    bool flush(std::vector<Packet>* out, std::string* error);

    // 运行期动态调整目标码率（ABR）。
    bool set_bitrate(int bitrate_kbps, std::string* error);

    int width() const noexcept { return config_.width; }
    int height() const noexcept { return config_.height; }
    int fps() const noexcept { return config_.fps; }
    int bitrate_kbps() const noexcept { return config_.bitrate_kbps; }

private:
    x264_t* handle_{nullptr};
    Config config_{};
};

}  // namespace lowlat
