#!/usr/bin/env bash
#
# 用 Windows 摄像头作为输入，跑通完整链路。
#
#   ./scripts/camera.sh              # 打开窗口看摄像头
#   ./scripts/camera.sh --loss 20    # 顺便模拟 20% 丢包
#
# 为什么需要这个脚本：
#   摄像头由 Windows 侧的 ffmpeg 采集（WSL2 拿不到 /dev/video0），
#   它和我们的界面是两个进程。手工启动的话，关掉窗口后 ffmpeg 还在跑，
#   摄像头指示灯会一直亮着——因为设备仍被它占用。
#   这个脚本用 trap 保证退出时把采集进程一起收掉。
#
# 可选环境变量：
#   CAMERA_NAME  摄像头设备名，默认 "Integrated Camera"
#   SIZE         采集分辨率，默认 640x480
#   FPS          采集帧率，默认 30
#   BITRATE      Windows 侧编码码率，默认 1500k
#   PORT         UDP 端口，默认 5100

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI="$ROOT/build/src/ui/lowlat_ui"
FFMPEG=${FFMPEG:-/mnt/d/tools/ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe}

CAMERA_NAME=${CAMERA_NAME:-Integrated Camera}
SIZE=${SIZE:-640x480}
FPS=${FPS:-30}
BITRATE=${BITRATE:-1500k}
PORT=${PORT:-5100}

if [ ! -x "$FFMPEG" ]; then
    echo "错误: 找不到 Windows 版 ffmpeg（$FFMPEG）" >&2
    echo "      可以用 FFMPEG=/path/to/ffmpeg.exe 指定" >&2
    exit 1
fi
if [ ! -x "$UI" ]; then
    echo "错误: 找不到 $UI，请先构建" >&2
    exit 1
fi

FFMPEG_LOG=${FFMPEG_LOG:-$ROOT/out/camera_ffmpeg.log}
mkdir -p "$(dirname "$FFMPEG_LOG")"

WSL_IP=$(hostname -I | awk '{print $1}')
if [ -z "$WSL_IP" ]; then
    echo "错误: 取不到 WSL 的 IP" >&2
    exit 1
fi

# ---------------------------------------------------------------- 清理
FFMPEG_PID=""
# Windows 程序在 WSL 里由 /init 代理，$! 拿到的是代理进程，
# kill -0 不能可靠反映真实状态。退出一律按命令行特征来清理。
ffmpeg_pattern() {
    echo "ffmpeg.exe.*udp://${WSL_IP}:${PORT}"
}

cleanup() {
    pkill -f "$(ffmpeg_pattern)" 2>/dev/null
    # 给 Windows 侧一点时间真正释放设备，否则指示灯会残留
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -f "$(ffmpeg_pattern)" > /dev/null 2>&1 || break
        sleep 0.2
    done
    pkill -9 -f "$(ffmpeg_pattern)" 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- 检查设备
# 摄像头如果还被 usbipd 绑着，Windows 是拿不到的
if command -v powershell.exe > /dev/null 2>&1; then
    status=$(timeout 15 powershell.exe -NoProfile -Command \
        "(Get-PnpDevice -Class Camera | Where-Object FriendlyName -eq '$CAMERA_NAME').Status" \
        2>/dev/null | tr -d '\r\n ')
    if [ -n "$status" ] && [ "$status" != "OK" ]; then
        echo "错误: 摄像头状态是 '$status'，Windows 无法使用" >&2
        echo "      如果之前用过 usbipd，先执行: usbipd unbind --busid 1-6" >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------- 启动采集
echo "摄像头: $CAMERA_NAME  ${SIZE}@${FPS}  ->  udp://$WSL_IP:$PORT"

setsid "$FFMPEG" -hide_banner -loglevel error \
    -f dshow -rtbufsize 100M -framerate "$FPS" -video_size "$SIZE" \
    -i "video=$CAMERA_NAME" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v "$BITRATE" -g "$FPS" \
    -f mpegts "udp://$WSL_IP:$PORT?pkt_size=1316" > "$FFMPEG_LOG" 2>&1 &
FFMPEG_PID=$!

# 等流真正可用再开窗口，否则界面会先报"打不开"
# Windows 进程通过 /init 启动，枚举 dshow 设备要 1 秒左右。
# 太早检查存活会把"还没起来"误判成"已失败"。
sleep 2

stream_ready=0
for _ in $(seq 1 24); do
    if timeout 6 ffprobe -v quiet -show_streams \
        "udp://0.0.0.0:$PORT?fifo_size=262144&overrun_nonfatal=1" > /dev/null 2>&1; then
        stream_ready=1
        break
    fi
    sleep 0.5
done
if [ "$stream_ready" -ne 1 ]; then
    echo "错误: 12 秒内没有收到视频流，ffmpeg 输出：" >&2
    tail -5 "$FFMPEG_LOG" >&2
    exit 1
fi

# ---------------------------------------------------------------- 打开界面
echo "关闭窗口即停止采集"
"$UI" --local "udp://0.0.0.0:$PORT?fifo_size=262144&overrun_nonfatal=1" "$@"
