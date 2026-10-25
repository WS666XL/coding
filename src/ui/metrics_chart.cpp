#include "ui/metrics_chart.hpp"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <algorithm>
#include <utility>

namespace lowlat {
namespace {

constexpr int kPadding = 8;
constexpr int kLabelWidth = 54;
constexpr int kRowGap = 10;
// 容量：500ms 一个点，共 2 分钟
constexpr std::size_t kCapacity = 240;

// 画一格曲线。values 已归一化到 [0,1]。
void draw_series(QPainter& painter, const QRectF& area, const std::deque<double>& values,
                 const QColor& color, double max_value, const QString& title,
                 const QString& current)
{
    painter.setPen(QColor(0x8a, 0x8a, 0x92));
    QFont label_font = painter.font();
    label_font.setPointSizeF(std::max(7.0, label_font.pointSizeF() - 2.0));
    painter.setFont(label_font);
    painter.drawText(QRectF(area.left(), area.top() - 14, area.width() / 2, 14),
                     Qt::AlignLeft | Qt::AlignVCenter, title);
    painter.drawText(QRectF(area.left() + area.width() / 2, area.top() - 14, area.width() / 2, 14),
                     Qt::AlignRight | Qt::AlignVCenter, current);

    // 底纹与网格
    painter.fillRect(area, QColor(0x24, 0x24, 0x2a));
    painter.setPen(QColor(0x33, 0x33, 0x3a));
    painter.drawRect(area);

    if (values.size() < 2 || max_value <= 0.0) {
        return;
    }

    // 折线：从左侧开始向右生长，样本攒满后整条曲线向左滚动。
    // （之前把 x 偏移算成"靠右对齐"，样本少的时候整条线被挤在最右侧一小块。）
    const double step = area.width() / static_cast<double>(kCapacity - 1);
    const double x_offset = 0.0;

    QPainterPath path;
    bool first = true;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double normalized = std::clamp(values[i] / max_value, 0.0, 1.0);
        const double x = area.left() + x_offset + step * static_cast<double>(i);
        const double y = area.bottom() - normalized * (area.height() - 2.0) - 1.0;
        if (first) {
            path.moveTo(x, y);
            first = false;
        } else {
            path.lineTo(x, y);
        }
    }

    QPen pen(color);
    pen.setWidthF(1.6);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

}  // namespace

MetricsChart::MetricsChart(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(150);
    setMaximumHeight(170);
    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor(30, 30, 34));
    setPalette(palette);
}

void MetricsChart::push_sample(double latency_ms, int bitrate_kbps)
{
    latency_.push_back(latency_ms);
    bitrate_.push_back(static_cast<double>(bitrate_kbps));
    while (latency_.size() > kCapacity) {
        latency_.pop_front();
    }
    while (bitrate_.size() > kCapacity) {
        bitrate_.pop_front();
    }
    update();
}

void MetricsChart::clear()
{
    latency_.clear();
    bitrate_.clear();
    update();
}

void MetricsChart::paintEvent(QPaintEvent* event)
{
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(30, 30, 34));

    const QRectF content = QRectF(rect()).adjusted(kPadding, kPadding + 14, -kPadding, -kPadding);
    const double row_height = (content.height() - kRowGap) / 2.0;

    const QRectF latency_area(content.left() + kLabelWidth, content.top(),
                              content.width() - kLabelWidth, row_height);
    const QRectF bitrate_area(content.left() + kLabelWidth, content.top() + row_height + kRowGap,
                              content.width() - kLabelWidth, row_height);

    // 纵轴自适应：取历史最大值并留 20% 余量，避免曲线贴顶
    double latency_max = 1.0;
    for (double value : latency_) {
        latency_max = std::max(latency_max, value);
    }
    latency_max *= 1.2;

    double bitrate_max = 1.0;
    for (double value : bitrate_) {
        bitrate_max = std::max(bitrate_max, value);
    }
    bitrate_max *= 1.2;

    draw_series(painter, latency_area, latency_, QColor(0x7f, 0xb4, 0xff),
                latency_max, QStringLiteral("LATENCY p99 (ms)"),
                latency_.empty() ? QStringLiteral("--")
                                 : QStringLiteral("%1 ms").arg(latency_.back(), 0, 'f', 0));

    draw_series(painter, bitrate_area, bitrate_, QColor(0x66, 0xbb, 0x6a),
                bitrate_max, QStringLiteral("TARGET BITRATE (kbps)"),
                bitrate_.empty() ? QStringLiteral("--")
                                 : QStringLiteral("%1 kbps").arg(bitrate_.back(), 0, 'f', 0));

    // 左侧纵轴刻度（只标最大值，够用）
    painter.setPen(QColor(0x8a, 0x8a, 0x92));
    QFont axis_font = painter.font();
    axis_font.setPointSizeF(std::max(7.0, axis_font.pointSizeF() - 2.0));
    painter.setFont(axis_font);
    painter.drawText(QRectF(content.left(), latency_area.top(), kLabelWidth - 6, 12),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(static_cast<int>(latency_max)));
    painter.drawText(QRectF(content.left(), bitrate_area.top(), kLabelWidth - 6, 12),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(static_cast<int>(bitrate_max)));
}

}  // namespace lowlat
