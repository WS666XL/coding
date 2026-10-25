#include "ui/stats_panel.hpp"

#include <algorithm>

#include <QFont>
#include <QPalette>
#include <QVBoxLayout>

namespace lowlat {
namespace {

// 健康阈值。取自实测：5% 丢包 + FEC + NACK 时 e2e p99 约 118ms、丢帧约 3%。
// 因此 150ms / 5% 视为可接受，超过标黄，250ms / 15% 标红。
constexpr double kWarnLatencyMs = 150.0;
constexpr double kBadLatencyMs = 250.0;
constexpr double kWarnDropRate = 0.05;
constexpr double kBadDropRate = 0.15;

// 面板标签一律用英文：本机实测 fontconfig 里没有任何中文字体
// （fc-list :lang=zh 计数为 0），中文会渲染成方框。
// 用英文还有一个好处：换到别人的机器上不会因为缺字体而变成豆腐块。
QString format_ms(double value)
{
    return QStringLiteral("%1 ms").arg(value, 0, 'f', 1);
}

}  // namespace

StatsPanel::StatsPanel(QWidget* parent) : QWidget(parent)
{
    // 用调色板而不是样式表设背景：样式表里给自定义 QWidget 子类设背景
    // 需要 WA_StyledBackground，且抓图时经常失效；调色板更可靠。
    setAutoFillBackground(true);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor(30, 30, 34));
    palette.setColor(QPalette::WindowText, QColor(216, 216, 220));
    setPalette(palette);

    setMinimumWidth(310);
    setMaximumWidth(350);

    outer_layout_ = new QVBoxLayout(this);
    outer_layout_->setContentsMargins(14, 14, 14, 14);
    outer_layout_->setSpacing(6);

    auto* outer = outer_layout_;

    status_ = new QLabel(QStringLiteral("● IDLE"), this);
    apply_status_font(this->font().pointSizeF() > 0.0 ? this->font().pointSizeF() : 10.0);
    outer->addWidget(status_);

    status_detail_ = new QLabel(QStringLiteral("no data yet"), this);
    status_detail_->setStyleSheet(QStringLiteral("color: #8a8a92;"));
    outer->addWidget(status_detail_);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(3);
    grid->setColumnStretch(1, 1);
    outer->addLayout(grid);

    int row = 0;

    add_section(grid, row++, QStringLiteral("INPUT"));
    source_ = add_row(grid, row++, QStringLiteral("source"));
    resolution_ = add_row(grid, row++, QStringLiteral("resolution"));
    bitrate_ = add_row(grid, row++, QStringLiteral("target bitrate"));

    add_section(grid, row++, QStringLiteral("PLAYBACK"));
    fps_ = add_row(grid, row++, QStringLiteral("fps"));
    shown_ = add_row(grid, row++, QStringLiteral("shown"));
    dropped_ = add_row(grid, row++, QStringLiteral("dropped"));
    decode_errors_ = add_row(grid, row++, QStringLiteral("decode errors"));

    add_section(grid, row++, QStringLiteral("LATENCY"));
    e2e_p50_ = add_row(grid, row++, QStringLiteral("capture→screen p50"));
    e2e_p99_ = add_row(grid, row++, QStringLiteral("capture→screen p99"));
    e2e_max_ = add_row(grid, row++, QStringLiteral("max"));
    queue_avg_ = add_row(grid, row++, QStringLiteral("pickup avg"));
    queue_max_ = add_row(grid, row++, QStringLiteral("pickup max"));

    add_section(grid, row++, QStringLiteral("NETWORK"));
    jitter_ = add_row(grid, row++, QStringLiteral("jitter"));
    rtt_ = add_row(grid, row++, QStringLiteral("rtt"));
    depth_ = add_row(grid, row++, QStringLiteral("buffer depth"));
    packets_ = add_row(grid, row++, QStringLiteral("packets"));
    received_ = add_row(grid, row++, QStringLiteral("received"));

    add_section(grid, row++, QStringLiteral("RECOVERY"));
    fec_ = add_row(grid, row++, QStringLiteral("fec repaired"));
    nack_ = add_row(grid, row++, QStringLiteral("nack sent"));
    retransmit_ = add_row(grid, row++, QStringLiteral("retransmits"));
    late_ = add_row(grid, row++, QStringLiteral("late dropped"));
    duplicate_ = add_row(grid, row++, QStringLiteral("dup dropped"));

    outer->addStretch(1);
}

void StatsPanel::add_section(QGridLayout* grid, int row, const QString& title)
{
    auto* label = new QLabel(title, this);
    label->setStyleSheet(QStringLiteral("color: #7fb4ff; padding-top: 8px;"));
    sections_.push_back(label);
    grid->addWidget(label, row, 0, 1, 2);
}

QLabel* StatsPanel::add_row(QGridLayout* grid, int row, const QString& caption)
{
    auto* name = new QLabel(caption, this);
    name->setStyleSheet(QStringLiteral("color: #9a9aa2;"));

    auto* value = new QLabel(QStringLiteral("--"), this);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    captions_.push_back(name);
    values_.push_back(value);

    grid->addWidget(name, row, 0);
    grid->addWidget(value, row, 1);
    return value;
}

void StatsPanel::update_metrics(const UiMetrics& metrics)
{
    fps_->setText(QStringLiteral("%1 fps").arg(metrics.fps, 0, 'f', 1));
    shown_->setText(QString::number(metrics.shown));
    dropped_->setText(QString::number(metrics.dropped));
    decode_errors_->setText(QString::number(metrics.decode_errors));

    e2e_p50_->setText(format_ms(metrics.e2e_p50_ms));
    e2e_p99_->setText(format_ms(metrics.e2e_p99_ms));
    e2e_max_->setText(format_ms(metrics.e2e_max_ms));
    queue_avg_->setText(format_ms(metrics.queue_avg_ms));
    queue_max_->setText(format_ms(metrics.queue_max_ms));

    jitter_->setText(QStringLiteral("%1 ms").arg(metrics.jitter_ms, 0, 'f', 2));
    rtt_->setText(QStringLiteral("%1 ms").arg(metrics.rtt_ms, 0, 'f', 2));
    depth_->setText(QStringLiteral("%1 ms").arg(metrics.depth_ms));
    packets_->setText(QString::number(metrics.packets));
    // 显示累计接收量而不是"瞬时码率"：后者需要额外的时间窗，容易和日志口径不一致
    received_->setText(QStringLiteral("%1 KB").arg(metrics.bytes / 1024));

    // 长名字（URL、长文件名）必须省略，否则会撑破标签列、
    // 甚至画到左边的标题区上。
    if (metrics.source_label.empty()) {
        source_->setText(QStringLiteral("--"));
    } else {
        const QString full = QString::fromStdString(metrics.source_label);
        const int available = std::max(80, static_cast<int>(width() * 0.55));
        source_->setText(source_->fontMetrics().elidedText(full, Qt::ElideMiddle, available));
        source_->setToolTip(full);
    }
    resolution_->setText(metrics.video_width > 0
                             ? QStringLiteral("%1x%2").arg(metrics.video_width).arg(
                                   metrics.video_height)
                             : QStringLiteral("--"));
    bitrate_->setText(metrics.target_bitrate_kbps > 0
                          ? QStringLiteral("%1 kbps").arg(metrics.target_bitrate_kbps)
                          : QStringLiteral("--"));

    fec_->setText(QString::number(metrics.fec_recovered));
    nack_->setText(QString::number(metrics.nack_sent));
    retransmit_->setText(QString::number(metrics.retransmits));
    late_->setText(QString::number(metrics.late_dropped));
    duplicate_->setText(QString::number(metrics.duplicate_dropped));

    const double drop_rate =
        metrics.decoded == 0
            ? 0.0
            : static_cast<double>(metrics.dropped) / static_cast<double>(metrics.decoded);

    QString color = QStringLiteral("#4caf50");
    QString text = QStringLiteral("● OK");
    if (metrics.e2e_p99_ms >= kBadLatencyMs || drop_rate >= kBadDropRate) {
        color = QStringLiteral("#e53935");
        text = QStringLiteral("● BAD");
    } else if (metrics.e2e_p99_ms >= kWarnLatencyMs || drop_rate >= kWarnDropRate) {
        color = QStringLiteral("#fbc02d");
        text = QStringLiteral("● HIGH");
    }
    if (metrics.received == 0) {
        color = QStringLiteral("#8a8a92");
        text = QStringLiteral("● IDLE");
    }

    status_->setText(text);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    status_detail_->setText(QStringLiteral("p99 %1 ms  ·  drop %2%")
                                .arg(metrics.e2e_p99_ms, 0, 'f', 0)
                                .arg(drop_rate * 100.0, 0, 'f', 1));
}

void StatsPanel::apply_status_font(double point)
{
    if (status_ == nullptr) {
        return;
    }
    QFont font = this->font();
    font.setPointSizeF(point * 1.25);
    font.setBold(true);
    status_->setFont(font);
}

void StatsPanel::set_ui_scale(double scale, double base_point)
{
    if (scale < 1.0) {
        scale = 1.0;
    }
    if (scale > 2.2) {
        scale = 2.2;
    }
    ui_scale_ = scale;

    setMinimumWidth(static_cast<int>(310 * scale));
    setMaximumWidth(static_cast<int>(360 * scale));

    if (outer_layout_ != nullptr) {
        const int margin = static_cast<int>(14 * scale);
        outer_layout_->setContentsMargins(margin, margin, margin, margin);
        outer_layout_->setSpacing(static_cast<int>(6 * scale));
    }

    // 两列必须一起重设。之前只给数值列设了 monospace 字体（创建时的字号），
    // 标题列则依赖继承——缩放后一列变大、一列停在原处，看起来"左边那列特别小"。
    const double point = base_point * scale;
    if (point <= 0.0) {
        return;
    }

    QFont caption_font = this->font();
    caption_font.setPointSizeF(point);
    for (QLabel* label : captions_) {
        if (label != nullptr) {
            label->setFont(caption_font);
        }
    }

    QFont value_font = this->font();
    value_font.setFamily(QStringLiteral("monospace"));
    value_font.setPointSizeF(point);
    for (QLabel* label : values_) {
        if (label != nullptr) {
            label->setFont(value_font);
        }
    }

    QFont section_font = this->font();
    section_font.setPointSizeF(point * 0.92);
    section_font.setBold(true);
    for (QLabel* label : sections_) {
        if (label != nullptr) {
            label->setFont(section_font);
        }
    }

    apply_status_font(point);
    if (status_detail_ != nullptr) {
        QFont detail_font = this->font();
        detail_font.setPointSizeF(point * 0.95);
        status_detail_->setFont(detail_font);
    }
}
}  // namespace lowlat
