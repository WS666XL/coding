// UI 进程入口：只做参数解析与模式分发，具体实现在 offline_mode / live_mode。

#include <QApplication>
#include <QDebug>

#include <cstdio>
#include <string>

#include "ui/app_options.hpp"
#include "ui/live_mode.hpp"
#include "ui/offline_mode.hpp"

int main(int argc, char** argv)
{
    lowlat::AppOptions options;
    std::string error;
    if (!lowlat::parse_ui_options(argc, argv, &options, &error)) {
        std::fprintf(stderr, "参数错误: %s\n", error.c_str());
        lowlat::print_ui_usage();
        return 1;
    }

    QApplication app(argc, argv);
    qInfo().noquote() << "[ui] platform=" << QGuiApplication::platformName();

    return options.input.empty() ? lowlat::run_live_mode(options)
                                 : lowlat::run_offline_mode(options);
}
