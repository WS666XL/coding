// 接收端入口（headless）：收包走 StreamReceiver，本文件只负责落盘与统计输出。
//
// 真正的接收流水线（FEC / NACK / 抖动缓冲 / 重排）都在 transport/stream_receiver，
// Qt 客户端会复用同一份实现，避免"命令行能播、界面不能播"的分叉。
//
// 退出码约定：
//   0 正常结束
//   1 参数错误
//   2 运行时错误
//   3 被信号中断（SIGINT / SIGTERM）

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codec/decoder.hpp"
#include "common/log.hpp"
#include "transport/stream_receiver.hpp"
#include "transport/udp_socket.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int /*signum*/)
{
    g_stop.store(true);
}

struct Options {
    std::string bind{"0.0.0.0:5000"};
    std::string output{"recv.h264"};
    int duration_sec{0};
    int frames{0};
    int rcvbuf_mb{8};
    bool nack{false};
    int nack_delay_ms{8};
    bool feedback{false};
    int frame_hold_ms{60};
    bool adaptive{false};
    double jitter_k{3.0};
    bool decode{false};
    int dump_frames{0};
    std::string dump_dir{"out/frames"};
    std::string log_level{"info"};
};

void print_usage()
{
    std::fprintf(stderr,
                 "用法: lowlat_receiver [选项]\n"
                 "  --bind <addr:port>   监听地址，默认 0.0.0.0:5000\n"
                 "  --output <path>      输出文件，默认 recv.h264\n"
                 "  --duration <sec>     运行时长上限，0 表示不限\n"
                 "  --frames <int>       收满多少帧后退出，0 表示不限\n"
                 "  --rcvbuf <MB>        接收缓冲区大小，默认 8\n"
                 "  --nack               检测到丢包时向发送端请求重传\n"
                 "  --nack-delay <ms>    NACK 重排序等待窗口，默认 8\n"
                 "  --feedback           回传时序样本与丢包率（配合发送端 --cc）\n"
                 "  --hold <ms>          帧保持窗口，默认 60\n"
                 "  --adaptive           按 RFC 3550 抖动估计自适应调整保持窗口\n"
                 "  --jitter-k <float>   深度 = k × 抖动估计，默认 3.0\n"
                 "  --decode             对重组出的 NAL 做 H.264 解码（验证链路）\n"
                 "  --dump-frames <int>  落盘前 N 帧解码结果为紧凑 YUV\n"
                 "  --dump-dir <path>    解码帧输出目录，默认 out/frames\n"
                 "  --log-level <level>  trace|debug|info|warn|error\n"
                 "  --help               显示本帮助\n");
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
                *error = std::string(name) + " 取值不是整数";
                return false;
            }
            return true;
        };

        if (key == "--help" || key == "-h") {
            print_usage();
            std::exit(0);
        } else if (key == "--bind") {
            if (!take_value("--bind", &opt->bind)) return false;
        } else if (key == "--output") {
            if (!take_value("--output", &opt->output)) return false;
        } else if (key == "--duration") {
            if (!take_int("--duration", &opt->duration_sec)) return false;
        } else if (key == "--frames") {
            if (!take_int("--frames", &opt->frames)) return false;
        } else if (key == "--rcvbuf") {
            if (!take_int("--rcvbuf", &opt->rcvbuf_mb)) return false;
        } else if (key == "--nack") {
            opt->nack = true;
        } else if (key == "--feedback") {
            opt->feedback = true;
        } else if (key == "--nack-delay") {
            if (!take_int("--nack-delay", &opt->nack_delay_ms)) return false;
        } else if (key == "--hold") {
            if (!take_int("--hold", &opt->frame_hold_ms)) return false;
        } else if (key == "--adaptive") {
            opt->adaptive = true;
        } else if (key == "--jitter-k") {
            if (i + 1 >= argc) {
                *error = "--jitter-k 缺少取值";
                return false;
            }
            char* end = nullptr;
            const double value = std::strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || value <= 0.0) {
                *error = "--jitter-k 取值非法";
                return false;
            }
            opt->jitter_k = value;
        } else if (key == "--decode") {
            opt->decode = true;
        } else if (key == "--dump-frames") {
            if (!take_int("--dump-frames", &opt->dump_frames)) return false;
        } else if (key == "--dump-dir") {
            if (!take_value("--dump-dir", &opt->dump_dir)) return false;
        } else if (key == "--log-level") {
            if (!take_value("--log-level", &opt->log_level)) return false;
        } else {
            *error = "未知参数: " + key;
            return false;
        }
    }

    if (opt->duration_sec < 0 || opt->frames < 0 || opt->dump_frames < 0) {
        *error = "时长与帧数不能为负";
        return false;
    }
    if (opt->rcvbuf_mb <= 0 || opt->rcvbuf_mb > 1024) {
        *error = "rcvbuf 取值应在 1..1024 MB";
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

bool install_signal_handlers()
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGINT, &action, nullptr) == 0 &&
           sigaction(SIGTERM, &action, nullptr) == 0;
}

const std::uint8_t kStartCode[4] = {0, 0, 0, 1};

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

    lowlat::UdpEndpoint bind_endpoint;
    if (!lowlat::parse_endpoint(opt.bind, &bind_endpoint, &error)) {
        std::fprintf(stderr, "监听地址错误: %s\n", error.c_str());
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

    lowlat::StreamReceiver::Options receiver_options;
    receiver_options.bind_address = bind_endpoint.address;
    receiver_options.bind_port = bind_endpoint.port;
    receiver_options.rcvbuf_mb = opt.rcvbuf_mb;
    receiver_options.enable_nack = opt.nack;
    receiver_options.nack_delay_ms = opt.nack_delay_ms;
    receiver_options.enable_feedback = opt.feedback;
    receiver_options.adaptive_buffer = opt.adaptive;
    receiver_options.frame_hold_ms = opt.frame_hold_ms;
    receiver_options.jitter_k = opt.jitter_k;

    lowlat::StreamReceiver receiver;
    if (!receiver.open(receiver_options, &error)) {
        LOWLAT_LOG_ERROR("监听失败: {}", error);
        return 2;
    }

    lowlat::H264Decoder decoder;
    bool decoder_ready = false;
    std::uint64_t decoded_frame_count = 0;
    std::uint64_t decode_errors = 0;
    std::int64_t decode_total_us = 0;
    std::int64_t decode_max_us = 0;
    int dump_remaining = opt.dump_frames;
    int dump_index = 0;

    if (opt.decode) {
        if (!decoder.open(&error)) {
            LOWLAT_LOG_ERROR("解码器打开失败: {}", error);
            return 2;
        }
        decoder_ready = true;
        if (opt.dump_frames > 0) {
            std::error_code ec;
            std::filesystem::create_directories(opt.dump_dir, ec);
        }
    }

    const std::filesystem::path output_path(opt.output);
    if (output_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(output_path.parent_path(), ec);
    }
    std::ofstream output(opt.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        LOWLAT_LOG_ERROR("无法打开输出文件: {}", opt.output);
        return 2;
    }

    LOWLAT_LOG_INFO("监听={} local_port={} 输出={} rcvbuf={}MB nack={} 解码={}",
                    bind_endpoint.to_string(), receiver.local_port(), opt.output, opt.rcvbuf_mb,
                    opt.nack ? "on" : "off", opt.decode ? "on" : "off");

    auto dump_decoded_frame = [&](const lowlat::H264Decoder::Frame& frame) {
        if (dump_remaining <= 0) {
            return;
        }
        --dump_remaining;

        char name[256];
        std::snprintf(name, sizeof(name), "%s/dec_%04d.yuv", opt.dump_dir.c_str(), dump_index);
        ++dump_index;

        std::ofstream dump(name, std::ios::binary | std::ios::trunc);
        if (!dump) {
            LOWLAT_LOG_WARN("无法写入解码帧: {}", name);
            return;
        }
        // 紧凑 YUV420P，便于与 ffmpeg 的 rawvideo 输出逐字节比对
        for (int y = 0; y < frame.height; ++y) {
            dump.write(reinterpret_cast<const char*>(
                           frame.y + static_cast<std::ptrdiff_t>(y) * frame.stride_y),
                       frame.width);
        }
        const int chroma_width = frame.width / 2;
        const int chroma_height = frame.height / 2;
        for (int y = 0; y < chroma_height; ++y) {
            dump.write(reinterpret_cast<const char*>(
                           frame.u + static_cast<std::ptrdiff_t>(y) * frame.stride_u),
                       chroma_width);
        }
        for (int y = 0; y < chroma_height; ++y) {
            dump.write(reinterpret_cast<const char*>(
                           frame.v + static_cast<std::ptrdiff_t>(y) * frame.stride_v),
                       chroma_width);
        }
    };

    std::vector<lowlat::ReceivedFrame> frames;
    std::uint64_t nals_written = 0;
    bool write_failed = false;

    auto consume = [&](lowlat::ReceivedFrame& frame) {
        for (const auto& nal : frame.nals) {
            output.write(reinterpret_cast<const char*>(kStartCode), sizeof(kStartCode));
            output.write(reinterpret_cast<const char*>(nal.data()),
                         static_cast<std::streamsize>(nal.size()));
            if (!output) {
                write_failed = true;
                return;
            }
            ++nals_written;
        }

        if (!decoder_ready || write_failed) {
            return;
        }

        const auto decode_begin = std::chrono::steady_clock::now();
        std::vector<lowlat::H264Decoder::Frame> decoded;
        for (const auto& nal : frame.nals) {
            if (!decoder.feed_nal(lowlat::ByteSpan(nal.data(), nal.size()), &decoded, &error)) {
                ++decode_errors;
                break;
            }
        }
        const auto decode_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - decode_begin)
                                   .count();
        decode_total_us += decode_us;
        if (decode_us > decode_max_us) {
            decode_max_us = decode_us;
        }
        for (const auto& decoded_frame : decoded) {
            ++decoded_frame_count;
            dump_decoded_frame(decoded_frame);
        }
    };

    const auto started_at = std::chrono::steady_clock::now();
    auto last_report = started_at;

    while (!g_stop.load()) {
        if (opt.frames > 0 &&
            receiver.received_frames() >= static_cast<std::uint64_t>(opt.frames)) {
            break;
        }
        if (opt.duration_sec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_at);
            if (elapsed.count() >= opt.duration_sec) {
                break;
            }
        }

        frames.clear();
        if (!receiver.poll(20, &frames, &error)) {
            LOWLAT_LOG_ERROR("接收失败: {}", error);
            return 2;
        }
        for (auto& frame : frames) {
            consume(frame);
        }
        if (write_failed) {
            LOWLAT_LOG_ERROR("写输出文件失败: {}", opt.output);
            return 2;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(1)) {
            last_report = now;
            const lowlat::ReceiverStats live = receiver.snapshot();
            LOWLAT_LOG_INFO("packets={} bytes={} frames={} nals={} gaps={} seq_breaks={} bad={} "
                            "jitter={:.1f}ms depth={}ms",
                            live.packets, live.bytes, live.distinct_frames, nals_written,
                            live.frame_gaps, live.sequence_breaks, live.malformed,
                            live.jitter_us / 1000.0, live.depth_ms);
        }
    }

    frames.clear();
    receiver.flush(&frames);
    for (auto& frame : frames) {
        consume(frame);
    }

    output.flush();
    output.close();

    if (decoder_ready) {
        // 解码器内部通常还压着一帧，不冲就会少一帧
        std::vector<lowlat::H264Decoder::Frame> tail;
        std::string flush_error;
        if (!decoder.flush(&tail, &flush_error)) {
            LOWLAT_LOG_WARN("解码器冲刷失败: {}", flush_error);
        }
        for (const auto& decoded_frame : tail) {
            ++decoded_frame_count;
            dump_decoded_frame(decoded_frame);
        }
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    const lowlat::ReceiverStats final_stats = receiver.snapshot();

    LOWLAT_LOG_INFO("结束: packets={} bytes={} frames={} nals={} elapsed={}ms",
                    final_stats.packets, final_stats.bytes, final_stats.distinct_frames,
                    nals_written, elapsed_ms);
    LOWLAT_LOG_INFO("完整性: frame_gaps={} sequence_breaks={} malformed={} fu_a_dropped={}",
                    final_stats.frame_gaps, final_stats.sequence_breaks, final_stats.malformed, 0);
    LOWLAT_LOG_INFO("FEC: 恢复包={} 由FEC产出的NAL={}", final_stats.fec_recovered_packets,
                    final_stats.nals_from_fec);
    LOWLAT_LOG_INFO("抖动: 估计={:.2f}ms 缓冲深度={}..{}ms (末值 {}ms) 模式={} k={:.1f}",
                    final_stats.jitter_us / 1000.0, final_stats.depth_min_ms,
                    final_stats.depth_max_ms, final_stats.depth_ms,
                    opt.adaptive ? "自适应" : "固定", opt.jitter_k);
    LOWLAT_LOG_INFO("RTT: srtt={:.2f}ms min={:.2f}ms 样本={} NACK等待窗口={}ms",
                    final_stats.srtt_us / 1000.0, final_stats.min_rtt_us / 1000.0,
                    final_stats.rtt_samples, opt.nack_delay_ms);
    if (decoder_ready) {
        const double avg_decode_ms =
            decoded_frame_count == 0
                ? 0.0
                : static_cast<double>(decode_total_us) /
                      static_cast<double>(decoded_frame_count) / 1000.0;
        LOWLAT_LOG_INFO("解码: 帧数={} 送包错误={} 耗时 avg={:.2f}ms max={:.2f}ms 已落盘样本={}",
                        decoded_frame_count, decode_errors, avg_decode_ms,
                        static_cast<double>(decode_max_us) / 1000.0, dump_index);
    }
    LOWLAT_LOG_INFO("端到端延迟(采集→落盘): 样本={} avg={:.2f}ms p50={:.2f}ms p99={:.2f}ms "
                    "max={:.2f}ms",
                    final_stats.latency_samples, final_stats.latency_avg_ms,
                    final_stats.latency_p50_ms, final_stats.latency_p99_ms,
                    final_stats.latency_max_ms);
    LOWLAT_LOG_INFO("NACK: 请求次数={} 请求序列号={} 收到重传={} 晚到丢弃={} 重复丢弃={}",
                    final_stats.nack_sent, final_stats.nack_sequences_requested,
                    final_stats.retransmits_received, final_stats.nals_dropped_late,
                    final_stats.duplicate_nals_dropped);
    LOWLAT_LOG_INFO("输出文件: {}", opt.output);

    lowlat::Logger::instance().shutdown();

    if (g_stop.load()) {
        return 3;
    }
    if (final_stats.packets == 0) {
        return 2;
    }
    return (final_stats.frame_gaps == 0 && final_stats.malformed == 0) ? 0 : 2;
}
