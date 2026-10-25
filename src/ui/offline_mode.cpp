#include "ui/offline_mode.hpp"

#include <QApplication>
#include <QDebug>
#include <QImage>
#include <QMainWindow>
#include <QTimer>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "codec/decoder.hpp"
#include "protocol/h264_fragmenter.hpp"
#include "ui/video_widget.hpp"

namespace lowlat {
namespace {

bool read_whole_file(const std::string& path, std::vector<std::uint8_t>* data, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "无法打开文件: " + path;
        return false;
    }
    data->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (data->empty()) {
        *error = "文件为空: " + path;
        return false;
    }
    return true;
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

int run_offline_mode(const AppOptions& options)
{
    std::string error;
    std::vector<std::uint8_t> file_data;
    if (!read_whole_file(options.input, &file_data, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    H264Decoder decoder;
    if (!decoder.open(&error)) {
        std::fprintf(stderr, "解码器打开失败: %s\n", error.c_str());
        return 2;
    }

    std::vector<H264Decoder::Frame> frames;
    const auto nals = split_annexb(file_data.data(), file_data.size());
    for (const auto& nal : nals) {
        if (!decoder.feed_nal(nal, &frames, &error)) {
            std::fprintf(stderr, "解码失败: %s\n", error.c_str());
            return 2;
        }
    }
    if (!decoder.flush(&frames, &error)) {
        std::fprintf(stderr, "解码器冲刷失败: %s\n", error.c_str());
        return 2;
    }

    qInfo().noquote() << "[ui] decoded_frames=" << static_cast<int>(frames.size());
    if (frames.empty()) {
        std::fprintf(stderr, "没有解出任何帧\n");
        return 2;
    }
    if (options.frame >= static_cast<int>(frames.size())) {
        std::fprintf(stderr, "--frame %d 超出范围（共 %zu 帧）\n", options.frame, frames.size());
        return 1;
    }

    const auto& source = frames[static_cast<std::size_t>(options.frame)];
    const YuvPlanes planes = to_planes(source);
    if (!planes.valid()) {
        std::fprintf(stderr, "平面数据非法\n");
        return 2;
    }

    qInfo().noquote() << "[ui] frame=" << options.frame << "size=" << planes.width << "x"
                      << planes.height << "stride_y=" << planes.stride_y;

    QMainWindow window;
    auto* video = new VideoWidget(&window);
    window.setCentralWidget(video);
    // 离线模式锁死控件尺寸，保证截帧可以和参考图逐像素比对
    video->setFixedSize(planes.width, planes.height);
    window.adjustSize();
    window.setWindowTitle(QStringLiteral("lowlat video (offline)"));
    window.show();

    video->set_frame(planes, std::make_shared<int>(0));

    int exit_code = 0;
    QTimer::singleShot(options.wait_ms, &window, [&]() {
        QImage image = video->grabFramebuffer();
        if (image.isNull()) {
            qCritical().noquote() << "[ui] grabFramebuffer 返回空图像";
            exit_code = 3;
            QApplication::quit();
            return;
        }
        qInfo().noquote() << "[ui] framebuffer=" << image.size()
                          << "dpr=" << image.devicePixelRatio();
        if (!options.dump.empty()) {
            if (!image.save(QString::fromStdString(options.dump))) {
                qCritical().noquote() << "[ui] 保存失败:" << options.dump.c_str();
                exit_code = 3;
            } else {
                qInfo().noquote() << "[ui] saved=" << options.dump.c_str();
            }
        }
        QApplication::quit();
    });

    return QApplication::exec() == 0 ? exit_code : 3;
}

}  // namespace lowlat
