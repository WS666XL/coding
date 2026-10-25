// 发送端入口：采集 -> x264 编码 -> 打包 -> 落盘 或 UDP 发送
//
// 两种输出：
//   --dest 为空     落盘 .h264（P1 行为）
//   --dest host:port 走 RTP-lite over UDP
//
// 退出码约定：
//   0 正常结束
//   1 参数错误
//   2 运行时错误（采集 / 编码 / 写文件 / 网络失败）
//   3 被信号中断（SIGINT / SIGTERM）

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "capture/capture_factory.hpp"
#include "codec/encoder.hpp"
#include "common/log.hpp"
#include "common/stats.hpp"
#include "protocol/h264_fragmenter.hpp"
#include "transport/stream_sender.hpp"
#include "transport/udp_socket.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int /*signum*/)
{
    g_stop.store(true);
}

struct Options {
    std::string source{"synthetic"};
    std::string device{"/dev/video0"};
    std::string input;       // source=file 时的媒体文件
    bool loop{false};        // 文件源循环播放
    bool fps_explicit{false};
    int width{1280};
    int height{720};
    int fps{30};
    int bitrate_kbps{2000};
    bool congestion_control{false};
    int min_bitrate_kbps{300};
    int max_bitrate_kbps{4000};
    int noise_level{0};
    int frames{0};        // 0 表示不限
    int duration_sec{0};  // 0 表示不限
    int keyint_sec{2};
    std::string output{"out.h264"};
    std::string dest{};  // 空表示落盘模式
    int max_payload{1200};
    bool fec{false};
    int fec_group{8};
    std::string log_level{"info"};
    bool realtime{true};
};

void print_usage()
{
    std::fprintf(stderr,
                 "用法: lowlat_sender [选项]\n"
                 "  --source <synthetic|v4l2|file>  采集源，默认 synthetic\n"
                 "  --device <path>            V4L2 设备节点，默认 /dev/video0\n"
                 "  --input <path>             文件源要播放的媒体文件（mp4/mkv/avi...）\n"
                 "  --loop                     文件源播完循环\n"
                 "  --width <int>              宽度，默认 1280\n"
                 "  --height <int>             高度，默认 720\n"
                 "  --fps <int>                帧率，默认 30\n"
                 "  --bitrate <kbps>           目标码率，默认 2000\n"
                 "  --frames <int>             编码帧数上限，0 表示不限\n"
                 "  --duration <sec>           时长上限，0 表示不限\n"
                 "  --keyint <sec>             关键帧间隔秒数，默认 2\n"
                 "  --output <path>            输出文件，默认 out.h264（仅落盘模式）\n"
                 "  --dest <host:port>         UDP 目标；设置后进入网络模式\n"
                 "  --max-payload <bytes>      单个 UDP 载荷上限，默认 1200\n"
                 "  --cc                       开启拥塞控制（按接收端反馈调整码率）\n"
                 "  --min-bitrate <kbps>       拥塞控制下限，默认 300\n"
                 "  --max-bitrate <kbps>       拥塞控制上限，默认 4000\n"
                 "  --noise <0-127>            合成源叠加噪声幅度（0 关闭，>0 用于码率压测）\n"
                 "  --fec                      开启 XOR FEC（每帧追加修复包）\n"
                 "  --fec-group <int>          FEC 组大小，默认 8\n"
                 "  --log-level <level>        trace|debug|info|warn|error\n"
                 "  --no-realtime              不按帧率等待（压测用）\n"
                 "  --help                     显示本帮助\n");
}

bool parse_int(const char* text, int* out)
{
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool parse_args(int argc, char** argv, Options* opt, std::string* error)
{
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto take_value = [&](const char* name, std::string* value) -> bool {
            if (i + 1 >= argc) {
                *error = std::string(name) + " 缺少取值";
                return false;
            }
            *value = argv[++i];
            return true;
        };
        auto take_int = [&](const char* name, int* value) -> bool {
            if (i + 1 >= argc) {
                *error = std::string(name) + " 缺少取值";
                return false;
            }
            if (!parse_int(argv[++i], value)) {
                *error = std::string(name) + " 取值不是整数: " + argv[i];
                return false;
            }
            return true;
        };

        if (key == "--help" || key == "-h") {
            print_usage();
            std::exit(0);
        } else if (key == "--source") {
            if (!take_value("--source", &opt->source)) return false;
        } else if (key == "--device") {
            if (!take_value("--device", &opt->device)) return false;
        } else if (key == "--input") {
            if (!take_value("--input", &opt->input)) return false;
        } else if (key == "--loop") {
            opt->loop = true;
        } else if (key == "--width") {
            if (!take_int("--width", &opt->width)) return false;
        } else if (key == "--height") {
            if (!take_int("--height", &opt->height)) return false;
        } else if (key == "--fps") {
            if (!take_int("--fps", &opt->fps)) return false;
            opt->fps_explicit = true;
        } else if (key == "--bitrate") {
            if (!take_int("--bitrate", &opt->bitrate_kbps)) return false;
        } else if (key == "--frames") {
            if (!take_int("--frames", &opt->frames)) return false;
        } else if (key == "--duration") {
            if (!take_int("--duration", &opt->duration_sec)) return false;
        } else if (key == "--keyint") {
            if (!take_int("--keyint", &opt->keyint_sec)) return false;
        } else if (key == "--output") {
            if (!take_value("--output", &opt->output)) return false;
        } else if (key == "--dest") {
            if (!take_value("--dest", &opt->dest)) return false;
        } else if (key == "--noise") {
            if (!take_int("--noise", &opt->noise_level)) return false;
        } else if (key == "--cc") {
            opt->congestion_control = true;
        } else if (key == "--min-bitrate") {
            if (!take_int("--min-bitrate", &opt->min_bitrate_kbps)) return false;
        } else if (key == "--max-bitrate") {
            if (!take_int("--max-bitrate", &opt->max_bitrate_kbps)) return false;
        } else if (key == "--max-payload") {
            if (!take_int("--max-payload", &opt->max_payload)) return false;
        } else if (key == "--fec") {
            opt->fec = true;
        } else if (key == "--fec-group") {
            if (!take_int("--fec-group", &opt->fec_group)) return false;
        } else if (key == "--log-level") {
            if (!take_value("--log-level", &opt->log_level)) return false;
        } else if (key == "--no-realtime") {
            opt->realtime = false;
        } else {
            *error = "未知参数: " + key;
            return false;
        }
    }

    if (opt->width <= 0 || opt->height <= 0 || (opt->width % 2) != 0 || (opt->height % 2) != 0) {
        *error = "宽高必须为正偶数";
        return false;
    }
    if (opt->fps <= 0) {
        *error = "fps 必须为正数";
        return false;
    }
    if (opt->bitrate_kbps <= 0) {
        *error = "码率必须为正数";
        return false;
    }
    if (opt->frames < 0 || opt->duration_sec < 0) {
        *error = "帧数与时长不能为负";
        return false;
    }
    if (opt->max_payload < 32) {
        *error = "max_payload 至少 32 字节";
        return false;
    }
    return true;
}

lowlat::LogLevel to_log_level(const std::string& text)
{
    if (text == "trace") return lowlat::LogLevel::Trace;
    if (text == "debug") return lowlat::LogLevel::Debug;
    if (text == "warn") return lowlat::LogLevel::Warn;
    if (text == "error") return lowlat::LogLevel::Error;
    return lowlat::LogLevel::Info;
}

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool install_signal_handlers()
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, nullptr) != 0) {
        return false;
    }
    if (sigaction(SIGTERM, &action, nullptr) != 0) {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    std::string error;
    if (!parse_args(argc, argv, &opt, &error)) {
        std::fprintf(stderr, "参数错误: %s\n", error.c_str());
        print_usage();
        return 1;
    }

    // 网络模式：目标地址在启动任何资源之前先校验，避免白跑一遍采集
    const bool network_mode = !opt.dest.empty();
    lowlat::UdpEndpoint destination;
    if (network_mode && !lowlat::parse_endpoint(opt.dest, &destination, &error)) {
        std::fprintf(stderr, "目标地址错误: %s\n", error.c_str());
        print_usage();
        return 1;
    }

    lowlat::LogConfig log_config;
    log_config.level = to_log_level(opt.log_level);
    if (!lowlat::Logger::instance().init(log_config)) {
        std::fprintf(stderr, "日志初始化失败，降级为默认控制台输出\n");
    }

    if (!install_signal_handlers()) {
        LOWLAT_LOG_ERROR("信号处理器安装失败");
        return 2;
    }

    lowlat::CaptureOptions capture_options;
    capture_options.source = opt.source;
    capture_options.device = opt.device;
    capture_options.width = opt.width;
    capture_options.height = opt.height;
    // 文件源默认沿用文件自身帧率；显式给了 --fps 才覆盖
    capture_options.fps = (opt.source == "file" && !opt.fps_explicit) ? 0 : opt.fps;
    capture_options.input = opt.input;
    capture_options.loop = opt.loop;
    capture_options.realtime = opt.realtime;

    std::string create_error;
    auto source = lowlat::create_capture_source(capture_options, &create_error);
    if (!source) {
        std::fprintf(stderr, "创建采集源失败: %s\n", create_error.c_str());
        print_usage();
        return 1;
    }

    if (!source->start(&error)) {
        LOWLAT_LOG_ERROR("采集源启动失败: {}", error);
        return 2;
    }
    LOWLAT_LOG_INFO("采集源={} 请求={}x{}@{}fps 实际={}x{}@{}fps", source->name(), opt.width,
                    opt.height, opt.fps, source->width(), source->height(), source->fps());
    if (opt.source == "file") {
        LOWLAT_LOG_INFO("文件={} 循环={}", opt.input, opt.loop ? "on" : "off");
    }
    if (opt.source == "v4l2") {
        LOWLAT_LOG_INFO("V4L2 设备={}", opt.device);
    }
    if (opt.noise_level > 0) {
        source->set_noise_level(opt.noise_level);
        LOWLAT_LOG_INFO("合成源噪声幅度={}", opt.noise_level);
    }

    lowlat::H264Encoder encoder;
    lowlat::H264Encoder::Config encoder_config;
    // 编码参数必须跟采集源的实际输出一致：文件源的分辨率由文件决定，
    // 用命令行传入的默认 1280x720 会导致编码器与帧数据尺寸不匹配。
    const int stream_width = source->width();
    const int stream_height = source->height();
    const int stream_fps = source->fps();
    encoder_config.width = stream_width;
    encoder_config.height = stream_height;
    encoder_config.fps = stream_fps;
    encoder_config.bitrate_kbps = opt.bitrate_kbps;
    encoder_config.keyint_sec = opt.keyint_sec;

    if (!encoder.open(encoder_config, &error)) {
        LOWLAT_LOG_ERROR("编码器打开失败: {}", error);
        source->stop();
        return 2;
    }
    LOWLAT_LOG_INFO("编码器=x264 {} / {} / zerolatency, bitrate={}kbps, keyint={}s",
                    encoder_config.preset, encoder_config.tune, encoder_config.bitrate_kbps,
                    encoder_config.keyint_sec);

    std::unique_ptr<lowlat::StreamSender> sender;
    std::ofstream output;

    if (network_mode) {
        lowlat::StreamSender::Config sender_config;
        sender_config.destination = destination;
        sender_config.max_payload = static_cast<std::size_t>(opt.max_payload);
        sender_config.enable_fec = opt.fec;
        sender_config.fec_group_size = static_cast<std::size_t>(opt.fec_group);
        sender_config.enable_congestion_control = opt.congestion_control;
        sender_config.start_bitrate_kbps = opt.bitrate_kbps;
        sender_config.min_bitrate_kbps = opt.min_bitrate_kbps;
        sender_config.max_bitrate_kbps = opt.max_bitrate_kbps;

        sender = std::make_unique<lowlat::StreamSender>();
        if (!sender->open(sender_config, &error)) {
            LOWLAT_LOG_ERROR("发送器打开失败: {}", error);
            source->stop();
            return 2;
        }
        LOWLAT_LOG_INFO("输出=UDP 目标={} 本地端口={} max_payload={} fec={} group={}",
                        destination.to_string(), sender->local_port(), sender_config.max_payload,
                        opt.fec ? "on" : "off", sender_config.fec_group_size);
    } else {
        const std::filesystem::path output_path(opt.output);
        if (output_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(output_path.parent_path(), ec);
            if (ec) {
                LOWLAT_LOG_ERROR("创建输出目录失败: {}", ec.message());
                source->stop();
                return 2;
            }
        }

        output.open(opt.output, std::ios::binary | std::ios::trunc);
        if (!output) {
            LOWLAT_LOG_ERROR("无法打开输出文件: {}", opt.output);
            source->stop();
            return 2;
        }
        LOWLAT_LOG_INFO("输出=文件 {}", opt.output);
    }

    lowlat::Stats stats;
    std::vector<lowlat::Packet> packets;
    std::uint64_t frames_written = 0;
    std::uint64_t nals_written = 0;
    std::int64_t encode_total_us = 0;
    std::int64_t encode_max_us = 0;

    const auto started_at = std::chrono::steady_clock::now();
    auto last_report = started_at;

    // 把一帧的 NAL 投递到当前输出（文件或 UDP），统一在这里做统计
    auto emit_packets = [&](const std::vector<lowlat::Packet>& nal_packets,
                            std::uint32_t frame_index, std::int64_t capture_ts_us,
                            bool count_frame) -> bool {
        std::uint64_t nals_here = 0;

        if (network_mode) {
            std::vector<lowlat::ByteSpan> nals;
            nals.reserve(nal_packets.size());
            for (const auto& packet : nal_packets) {
                const auto spans =
                    lowlat::split_annexb(packet.data.data(), packet.data.size());
                for (const auto& span : spans) {
                    nals.push_back(span);
                }
            }
            if (nals.empty()) {
                return true;
            }

            // 90kHz 时钟：ts = frame_index * 90000 / fps
            const std::uint64_t scaled = static_cast<std::uint64_t>(frame_index) * 90000ull /
                                         static_cast<std::uint64_t>(stream_fps);
            const std::uint64_t before_bytes = sender->bytes_sent();
            const std::uint64_t before_packets = sender->packets_sent();

            const bool ok =
                sender->send_frame(nals, frame_index, static_cast<std::uint32_t>(scaled),
                                   static_cast<std::uint32_t>(capture_ts_us), &error);
            stats.add_bytes(sender->bytes_sent() - before_bytes);
            stats.add_packets(sender->packets_sent() - before_packets);
            nals_here = nals.size();

            if (!ok) {
                // 不致命：发送缓冲区满时按包丢弃，由上层码率控制处理
                LOWLAT_LOG_EVERY_MS(1000, warn, "发送未完全成功: {}", error);
            }
        } else {
            for (const auto& packet : nal_packets) {
                output.write(reinterpret_cast<const char*>(packet.data.data()),
                             static_cast<std::streamsize>(packet.data.size()));
                if (!output) {
                    LOWLAT_LOG_ERROR("写输出文件失败: {}", opt.output);
                    return false;
                }
                stats.add_bytes(packet.data.size());
                ++nals_here;
            }
        }

        nals_written += nals_here;
        if (count_frame) {
            stats.add_frames(1);
            ++frames_written;
        }
        return true;
    };

    while (!g_stop.load()) {
        if (opt.frames > 0 && frames_written >= static_cast<std::uint64_t>(opt.frames)) {
            break;
        }
        if (opt.duration_sec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_at);
            if (elapsed.count() >= opt.duration_sec) {
                break;
            }
        }

        if (network_mode) {
            // 把"等下一帧"的空档交给 NACK 轮询：阻塞到帧到期为止。
            // 这样重传延迟从最多一个帧间隔（33ms@30fps）降到亚毫秒级，
            // 否则接收端测到的 RTT 实际是"发送端轮询粒度"，会把自适应缓冲带偏。
            const std::int64_t now_us = steady_now_us();
            const std::int64_t due_us = source->next_frame_due_us();
            int wait_ms = 0;
            if (due_us > now_us) {
                wait_ms = static_cast<int>((due_us - now_us) / 1000);
            }
            std::string nack_error;
            (void)sender->poll_incoming(256, wait_ms, &nack_error);
        }

        if (network_mode && opt.congestion_control) {
            // 把估计器给出的目标码率应用到编码器。
            // 只在变化超过 5% 时重配：x264 每次重配都有开销，
            // 逐帧微调反而会让码率曲线抖动。
            const int target = sender->target_bitrate_kbps();
            const int current = encoder.bitrate_kbps();
            if (current > 0 && std::abs(target - current) * 100 >= current * 5) {
                std::string cc_error;
                if (encoder.set_bitrate(target, &cc_error)) {
                    LOWLAT_LOG_EVERY_MS(1000, info, "码率调整 {} → {} kbps", current, target);
                } else {
                    LOWLAT_LOG_EVERY_MS(1000, warn, "码率调整失败: {}", cc_error);
                }
            }
        }

        lowlat::YuvFrame frame;
        if (!source->next_frame(&frame, &error)) {
            LOWLAT_LOG_ERROR("取帧失败: {}", error);
            source->stop();
            return 2;
        }

        const auto encode_begin = std::chrono::steady_clock::now();
        packets.clear();
        if (!encoder.encode(frame, &packets, &error)) {
            LOWLAT_LOG_ERROR("编码失败: {}", error);
            source->stop();
            return 2;
        }
        const auto encode_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - encode_begin)
                                   .count();
        encode_total_us += encode_us;
        encode_max_us = std::max(encode_max_us, encode_us);

        if (!emit_packets(packets, frame.frame_index, frame.capture_ts_us, true)) {
            source->stop();
            return 2;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            last_report = now;
            const auto snapshot = stats.snapshot_and_reset();
            LOWLAT_LOG_INFO("{}", snapshot.to_string());
        }
    }

    source->stop();

    packets.clear();
    if (!encoder.flush(&packets, &error)) {
        LOWLAT_LOG_ERROR("编码器冲刷失败: {}", error);
        return 2;
    }
    // flush 出来的残留包不算新的一帧
    if (!emit_packets(packets, static_cast<std::uint32_t>(frames_written), 0, false)) {
        return 2;
    }
    if (!network_mode) {
        output.flush();
        output.close();
    }

    const auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    const auto final_snapshot = stats.snapshot_and_reset();
    const double avg_encode_ms =
        frames_written == 0
            ? 0.0
            : static_cast<double>(encode_total_us) / static_cast<double>(frames_written) / 1000.0;

    LOWLAT_LOG_INFO("结束: frames={} nals={} bytes={} elapsed={}ms",
                    frames_written, nals_written, final_snapshot.bytes, total_elapsed.count());
    LOWLAT_LOG_INFO("编码耗时: avg={:.3f}ms max={:.3f}ms", avg_encode_ms,
                    static_cast<double>(encode_max_us) / 1000.0);

    if (network_mode) {
        LOWLAT_LOG_INFO("发送统计: packets={} bytes={} would_block={} dropped={} next_seq={}",
                        sender->packets_sent(), sender->bytes_sent(),
                        sender->would_block_events(), sender->packets_dropped(),
                        sender->next_sequence());
        LOWLAT_LOG_INFO("FEC 统计: 修复包={} 组失败={}", sender->fec_packets_sent(),
                        sender->fec_group_failures());
        LOWLAT_LOG_INFO("重传统计: 收到NACK={} 重传包={} 无法满足={}",
                        sender->nack_received(), sender->retransmit_packets_sent(),
                        sender->nack_unserviceable());
        if (opt.congestion_control) {
            const char* state = "normal";
            switch (sender->bandwidth_state()) {
                case lowlat::BandwidthEstimator::State::Overuse:
                    state = "overuse";
                    break;
                case lowlat::BandwidthEstimator::State::Underuse:
                    state = "underuse";
                    break;
                default:
                    break;
            }
            LOWLAT_LOG_INFO("拥塞控制: 反馈包={} 目标码率={}kbps 起始={}kbps 末态={} 末窗延迟={}us",
                            sender->feedback_received(), sender->target_bitrate_kbps(),
                            opt.bitrate_kbps, state, sender->last_window_delay_us());
        }
        LOWLAT_LOG_INFO("目标: {}", destination.to_string());
    } else {
        LOWLAT_LOG_INFO("输出文件: {}", opt.output);
    }

    lowlat::Logger::instance().shutdown();

    if (g_stop.load()) {
        return 3;
    }
    return frames_written > 0 ? 0 : 2;
}
