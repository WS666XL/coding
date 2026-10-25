#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "capture/capture_source.hpp"

namespace lowlat {

struct CaptureOptions {
    std::string source{"synthetic"};  // synthetic | v4l2 | file
    std::string device{"/dev/video0"};
    std::string input;   // source=file 时的媒体文件路径
    bool loop{false};    // source=file 时循环播放
    int width{1280};
    int height{720};
    int fps{30};  // 0 表示沿用文件帧率（仅 source=file）
    bool realtime{true};
    std::uint32_t buffer_count{4};
};

// 构造采集源。未知 source 名或参数非法时返回 nullptr 并写入 error。
std::unique_ptr<CaptureSource> create_capture_source(const CaptureOptions& options,
                                                     std::string* error);

}  // namespace lowlat
