#!/usr/bin/env bash
#
# 启动 lowlat video 界面。
#
#   ./scripts/demo.sh                打开空窗口，用界面里的"Open file..."或拖拽选视频
#   ./scripts/demo.sh 我的视频.mp4    直接播放这个文件
#
# 单机模式下，一个进程就把整条链路跑起来：
#   文件 -> 解码 -> x264 重编码 -> 打包 -> 本机 UDP -> 收包 -> 重组 -> 解码 -> 渲染
# 中间走的是真实协议栈，面板上的延迟/丢包/码率都不是模拟值。
#
# 可选环境变量：
#   DURATION=300   自动退出时间（秒），默认 0 = 不自动退出
#   BITRATE=3000   目标码率

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI="$ROOT/build/src/ui/lowlat_ui"

INPUT=${1:-}
DURATION=${DURATION:-0}
BITRATE=${BITRATE:-2000}

if [ ! -x "$UI" ]; then
    echo "错误: 找不到 $UI，请先构建：cmake --build build -j" >&2
    exit 1
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "错误: 没有图形环境（DISPLAY 与 WAYLAND_DISPLAY 都为空）" >&2
    exit 1
fi

if [ -n "$INPUT" ] && [ ! -f "$INPUT" ]; then
    echo "错误: 文件不存在 $INPUT" >&2
    exit 1
fi

# 上一个实例还占着端口会让新实例收不到包
pkill -f 'lowlat_ui' 2>/dev/null
sleep 0.5

args=(--bitrate "$BITRATE")
if [ -n "$INPUT" ]; then
    args+=(--local "$INPUT")
fi
if [ "$DURATION" -gt 0 ]; then
    args+=(--duration "$DURATION")
fi

exec "$UI" "${args[@]}"
