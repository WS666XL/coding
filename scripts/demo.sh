#!/usr/bin/env bash
#
# 演示脚本。
#
#   ./scripts/demo.sh                 合成测试图，双进程（发送端 + 接收端窗口）
#   ./scripts/demo.sh 我的视频.mp4     播放你自己的视频文件，单进程
#
# 单进程模式会把文件解码 -> 重编码 -> 打包 -> 本机 UDP -> 收包 -> 重组 ->
# 解码 -> 渲染 全部跑一遍，中间走的是真实协议栈，指标都是真的。
#
# 可选环境变量：
#   DURATION=300   播放时长，默认 120 秒
#   BITRATE=3000   单进程模式的目标码率
#   LOSS=5         模拟丢包（仅双进程模式生效）
#   NOISE=40       给合成画面加噪声
#
# 按 Ctrl-C 结束。

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI="$ROOT/build/src/ui/lowlat_ui"
SENDER="$ROOT/build/src/app/lowlat_sender"

INPUT=${1:-}
DURATION=${DURATION:-120}
BITRATE=${BITRATE:-2000}
NOISE=${NOISE:-0}
LOSS=${LOSS:-0}
PORT=${PORT:-5950}

if [ ! -x "$UI" ]; then
    echo "错误: 找不到 $UI，请先构建：cmake --build build -j" >&2
    exit 1
fi

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "错误: 没有图形环境（DISPLAY 与 WAYLAND_DISPLAY 都为空）" >&2
    exit 1
fi

pkill -f 'lowlat_(ui|sender)' 2>/dev/null
sleep 0.5

trap 'pkill -f "lowlat_(ui|sender)" 2>/dev/null' EXIT INT TERM

if [ -n "$INPUT" ]; then
    # ---------------- 单进程：一个窗口，播放文件
    if [ ! -f "$INPUT" ]; then
        echo "错误: 文件不存在 $INPUT" >&2
        exit 1
    fi
    echo "=== 播放文件: $INPUT ==="
    echo "  链路: 解码 -> 重编码 -> 打包 -> UDP -> 收包 -> 重组 -> 解码 -> 渲染"
    echo "  时长: ${DURATION}s  码率: ${BITRATE}kbps"
    echo
    "$UI" --local "$INPUT" --bitrate "$BITRATE" --duration "$DURATION"
    exit $?
fi

# ---------------- 双进程：合成源 + 独立接收端
if [ ! -x "$SENDER" ]; then
    echo "错误: 找不到 $SENDER，请先构建" >&2
    exit 1
fi

echo "=== 合成源演示（端口 $PORT）==="
"$UI" --bind "127.0.0.1:$PORT" --nack --feedback --duration "$DURATION" &
ui_pid=$!

sleep 3   # 等接收端绑定端口，UDP 无连接，早发的包会被丢弃

if ! kill -0 "$ui_pid" 2>/dev/null; then
    echo "错误: 接收端启动失败" >&2
    exit 1
fi

echo "=== 发送端推流（$((DURATION)) 秒）==="
frames=$((DURATION * 30))
"$SENDER" --frames "$frames" --dest "127.0.0.1:$PORT" --fec --cc \
    --bitrate "$BITRATE" --noise "$NOISE" &
send_pid=$!

echo
echo "窗口标题: lowlat video (live)"
echo "按 Ctrl-C 结束。"

wait "$ui_pid" 2>/dev/null
wait "$send_pid" 2>/dev/null
