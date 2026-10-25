#pragma once

#include "ui/app_options.hpp"

namespace lowlat {

// 实时播放：收包 → 解码 → 上屏，右侧带指标面板。
int run_live_mode(const AppOptions& options);

}  // namespace lowlat
