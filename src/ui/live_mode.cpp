#include "ui/live_mode.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QResizeEvent>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "common/log.hpp"
#include "common/stats.hpp"
#include "transport/udp_socket.hpp"
#include "ui/local_sender.hpp"
#include "ui/metrics_chart.hpp"
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

// 来源标签：URL 只保留 scheme://host:port，普通路径取文件名。
// 不截短的话，udp:// 的查询串会把工具条和面板整行撑满。
std::string short_source_label(const std::string& path)
{
    const std::size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
        // 端口后面可能直接跟查询串（udp://0.0.0.0:5100?fifo_size=...），
        // 所以要在 '/' 和 '?' 里取更靠前的那个作为截断点。
        std::size_t end = path.find('/', scheme + 3);
        const std::size_t query = path.find('?', scheme + 3);
        if (query != std::string::npos && (end == std::string::npos || query < end)) {
            end = query;
        }
        return end == std::string::npos ? path : path.substr(0, end);
    }
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 主窗口：把尺寸变化转成缩放回调。
// 不使用信号/槽，所以不需要 moc。
class ScaledWindow final : public QMainWindow {
public:
    std::function<void(int)> on_resize;

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QMainWindow::resizeEvent(event);
        if (on_resize) {
            on_resize(height());
        }
    }
};

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

    // 模拟丢包档位（千分比）。换文件后要重新应用到新发送端，所以放在外面。
    int current_loss_permille = std::clamp(options.simulated_loss_percent, 0, 100) * 10;

    // ---------------------------------------------------------------- 界面
    ScaledWindow window;
    auto* central = new QWidget(&window);
    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* top_row = new QHBoxLayout();
    top_row->setContentsMargins(0, 0, 0, 0);
    top_row->setSpacing(0);

    auto* video = new VideoWidget(central);
    auto* panel = new StatsPanel(central);
    top_row->addWidget(video, 1);
    top_row->addWidget(panel, 0);

    // 底部实时曲线：把"看不见的过程"画出来
    auto* chart = new MetricsChart(central);
    root_layout->addLayout(top_row, 1);
    root_layout->addWidget(chart, 0);

    window.setCentralWidget(central);



    auto* toolbar = window.addToolBar(QStringLiteral("Controls"));
    toolbar->setMovable(false);

    auto* open_action = toolbar->addAction(QStringLiteral("Open file..."));
    auto* pause_action = toolbar->addAction(QStringLiteral("Pause"));
    auto* restart_action = toolbar->addAction(QStringLiteral("Restart"));
    pause_action->setEnabled(false);
    restart_action->setEnabled(false);

    // 丢包档位：在发送端按比例丢包，不需要 root，也不影响系统网络
    toolbar->addSeparator();
    auto* loss_combo = new QComboBox(toolbar);
    loss_combo->addItem(QStringLiteral("Loss: off"), 0);
    loss_combo->addItem(QStringLiteral("Loss: 2%"), 20);
    loss_combo->addItem(QStringLiteral("Loss: 5%"), 50);
    loss_combo->addItem(QStringLiteral("Loss: 10%"), 100);
    loss_combo->addItem(QStringLiteral("Loss: 20%"), 200);
    loss_combo->addItem(QStringLiteral("Loss: 30%"), 300);
    loss_combo->setToolTip(
        QStringLiteral("Simulated packet loss on the sender (no root required)"));
    if (external_mode) {
        loss_combo->setEnabled(false);  // 只接收远端时没有本地发送端可丢
    }
    // 命令行预置的档位：找到匹配项就选中（找不到说明是自定义值，保持 off 显示）
    for (int i = 0; i < loss_combo->count(); ++i) {
        if (loss_combo->itemData(i).toInt() == current_loss_permille) {
            loss_combo->setCurrentIndex(i);
            break;
        }
    }
    toolbar->addWidget(loss_combo);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    auto* info_label = new QLabel(QStringLiteral("No file loaded — click \"Open file...\" or drag a video here"),
                                  toolbar);
    info_label->setStyleSheet(QStringLiteral("color:#9a9aa2; padding-right:8px;"));
    toolbar->addWidget(info_label);

    // ---------------------------------------------------------------- 缩放
    // 字号必须随窗口尺寸走。
    //
    // 之前所有字号都取 Qt 默认值（约 10pt），窗口最大化到 2K 屏后
    // 面板与工具条的文字相对画面显得极小——"放大屏幕反而更看不清"。
    // 这里按窗口高度对 760 的比值整体缩放，并同步面板宽度与曲线高度。
    const QFont base_font = QApplication::font();
    double applied_scale = 0.0;

    auto apply_ui_scale = [&](int window_height) {
        double scale = static_cast<double>(window_height) / 760.0;
        scale = std::clamp(scale, 1.0, 2.0);
        // 只有明显变化才重设字体：QApplication::setFont 会触发全局重排，
        // 拖动窗口时每次都调用会让界面发涩。
        if (std::abs(scale - applied_scale) < 0.04) {
            return;
        }
        applied_scale = scale;

        QFont font = base_font;
        font.setPointSizeF(base_font.pointSizeF() * scale);
        QApplication::setFont(font);

        const double base_point = base_font.pointSizeF() > 0.0 ? base_font.pointSizeF() : 10.0;

        // 工具条的子控件不会可靠地跟随 QApplication::setFont。
        // addAction 时 QToolBar 自己创建的 QToolButton、手动 addWidget 的下拉框，
        // 字体在创建那一刻就定了；只调 setFont 的话要等下一次布局才刷新——
        // 表现就是"启动时工具条字很小，拖动一下窗口才正常"。
        // 这里逐个显式设置，并强制重新布局。
        toolbar->setFont(font);
        for (QWidget* child : toolbar->findChildren<QWidget*>()) {
            child->setFont(font);
        }
        // 工具条原来是默认浅色，和深色面板拼在一起很割裂。
        // 一旦给 QToolBar 设样式表，Qt 就会改用样式表里的配色，
        // 所以背景、文字、控件外观都要一起写清楚。
        const int spacing = static_cast<int>(6 * scale);
        const int padding = static_cast<int>(4 * scale);
        toolbar->setStyleSheet(
            QStringLiteral(
                "QToolBar { background: #1e1e22; border: none; spacing: %1px; padding: 2px; }"
                "QToolButton { color: #d8d8dc; padding: %2px %3px; }"
                "QToolButton:hover { background: #2f2f36; border-radius: 4px; }"
                "QToolButton:pressed { background: #3a3a44; }"
                "QComboBox { color: #d8d8dc; background: #2a2a30;"
                "            border: 1px solid #3a3a44; border-radius: 4px;"
                "            padding: %2px %3px; }"
                "QComboBox QAbstractItemView { background: #2a2a30; color: #d8d8dc;"
                "                             selection-background-color: #3a3a44; }"
                "QLabel { color: #9a9aa2; }")
                .arg(spacing)
                .arg(padding)
                .arg(padding * 2));
        toolbar->updateGeometry();

        panel->set_ui_scale(scale, base_point);
        chart->set_ui_scale(scale, base_point);
    };

    window.on_resize = [&](int height) { apply_ui_scale(height); };
    window.resize(options.window_width, options.window_height);
    apply_ui_scale(options.window_height);

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
        local_sender->set_simulated_loss_permille(current_loss_permille);
        current_file = path;
        pause_action->setEnabled(true);
        pause_action->setText(QStringLiteral("Pause"));
        restart_action->setEnabled(true);

        const QString name = QString::fromStdString(short_source_label(path));
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

    // 记住上次打开的位置：WSL 下大家常用 /mnt/c/... 里的 Windows 文件，
    // 每次都从用户主目录点进去太麻烦。
    QString last_directory;
    QObject::connect(open_action, &QAction::triggered, video, [&]() {
        const QString start_dir =
            last_directory.isEmpty() ? QDir::homePath() : last_directory;
        const QString path = QFileDialog::getOpenFileName(
            &window, QStringLiteral("Choose a video file"), start_dir, video_filter());
        if (path.isEmpty()) {
            return;
        }
        last_directory = QFileInfo(path).absolutePath();
        start_file(path.toStdString());
    });

    // 记住正常状态下的工具条文案，暂停时临时替换再还原
    QString normal_info_text;

    QObject::connect(pause_action, &QAction::triggered, video, [&]() {
        if (!local_sender) {
            return;
        }
        const bool next = !local_sender->paused();
        local_sender->set_paused(next);
        pause_action->setText(next ? QStringLiteral("Resume") : QStringLiteral("Pause"));
        // 暂停时光靠按钮文字容易被忽略，右上角状态也一起改，
        // 免得用户以为程序卡死了
        if (next) {
            if (normal_info_text.isEmpty()) {
                normal_info_text = info_label->text();
            }
            info_label->setText(QStringLiteral("[PAUSED] click Resume to continue"));
            info_label->setStyleSheet(QStringLiteral("color:#fbc02d; font-weight:bold; "
                                                     "padding-right:8px;"));
        } else {
            info_label->setText(normal_info_text);
            info_label->setStyleSheet(QStringLiteral("color:#9a9aa2; padding-right:8px;"));
            normal_info_text.clear();
        }
    });

    QObject::connect(restart_action, &QAction::triggered, video, [&]() {
        if (current_file.empty()) {
            return;
        }
        start_file(current_file);
    });

    QObject::connect(loss_combo, &QComboBox::currentIndexChanged, video, [&](int index) {
        current_loss_permille = loss_combo->itemData(index).toInt();
        if (local_sender) {
            local_sender->set_simulated_loss_permille(current_loss_permille);
        }
        qInfo().noquote() << "[ui] 模拟丢包 =" << current_loss_permille / 10.0 << "%";
    });

    // 拖拽文件进来直接播放
    video->set_file_drop_handler([&](const std::string& path) { start_file(path); });

    window.show();

    // show() 之后控件才有真实尺寸。用实际高度再应用一次，
    // 否则启动时用的是命令行传入的尺寸，可能和布局最终算出的大小差一截。
    QTimer::singleShot(0, &window, [&]() { apply_ui_scale(window.height()); });

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

    // 曲线采样：500ms 一个点。比面板的 1s 快，切换丢包档位时形变更明显。
    auto* chart_timer = new QTimer(&window);
    chart_timer->setInterval(500);
    QObject::connect(chart_timer, &QTimer::timeout, video, [&]() {
        if (frames_shown == 0) {
            return;
        }
        const StatsSnapshot snapshot = e2e_stats.peek();
        chart->push_sample(snapshot.latency_p99_ms,
                           local_sender ? local_sender->current_bitrate_kbps() : 0);
    });
    chart_timer->start();

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
            const std::string label = short_source_label(local_sender->input_path());
            metrics.source_label = label;
            metrics.video_width = local_sender->frame_width();
            metrics.video_height = local_sender->frame_height();
            metrics.target_bitrate_kbps = local_sender->current_bitrate_kbps();
            metrics.frames_sent = local_sender->frames_sent();
        } else if (external_mode) {
            // 只接收远端时没有本地发送端，分辨率只能从解码后的纹理尺寸取。
            // 不填的话面板显示 "--"，看起来像没在收流。
            metrics.source_label = "udp (remote)";
            metrics.video_width = video->texture_width();
            metrics.video_height = video->texture_height();
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
    // 单张与连拍互斥：单张会在 wait_ms 后直接退出。
    if (!options.dump.empty() && options.dump_sequence_count <= 0) {
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

    // 连拍：导出序列供生成演示动图。
    // 视频区与面板分开存，因为 QWidget::grab() 抓不到 QOpenGLWidget 的内容。
    QTimer* sequence_timer = nullptr;
    if (!options.dump.empty() && options.dump_sequence_count > 0) {
        const QString directory =
            QString::fromStdString(options.dump) + QStringLiteral("_seq");
        QDir().mkpath(directory);
        const QByteArray dir_bytes = directory.toLocal8Bit();

        sequence_timer = new QTimer(&window);
        sequence_timer->setInterval(options.dump_interval_ms);

        auto captured = std::make_shared<int>(0);
        QObject::connect(sequence_timer, &QTimer::timeout, video, [&, captured, dir_bytes]() {
            if (*captured >= options.dump_sequence_count) {
                sequence_timer->stop();
                qInfo().noquote() << "[ui] 序列导出完成" << *captured << "帧";
                QApplication::quit();
                return;
            }
            const QImage video_image = video->grabFramebuffer();
            const QImage panel_image = panel->grab().toImage();
            if (video_image.isNull() || panel_image.isNull()) {
                return;
            }
            // 用 JPEG 而不是 PNG：PNG 编码 1280x580 要约 15ms，直接把 UI 线程
            // 拖到 12fps，面板会显示 BAD——录制手段干扰了被录对象。
            // JPEG(q85) 约 3ms，实测不再影响帧率。
            video_image.save(QString::asprintf("%s/%04d_video.jpg", dir_bytes.constData(),
                                               *captured),
                             "JPG", 85);
            panel_image.save(QString::asprintf("%s/%04d_panel.jpg", dir_bytes.constData(),
                                               *captured),
                             "JPG", 90);
            ++(*captured);
        });
        sequence_timer->start();
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
    chart_timer->stop();
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
