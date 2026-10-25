#pragma once

#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/ui_metrics.hpp"

namespace lowlat {

// 右侧指标面板。
//
// 设计取向是"一眼能看出好坏"，而不是把所有数字堆上去：
//   - 顶部一个状态灯，按延迟与丢帧率变色
//   - 分组显示，每组标题加粗
//   - 数值右对齐、等宽字体，方便纵向对比
class StatsPanel : public QWidget {
public:
    explicit StatsPanel(QWidget* parent = nullptr);

    void update_metrics(const UiMetrics& metrics);

    // 随窗口缩放：字号由应用字体承载，这里负责宽度、间距与状态灯字号。
    // 放大窗口时若不缩放，右侧文字会显得越来越小。
    void set_ui_scale(double scale);

private:
    void add_section(QGridLayout* grid, int row, const QString& title);
    QLabel* add_row(QGridLayout* grid, int row, const QString& caption);
    void apply_status_font();

    double ui_scale_{1.0};
    QVBoxLayout* outer_layout_{nullptr};

    QLabel* status_{nullptr};
    QLabel* status_detail_{nullptr};

    QLabel* source_{nullptr};
    QLabel* resolution_{nullptr};
    QLabel* bitrate_{nullptr};

    QLabel* fps_{nullptr};
    QLabel* shown_{nullptr};
    QLabel* dropped_{nullptr};
    QLabel* decode_errors_{nullptr};

    QLabel* e2e_p50_{nullptr};
    QLabel* e2e_p99_{nullptr};
    QLabel* e2e_max_{nullptr};
    QLabel* queue_avg_{nullptr};
    QLabel* queue_max_{nullptr};

    QLabel* jitter_{nullptr};
    QLabel* rtt_{nullptr};
    QLabel* depth_{nullptr};
    QLabel* packets_{nullptr};
    QLabel* received_{nullptr};

    QLabel* fec_{nullptr};
    QLabel* nack_{nullptr};
    QLabel* retransmit_{nullptr};
    QLabel* late_{nullptr};
    QLabel* duplicate_{nullptr};
};

}  // namespace lowlat
