// Qt 渲染可行性探针。
//
// 目的：在写真正的 UI 之前，用最小代价确认三件事
//   1. Qt 能否在当前的窗口平台上起窗口（WSLg / X11 / wayland / offscreen）
//   2. QOpenGLWidget 能否拿到 GL 上下文（拿不到就得退回软件渲染）
//   3. 渲染结果能否被截取下来做自动化校验
//
// 加 --bench 会额外做一次吞吐测量：用事件循环驱动重绘 2 秒，统计真实 fps。
// 注意不能用 QWidget::repaint() 循环来测——实测它只触发 1 次绘制，得到的时间
// 是"提交命令"而非"渲染完成"的时间，会高估两个数量级。
//
// 退出码：
//   0 全部通过
//   4 GL 上下文未初始化
//   5 截帧失败或像素颜色不符预期

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QMainWindow>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QTimer>

#include <chrono>
#include <cstdlib>
#include <string>

namespace {

// 清屏色，取 0..255 内可精确表示的值，便于像素校验
constexpr int kClearR = 38;
constexpr int kClearG = 140;
constexpr int kClearB = 89;

class ProbeWidget : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit ProbeWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent) {}

    bool gl_ready() const noexcept { return gl_ready_; }
    QString gl_info() const { return gl_info_; }
    int paint_count() const noexcept { return paint_count_; }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();

        const auto text = [this](GLenum name) -> QString {
            const auto* value = reinterpret_cast<const char*>(glGetString(name));
            return value != nullptr ? QString::fromLatin1(value) : QStringLiteral("<null>");
        };

        gl_info_ = QStringLiteral("vendor=%1 | renderer=%2 | version=%3")
                       .arg(text(GL_VENDOR), text(GL_RENDERER), text(GL_VERSION));
        gl_ready_ = true;
        qInfo().noquote() << "[probe] GL" << gl_info_;
    }

    void paintGL() override
    {
        ++paint_count_;
        glClearColor(kClearR / 255.0f, kClearG / 255.0f, kClearB / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

private:
    bool gl_ready_{false};
    int paint_count_{0};
    QString gl_info_;
};

// 截一帧、存盘、并校验中心像素
int capture_and_verify(ProbeWidget* widget)
{
    if (!widget->gl_ready()) {
        qCritical().noquote() << "[probe] GL 上下文未初始化";
        return 4;
    }

    const QImage frame = widget->grabFramebuffer();
    if (frame.isNull()) {
        qCritical() << "[probe] grabFramebuffer 返回空图像";
        return 5;
    }

    const QString path = QStringLiteral("/tmp/qt_probe.png");
    const bool saved = frame.save(path);
    qInfo().noquote() << "[probe] framebuffer=" << frame.size() << "saved=" << saved
                      << "path=" << path;

    const QColor center = frame.pixelColor(frame.width() / 2, frame.height() / 2);
    qInfo().noquote() << "[probe] center pixel rgb=(" << center.red() << "," << center.green()
                      << "," << center.blue() << ") expect=(" << kClearR << "," << kClearG << ","
                      << kClearB << ")";

    constexpr int kTolerance = 2;
    const bool match = std::abs(center.red() - kClearR) <= kTolerance &&
                       std::abs(center.green() - kClearG) <= kTolerance &&
                       std::abs(center.blue() - kClearB) <= kTolerance;
    if (!match) {
        qCritical().noquote() << "[probe] 中心像素与清屏色不符，GL 绘制可能没有生效";
        return 5;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool bench = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bench") {
            bench = true;
        }
    }

    // 基准模式下关掉垂直同步：否则测出来的只是刷新率上限（约 60Hz），
    // 拿不到渲染器的真实吞吐。生产代码不应该这么做。
    if (bench) {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }

    QApplication app(argc, argv);
    qInfo().noquote() << "[probe] platform=" << QGuiApplication::platformName()
                      << "swap_interval=" << QSurfaceFormat::defaultFormat().swapInterval();

    QMainWindow window;
    auto* widget = new ProbeWidget(&window);
    window.setCentralWidget(widget);
    window.resize(1280, 720);
    window.setWindowTitle(QStringLiteral("lowlat qt probe"));
    window.show();

    int exit_code = 0;

    if (bench) {
        // 用事件循环驱动重绘，测的是"提交 + 合成"的完整吞吐
        auto* driver = new QTimer(&window);
        driver->setInterval(0);
        QObject::connect(driver, &QTimer::timeout, widget, [widget]() { widget->update(); });

        const int before = widget->paint_count();
        const auto begin = std::chrono::steady_clock::now();
        driver->start();

        QTimer::singleShot(2000, &window, [&]() {
            driver->stop();
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - begin)
                                        .count();
            const int painted = widget->paint_count() - before;
            const double seconds = static_cast<double>(elapsed_us) / 1e6;

            qInfo().noquote() << "[probe] bench window=" << widget->size()
                              << "painted=" << painted << "elapsed_ms="
                              << QString::number(seconds * 1000.0, 'f', 1);
            if (seconds > 0.0 && painted > 0) {
                const double fps = static_cast<double>(painted) / seconds;
                const double per_frame_ms = 1000.0 / fps;
                qInfo().noquote() << "[probe] bench fps=" << QString::number(fps, 'f', 1)
                                  << "per_frame_ms=" << QString::number(per_frame_ms, 'f', 3)
                                  << "30fps_budget_used="
                                  << QString::number(per_frame_ms / 33.33 * 100.0, 'f', 1) << "%";
            } else {
                qWarning().noquote() << "[probe] 2 秒内没有产生任何绘制，基准无效";
            }

            exit_code = capture_and_verify(widget);
            app.quit();
        });
    } else {
        QTimer::singleShot(1200, &window, [&]() {
            exit_code = capture_and_verify(widget);
            app.quit();
        });
    }

    const int app_result = app.exec();
    return exit_code != 0 ? exit_code : app_result;
}
