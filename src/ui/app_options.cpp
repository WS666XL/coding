#include "ui/app_options.hpp"

#include <cstdio>
#include <cstdlib>

namespace lowlat {

void print_ui_usage()
{
    std::fprintf(stderr,
                 "用法:\n"
                 "  离线校验: lowlat_ui --input <h264> [--frame N] [--dump out.png]\n"
                 "  实时播放: lowlat_ui --bind <addr:port> [--nack] [--adaptive]\n"
                 "            [--duration sec] [--dump prefix]\n"
                 "  --wait-ms <int>   首帧渲染前的等待，默认 800\n"
                 "  --hold <ms>       帧保持窗口，默认 60\n");
}

bool parse_ui_options(int argc, char** argv, AppOptions* options, std::string* error)
{
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];

        auto next_string = [&](std::string* value) -> bool {
            if (i + 1 >= argc) {
                *error = key + " 缺少取值";
                return false;
            }
            *value = argv[++i];
            return true;
        };
        auto next_int = [&](int* value) -> bool {
            std::string text;
            if (!next_string(&text)) {
                return false;
            }
            *value = std::atoi(text.c_str());
            return true;
        };

        if (key == "--help" || key == "-h") {
            print_ui_usage();
            std::exit(0);
        } else if (key == "--input") {
            if (!next_string(&options->input)) return false;
        } else if (key == "--bind") {
            if (!next_string(&options->bind)) return false;
        } else if (key == "--frame") {
            if (!next_int(&options->frame)) return false;
        } else if (key == "--dump") {
            if (!next_string(&options->dump)) return false;
        } else if (key == "--wait-ms") {
            if (!next_int(&options->wait_ms)) return false;
        } else if (key == "--duration") {
            if (!next_int(&options->duration_sec)) return false;
        } else if (key == "--hold") {
            if (!next_int(&options->hold_ms)) return false;
        } else if (key == "--nack") {
            options->nack = true;
        } else if (key == "--feedback") {
            options->feedback = true;
        } else if (key == "--adaptive") {
            options->adaptive = true;
        } else {
            *error = "未知参数: " + key;
            return false;
        }
    }

    if (options->input.empty() && options->bind.empty()) {
        *error = "必须指定 --input 或 --bind";
        return false;
    }
    if (!options->input.empty() && !options->bind.empty()) {
        *error = "--input 与 --bind 不能同时使用";
        return false;
    }
    if (options->frame < 0 || options->wait_ms < 0 || options->duration_sec < 0) {
        *error = "帧号 / 等待时间 / 时长不能为负";
        return false;
    }
    if (options->hold_ms <= 0 || options->hold_ms > 2000) {
        *error = "hold 取值应在 1..2000 ms";
        return false;
    }
    return true;
}

}  // namespace lowlat
