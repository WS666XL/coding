#include "ui/live_mode.hpp"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <QToolBar>
#include <QWidget>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "transport/udp_socket.hpp"
#include "ui/local_sender.hpp"
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

// 打开文件对话框的过滤器。FFmpeg 能解的格式远不止这些，
// 但列太全会让对话框难用，猜不到的格式还有"所有文件"兜底。
QString video_filter()
{
    return QStringLiteral(
        "Video files (*.mp4 *.mkv *.avi *.mov *.flv *.webm *.ts *.m4v *.h264 *.h265 *.y4m);;"
        "All files (*)");
}

}  // namespace

int run_live_mode(const AppOptions& options)
{
    std::string error;

    // 三种进入方式：
    //   --bind  只接收远端推流
    //   --local 直接播放指定文件
    //   都没有  空窗口，等用户点"打开文件"或拖文件进来
    const bool external_mode = !options.bind.empty();

    UdpEndpoint bind_endpoint;
    if (external_mode) {
        if (!parse_endpoint(options.bind, &bind_endpoint, &error)) {
            std::fprintf(stderr, "监听地址错误: %s\n", error.c_str());
            return 1;
        }
    } else {
        // 自己发给自己：绑定临时端口，发送端再把目标指向这个端口，
        // 不用写死端口号，也不会和系统里其他进程冲突
        bind_endpoint.address = "127.0.0.1";
        bind_endpoint.port = 0;
    }

    LogConfig log_config;
    log_config.level = LogLevel::Info;
    Logger::instance().init(log_config);

    StreamReceiver::Options receiver_options;
    receiver_options.bind_address = bind_endpoint.address;
    receiver_options.bind_port = bind_endpoint.port;
    // 单机模式内部自己发，NACK/反馈总是开着才有意义
    receiver_options.enable_nack = external_mode ? options.nack : true;
    receiver_options.enable_feedback = external_mode ? options.feedback : true;
    receiver_options.adaptive_buffer = options.adaptive;
    receiver_options.frame_hold_ms = options.hold_ms;

    ReceiverThread worker;
    if (!worker.start(receiver_options, &error)) {
        std::fprintf(stderr, "启动接收线程失败: %s\n", error.c_str());
        return 2;
    }

    // ---------------------------------------------------------------- 界面
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

    auto* toolbar = window.addToolBar(QStringLiteral("Controls"));
    toolbar->setMovable(false);

    auto* open_action = toolbar->addAction(QStringLiteral("Open file..."));
    auto* pause_action = toolbar->addAction(QStringLiteral("Pause"));
    auto* restart_action = toolbar->addAction(QStringLiteral("Restart"));
    pause_action->setEnabled(false);
    restart_action->setEnabled(false);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    auto* info_label = new QLabel(QStringLiteral("No file loaded — click \"Open file...\" or drag a video here"),
                                  toolbar);
    info_label->setStyleSheet(QStringLiteral("color:#9a9aa2; padding-right:8px;"));
    toolbar->addWidget(info_label);

    // ---------------------------------------------------------------- 发送端
    std::unique_ptr<LocalSender> local_sender;
    std::string current_file;

    auto start_file = [&](const std::string& path) -> bool {
        if (local_sender) {
            local_sender->stop();
            local_sender.reset();
        }

        LocalSender::Options sender_options;
        sender_options.input = path;
        sender_options.destination_address = "127.0.0.1";
        sender_options.destination_port = worker.local_port();
        sender_options.bitrate_kbps = options.bitrate_kbps;
        sender_options.loop = true;
        sender_options.fec = true;
        sender_options.congestion_control = true;

        auto candidate = std::make_unique<LocalSender>();
        std::string start_error;
        if (!candidate->start(sender_options, &start_error)) {
            qCritical().noquote() << "[ui] 打开失败:"
                                  << QString::fromStdString(start_error);
            info_label->setText(QStringLiteral("Open failed: %1")
                                    .arg(QString::fromStdString(start_error)));
            return false;
        }

        local_sender = std::move(candidate);
        current_file = path;
        pause_action->setEnabled(true);
        pause_action->setText(QStringLiteral("Pause"));
        restart_action->setEnabled(true);

        const QString name = QFileInfo(QString::fromStdString(path)).fileName();
        info_label->setText(QStringLiteral("%1  ·  %2x%3  ·  %4 kbps")
                                .arg(name)
                                .arg(local_sender->frame_width())
                                .arg(local_sender->frame_height())
                                .arg(options.bitrate_kbps));
        window.setWindowTitle(QStringLiteral("lowlat video — %1").arg(name));
        qInfo().noquote() << "[ui] 播放" << QString::fromStdString(path)
                          << "尺寸=" << local_sender->frame_width() << "x"
                          << local_sender->frame_height();
        return true;
    };

    QObject::connect(open_action, &QAction::triggered, video, [&]() {
        const QString path = QFileDialog::getOpenFileName(
            &window, QStringLiteral("Choose a video file"), QString(),
            video_filter());
        if (path.isEmpty()) {
            return;
        }
        start_file(path.toStdString());
    });

    QObject::connect(pause_action, &QAction::triggered, video, [&]() {
        if (!local_sender) {
            return;
        }
        const bool next = !local_sender->paused();
        local_sender->set_paused(next);
        pause_action->setText(next ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    });

    QObject::connect(restart_action, &QAction::triggered, video, [&]() {
        if (current_file.empty()) {
            return;
        }
        start_file(current_file);
    });

    // 拖拽文件进来直接播放
    video->set_file_drop_handler([&](const std::string& path) { start_file(path); });

    window.show();

    // ---------------------------------------------------------------- 统计
    Stats e2e_stats;
    std::uint64_t frames_shown = 0;
    std::int64_t queue_total_us = 0;
    std::int64_t queue_max_us = 0;

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
        const double fps =
            seconds > 0.0 ? static_cast<double>(frames_shown - last_shown) / seconds : 0.0;
        last_shown = frames_shown;
        last_report = now;

        const ReceiverStats rs = worker.stats();
        const StatsSnapshot e2e = e2e_stats.peek();

        UiMetrics metrics;
        metrics.fps = fps;
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

        if (local_sender && local_sender->running()) {
            metrics.source_label = QFileInfo(QString::fromStdString(local_sender->input_path()))
                                       .fileName()
                                       .toStdString();
            metrics.video_width = local_sender->frame_width();
            metrics.video_height = local_sender->frame_height();
            metrics.target_bitrate_kbps = local_sender->current_bitrate_kbps();
            metrics.frames_sent = local_sender->frames_sent();
        } else if (external_mode) {
            metrics.source_label = "udp";
        }

        panel->update_metrics(metrics);

        qInfo().noquote()
            << QStringLiteral(
                   "[live] fps=%1 shown=%2 recv=%3 dec=%4 drop=%5 err=%6 | "
                   "e2e p50=%7 p99=%8 max=%9 ms | jitter=%10ms rtt=%11ms depth=%12ms | "
                   "fec=%13 nack=%14 retx=%15 | kbps=%16")
                   .arg(metrics.fps, 0, 'f', 1)
                   .arg(metrics.shown)
                   .arg(metrics.received)
                   .arg(metrics.decoded)
                   .arg(metrics.dropped)
                   .arg(metrics.decode_errors)
                   .arg(metrics.e2e_p50_ms, 0, 'f', 1)
                   .arg(metrics.e2e_p99_ms, 0, 'f', 1)
                   .arg(metrics.e2e_max_ms, 0, 'f', 1)
                   .arg(metrics.jitter_ms, 0, 'f', 2)
                   .arg(metrics.rtt_ms, 0, 'f', 2)
                   .arg(metrics.depth_ms)
                   .arg(metrics.fec_recovered)
                   .arg(metrics.nack_sent)
                   .arg(metrics.retransmits)
                   .arg(metrics.target_bitrate_kbps);
    });
    stats_timer->start();

    // 命令行指定了文件就直接播
    if (!options.local.empty()) {
        start_file(options.local);
    }

    // ---------------------------------------------------------------- 截图
    if (!options.dump.empty()) {
        QTimer::singleShot(options.wait_ms, &window, [&]() {
            const QImage video_image = video->grabFramebuffer();
            const QImage panel_image = panel->grab().toImage();
            // QWidget::grab 抓不到 QOpenGLWidget 的内容，所以整窗图里视频区是黑的，
            // 但工具条与面板能正常渲染——用来验证"控件确实存在且可点"。
            const QImage window_image = window.grab().toImage();

            const QString video_path =
                QString::fromStdString(options.dump) + QStringLiteral("_video.png");
            const QString panel_path =
                QString::fromStdString(options.dump) + QStringLiteral("_panel.png");
            const QString window_path =
                QString::fromStdString(options.dump) + QStringLiteral("_window.png");

            const bool ok_video = !video_image.isNull() && video_image.save(video_path);
            const bool ok_panel = !panel_image.isNull() && panel_image.save(panel_path);
            const bool ok_window = !window_image.isNull() && window_image.save(window_path);
            qInfo().noquote() << "[ui] snapshot video=" << ok_video << video_path
                              << "panel=" << ok_panel << panel_path
                              << "window=" << ok_window << window_path;
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
    if (local_sender) {
        local_sender->stop();
    }
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
