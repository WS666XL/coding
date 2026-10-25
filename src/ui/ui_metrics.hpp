#pragma once

#include <cstdint>
#include <string>

namespace lowlat {

// UI 层展示用的指标汇总。
//
// 面板与日志都从同一份结构渲染，避免出现"面板显示 30fps、日志显示 28fps"
// 这种口径不一致的问题——数字不一致会让整套测量失去可信度。
struct UiMetrics {
    // 播放
    double fps{0.0};
    std::uint64_t shown{0};
    std::uint64_t received{0};
    std::uint64_t decoded{0};
    std::uint64_t dropped{0};
    std::uint64_t decode_errors{0};

    // 延迟（毫秒）
    double e2e_p50_ms{0.0};
    double e2e_p99_ms{0.0};
    double e2e_max_ms{0.0};
    double queue_avg_ms{0.0};
    double queue_max_ms{0.0};

    // 网络
    std::uint64_t packets{0};
    std::uint64_t bytes{0};
    double jitter_ms{0.0};
    double rtt_ms{0.0};
    int depth_ms{0};

    // 输入与编码（单机模式下才有意义）
    std::string source_label;      // 文件名或"合成源"
    int video_width{0};
    int video_height{0};
    int target_bitrate_kbps{0};
    std::uint64_t frames_sent{0};

    // 丢包恢复
    std::uint64_t fec_recovered{0};
    std::uint64_t nack_sent{0};
    std::uint64_t retransmits{0};
    std::uint64_t late_dropped{0};
    std::uint64_t duplicate_dropped{0};
};

}  // namespace lowlat
