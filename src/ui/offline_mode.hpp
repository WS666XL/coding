#pragma once

#include "ui/app_options.hpp"

namespace lowlat {

// 离线校验：解码本地 Annex-B 文件，渲染指定帧并可选截图。
// 用途是把「YUV → RGB」这一步单独锁死，不受网络波动干扰。
int run_offline_mode(const AppOptions& options);

}  // namespace lowlat
