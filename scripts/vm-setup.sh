#!/usr/bin/env bash
#
# 在虚拟机（或任何原生 Linux）里一键准备并验证 V4L2 采集。
#
#   ./scripts/vm-setup.sh
#
# 做的事：装依赖 → 构建（关测试与 Qt 界面）→ 检查摄像头 → 采 150 帧 → 校验码流
#
# 为什么关掉测试和 Qt：
#   虚拟机常见配置是 4G 内存，编 198 个测试 + Qt 界面会让峰值内存翻倍，
#   而这次的目的只是验证 V4L2 采集，命令行发送端 + h264_probe 就够。
#   测试已经在开发机上跑过（198/198）。

set -uo pipefail

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# ---------------------------------------------------------------- 环境信息
say "环境"
(grep -E '^(PRETTY_NAME)=' /etc/os-release 2>/dev/null || echo "未知发行版")
echo "CPU 核数: $(nproc)"
free -h | head -2
df -h . | tail -1

# ---------------------------------------------------------------- 依赖
say "安装依赖"
if ! sudo apt update; then
    warn "apt update 失败，检查网络或换镜像源"
fi

# 必需：构建 + 编解码 + 日志 + 摄像头工具
PKGS="build-essential cmake ninja-build pkg-config
      libx264-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
      libspdlog-dev libfmt-dev v4l-utils"

if ! sudo apt install -y $PKGS; then
    warn "部分依赖安装失败。若 libspdlog-dev / libfmt-dev 缺失，"
    warn "说明系统版本过旧（Ubuntu 20.04 以下），需要自行编译或升级。"
    exit 1
fi

# ---------------------------------------------------------------- 构建
say "构建（关闭测试与 Qt 界面）"
# 4G 内存下 4 路并行比较安全；按核数上限再压一层
JOBS=$(nproc)
[ "$JOBS" -gt 4 ] && JOBS=4
echo "并行度: -j$JOBS"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLOWLAT_BUILD_TESTS=OFF || exit 1
cmake --build build -j "$JOBS" || exit 1

# ---------------------------------------------------------------- 摄像头
say "检查摄像头"
if ! ls -l /dev/video* 2>/dev/null; then
    warn "没有找到 /dev/video0。"
    warn "如果是 VMware：菜单『虚拟机 → 可移动设备 → Integrated Camera → 连接』"
    warn "如果是 VirtualBox：『设备 → USB → 勾选摄像头』"
    warn "接上后重新运行本脚本。"
    exit 1
fi

echo
v4l2-ctl --list-formats-ext -d /dev/video0 2>/dev/null | head -40

# ---------------------------------------------------------------- 采集
say "采集 150 帧（1280x720@30，优先 MJPG）"
mkdir -p out
rm -f out/cam.h264

./build/src/app/lowlat_sender \
    --source v4l2 --device /dev/video0 \
    --width 1280 --height 720 --fps 30 \
    --frames 150 --output out/cam.h264
SENDER_RC=$?

if [ $SENDER_RC -ne 0 ]; then
    warn "采集失败（退出码 $SENDER_RC）。常见原因："
    warn "  · 分辨率/格式不被支持 → 试着加 --width 640 --height 480"
    warn "  · 设备被占用 → 关掉其它用摄像头的程序"
    warn "  · 权限不足 → sudo usermod -aG video \$USER 后重新登录"
    exit 1
fi

# ---------------------------------------------------------------- 校验
say "校验码流"
if ./build/tools/h264_probe out/cam.h264; then
    printf '\n\033[32m'
    echo "=============================================="
    echo " V4L2 采集验证通过"
    echo "=============================================="
    printf '\033[0m'
    echo "请把上面的 h264_probe 输出（access_units / frames / errors）发给助手，"
    echo "用来更新 README 与面试材料里的『未实机验证』表述。"

    # 若挂了共享文件夹，把证据也写一份：WSL 侧可以直接读到，不用手工复制
    for shared in /mnt/hgfs/*/; do
        [ -d "$shared" ] || continue
        cp out/cam.h264 "${shared}cam.h264" 2>/dev/null &&
            echo "码流已复制到 ${shared}cam.h264"
    done
else
    warn "码流校验失败。把 out/cam.h264 和上面的完整输出发回来。"
    exit 1
fi
