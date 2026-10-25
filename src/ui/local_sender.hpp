#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "capture/capture_source.hpp"
#include "codec/encoder.hpp"
#include "transport/stream_sender.hpp"

namespace lowlat {

// 进程内发送端：文件源 -> x264 -> RTP-lite -> 本机 UDP。
//
// 存在的理由：原来的形态要开两个终端（sender + ui），对使用者来说不叫产品。
// 把发送端放进 UI 进程后，一个窗口就能把整条链路跑起来：
//
//   文件 -> 解码 -> 重编码 -> 打包 -> UDP(127.0.0.1) -> 收包 -> 重组 -> 解码 -> 渲染
//
// 链路绕了一圈回到同一个进程，但中间走的是真实的 UDP 和真实的协议栈，
// 所以指标（延迟、丢包、重传、码率）都是真的，不是模拟出来的。
class LocalSender {
public:
    struct Options {
        std::string input;                        // 媒体文件路径
        std::string destination_address{"127.0.0.1"};
        std::uint16_t destination_port{0};        // 0 表示未设置，start 会报错
        int bitrate_kbps{2000};
        int min_bitrate_kbps{300};
        int max_bitrate_kbps{4000};
        bool loop{true};
        bool fec{true};
        bool congestion_control{true};
    };

    LocalSender();
    ~LocalSender();

    LocalSender(const LocalSender&) = delete;
    LocalSender& operator=(const LocalSender&) = delete;

    bool start(const Options& options, std::string* error);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_relaxed); }
    std::string last_error() const;

    // 暂停只是停止推进帧，不拆连接：恢复后从这里继续。
    void set_paused(bool paused) noexcept;
    bool paused() const noexcept { return paused_.load(std::memory_order_relaxed); }

    const std::string& input_path() const noexcept { return options_.input; }

    std::uint64_t frames_sent() const noexcept;
    int current_bitrate_kbps() const noexcept;
    int frame_width() const noexcept;
    int frame_height() const noexcept;

private:
    void run();

    Options options_;
    std::unique_ptr<CaptureSource> source_;
    std::unique_ptr<H264Encoder> encoder_;
    std::unique_ptr<StreamSender> sender_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<int> current_bitrate_kbps_{0};
    std::atomic<int> frame_width_{0};
    std::atomic<int> frame_height_{0};

    mutable std::mutex error_mutex_;
    std::string error_;
};

}  // namespace lowlat
