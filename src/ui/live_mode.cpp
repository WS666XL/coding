#include "ui/live_mode.hpp"

#include <QApplication>
#include <QDebug>
#include <QHBoxLayout>
#include <QImage>
#include <QMainWindow>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <cstdio>
#include <string>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "transport/udp_socket.hpp"
#include "ui/receiver_thread.hpp"
#include "ui/stats_panel.hpp"
#include "ui/video_widget.hpp"

namespace lowlat {
namespace {

std::int64_t steady_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

YuvPlanes to_planes(const H264Decoder::Frame& frame)
{
    YuvPlanes planes;
    planes.y = frame.y;
    planes.u = frame.u;
    planes.v = frame.v;
    planes.width = frame.width;
    planes.height = frame.height;
    planes.stride_y = frame.stride_y;
    planes.stride_u = frame.stride_u;
    planes.stride_v = frame.stride_v;
    return planes;
}

}  // namespace

int run_live_mode(const AppOptions& options)
{
    std::string error;
    UdpEndpoint bind_endpoint;
    if (!parse_endpoint(options.bind, &bind_endpoint, &error)) {
        std::fprintf(stderr, "监听地址错误: %s\n", error.c_str());
        return 1;
    }

    LogConfig log_config;
    log_config.level = LogLevel::Info;
    Logger::instance().init(log_config);

    StreamReceiver::Options receiver_options;
    receiver_options.bind_address = bind_endpoint.address;
    receiver_options.bind_port = bind_endpoint.port;
    receiver_options.enable_nack = options.nack;
    receiver_options.enable_feedback = options.feedback;
    receiver_options.adaptive_buffer = options.adaptive;
    receiver_options.frame_hold_ms = options.hold_ms;

    ReceiverThread worker;
    if (!worker.start(receiver_options, &error)) {
        std::fprintf(stderr, "启动接收线程失败: %s\n", error.c_str());
        return 2;
    }
    qInfo().noquote() << "[ui] listening on port" << worker.local_port()
                      << "nack=" << (options.nack ? "on" : "off")
                      << "adaptive=" << (options.adaptive ? "on" : "off")
                      << "hold=" << options.hold_ms << "ms";

    QMainWindow window;
    auto* central = new QWidget(&window);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* video = new VideoWidget(central);
    auto* panel = new StatsPanel(central);
    layout->addWidget(video, 1);
    layout->addWidget(panel, 0);

    window.setCentralWidget(central);
    window.resize(1600, 760);
    window.setWindowTitle(QStringLiteral("lowlat video (live)"));
    window.show();

    // ---------------------------------------------------------------- 统计
    Stats e2e_stats;  // 采集 → 上屏
    std::uint64_t frames_shown = 0;
    std::int64_t queue_total_us = 0;
    std::int64_t queue_max_us = 0;
    double current_fps = 0.0;

    const auto started_at = std::chrono::steady_clock::now();
    auto last_report = started_at;
    std::uint64_t last_shown = 0;

    // ---------------------------------------------------------------- 渲染
    auto* render_timer = new QTimer(&window);
    render_timer->setInterval(16);
    QObject::connect(render_timer, &QTimer::timeout, video, [&]() {
        DecodedFrame entry;
        if (!worker.slot().take(&entry)) {
            return;
        }

        const std::int64_t now_us = steady_now_us();
        const std::int64_t queue_us = now_us - entry.ready_ts_us;
        queue_total_us += queue_us;
        if (queue_us > queue_max_us) {
            queue_max_us = queue_us;
        }

        if (entry.capture_ts_us != 0) {
            // 采集时间戳是 uint32 微秒，无符号差值天然处理回绕
            const std::uint32_t now_low32 = static_cast<std::uint32_t>(now_us);
            const std::uint32_t delta = now_low32 - entry.capture_ts_us;
            e2e_stats.record_latency_us(static_cast<std::int64_t>(delta));
        }

        const YuvPlanes planes = to_planes(entry.frame);
        if (!planes.valid()) {
            return;
        }
        video->set_frame(planes, entry.frame.owner);
        ++frames_shown;
    });
    render_timer->start();

    // ---------------------------------------------------------------- 报告
    auto* stats_timer = new QTimer(&window);
    stats_timer->setInterval(1000);
    QObject::connect(stats_timer, &QTimer::timeout, video, [&]() {
        if (worker.failed()) {
            qCritical().noquote() << "[ui] 接收线程失败:" << worker.last_error().c_str();
            QApplication::quit();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last_report).count();
        current_fps = seconds > 0.0 ? static_cast<double>(frames_shown - last_shown) / seconds : 0.0;
        last_shown = frames_shown;
        last_report = now;

        const ReceiverStats rs = worker.stats();
        const StatsSnapshot e2e = e2e_stats.peek();

        UiMetrics metrics;
        metrics.fps = current_fps;
        metrics.shown = frames_shown;
        metrics.received = worker.frames_received();
        metrics.decoded = worker.decoded_frames();
        metrics.dropped = worker.slot().dropped();
        metrics.decode_errors = worker.decode_errors();

        metrics.e2e_p50_ms = e2e.latency_p50_ms;
        metrics.e2e_p99_ms = e2e.latency_p99_ms;
        metrics.e2e_max_ms = e2e.latency_max_ms;
        metrics.queue_avg_ms =
            frames_shown == 0
                ? 0.0
                : static_cast<double>(queue_total_us) / static_cast<double>(frames_shown) / 1000.0;
        metrics.queue_max_ms = static_cast<double>(queue_max_us) / 1000.0;

        metrics.packets = rs.packets;
        metrics.bytes = rs.bytes;
        metrics.jitter_ms = rs.jitter_us / 1000.0;
        metrics.rtt_ms = rs.srtt_us / 1000.0;
        metrics.depth_ms = rs.depth_ms;

        metrics.fec_recovered = rs.fec_recovered_packets;
        metrics.nack_sent = rs.nack_sent;
        metrics.retransmits = rs.retransmits_received;
        metrics.late_dropped = rs.nals_dropped_late;
        metrics.duplicate_dropped = rs.duplicate_nals_dropped;

        panel->update_metrics(metrics);

        // 日志与面板同源，保证两处数字永远一致
        qInfo().noquote()
            << QStringLiteral(
                   "[live] fps=%1 shown=%2 recv=%3 dec=%4 drop=%5 err=%6 | "
                   "e2e p50=%7 p99=%8 max=%9 ms | queue avg=%10 max=%11 ms | "
                   "jitter=%12ms rtt=%13ms depth=%14ms | fec=%15 nack=%16 retx=%17")
                   .arg(metrics.fps, 0, 'f', 1)
                   .arg(metrics.shown)
                   .arg(metrics.received)
                   .arg(metrics.decoded)
                   .arg(metrics.dropped)
                   .arg(metrics.decode_errors)
                   .arg(metrics.e2e_p50_ms, 0, 'f', 1)
                   .arg(metrics.e2e_p99_ms, 0, 'f', 1)
                   .arg(metrics.e2e_max_ms, 0, 'f', 1)
                   .arg(metrics.queue_avg_ms, 0, 'f', 1)
                   .arg(metrics.queue_max_ms, 0, 'f', 1)
                   .arg(metrics.jitter_ms, 0, 'f', 2)
                   .arg(metrics.rtt_ms, 0, 'f', 2)
                   .arg(metrics.depth_ms)
                   .arg(metrics.fec_recovered)
                   .arg(metrics.nack_sent)
                   .arg(metrics.retransmits);
    });
    stats_timer->start();

    // ---------------------------------------------------------------- 截图
    // 用于自动化验证：分别保存视频区与面板区，避免 QWidget::grab 抓不到 GL 内容
    if (!options.dump.empty()) {
        QTimer::singleShot(options.wait_ms, &window, [&]() {
            const QImage video_image = video->grabFramebuffer();
            const QImage panel_image = panel->grab().toImage();

            const QString video_path =
                QString::fromStdString(options.dump) + QStringLiteral("_video.png");
            const QString panel_path =
                QString::fromStdString(options.dump) + QStringLiteral("_panel.png");

            const bool ok_video = !video_image.isNull() && video_image.save(video_path);
            const bool ok_panel = !panel_image.isNull() && panel_image.save(panel_path);
            qInfo().noquote() << "[ui] snapshot video=" << ok_video << video_path
                              << "panel=" << ok_panel << panel_path;
            QApplication::quit();
        });
    }

    if (options.duration_sec > 0) {
        QTimer::singleShot(options.duration_sec * 1000, &window, [&]() {
            qInfo().noquote() << "[ui] 达到时长上限，退出";
            QApplication::quit();
        });
    }

    const int app_result = QApplication::exec();

    render_timer->stop();
    stats_timer->stop();
    worker.stop();

    const StatsSnapshot e2e = e2e_stats.peek();
    qInfo().noquote() << QStringLiteral(
                             "[ui] 结束: shown=%1 recv=%2 dec=%3 err=%4 drop=%5 "
                             "e2e p50=%6 p99=%7 max=%8 ms")
                             .arg(frames_shown)
                             .arg(worker.frames_received())
                             .arg(worker.decoded_frames())
                             .arg(worker.decode_errors())
                             .arg(worker.slot().dropped())
                             .arg(e2e.latency_p50_ms, 0, 'f', 1)
                             .arg(e2e.latency_p99_ms, 0, 'f', 1)
                             .arg(e2e.latency_max_ms, 0, 'f', 1);

    Logger::instance().shutdown();
    return app_result;
}

}  // namespace lowlat
