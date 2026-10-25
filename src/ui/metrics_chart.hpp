#pragma once

#include <QWidget>

#include <cstddef>
#include <deque>

namespace lowlat {

// 实时曲线：延迟与码率随时间的变化。
//
// 为什么需要它：面板上的数字是静止的，正常回环下一切都"看起来正常"，
// 分不清这个程序和普通播放器有什么区别。把延迟和码率画成曲线后，
// 切换丢包档位、拥塞控制收敛这些动态过程会直接变成曲线的形变——
// 这是"程序在做事"最直观的证据。
class MetricsChart : public QWidget {
public:
    explicit MetricsChart(QWidget* parent = nullptr);

    // 每个采样点一条：latency_ms 为 p99 端到端延迟，bitrate_kbps 为当前目标码率
    void push_sample(double latency_ms, int bitrate_kbps);

    void clear();

    // 随窗口缩放：高度、内边距与刻度字号
    void set_ui_scale(double scale);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double ui_scale_{1.0};

    std::deque<double> latency_;
    std::deque<double> bitrate_;
};

}  // namespace lowlat
