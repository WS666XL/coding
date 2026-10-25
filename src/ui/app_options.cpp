#include "ui/app_options.hpp"

#include <cstdio>
#include <cstdlib>

namespace lowlat {

void print_ui_usage()
{
    std::fprintf(stderr,
                 "用法:\n"
                 "  单机播放文件: lowlat_ui --local <媒体文件>     （推荐，一个窗口跑完整链路）\n"
                 "  接收远端:     lowlat_ui --bind <addr:port> [--nack] [--adaptive]\n"
                 "  离线校验:     lowlat_ui --input <h264> [--frame N] [--dump out.png]\n"
                 "  --wait-ms <int>   首帧渲染前的等待，默认 800\n"
                 "  --hold <ms>       帧保持窗口，默认 60\n"
                 "  --bitrate <kbps>  单机模式的编码码率，默认 2000\n");
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
        } else if (key == "--local") {
            if (!next_string(&options->local)) return false;
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
        } else if (key == "--bitrate") {
            if (!next_int(&options->bitrate_kbps)) return false;
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

    const int modes = (!options->input.empty() ? 1 : 0) + (!options->bind.empty() ? 1 : 0) +
                      (!options->local.empty() ? 1 : 0);
    if (modes == 0) {
        *error = "必须指定 --local（单机播放文件）、--input（离线校验）或 --bind（接收远端）";
        return false;
    }
    if (modes > 1) {
        *error = "--local / --input / --bind 三者只能选一个";
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
