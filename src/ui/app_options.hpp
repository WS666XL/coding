#pragma once

#include <string>

namespace lowlat {

// UI 进程的命令行选项。离线校验与实时播放共用一套解析，
// 但两者的合法组合不同（--input 与 --bind 互斥）。
struct AppOptions {
    std::string input;   // 离线模式：Annex-B 文件
    std::string bind;    // 实时模式：监听地址
    int frame{0};
    std::string dump;    // 离线：截帧路径；实时：截图前缀
    int wait_ms{800};
    int duration_sec{0};
    bool nack{false};
    bool adaptive{false};
    bool feedback{false};
    int hold_ms{60};
};

void print_ui_usage();

bool parse_ui_options(int argc, char** argv, AppOptions* options, std::string* error);

}  // namespace lowlat
