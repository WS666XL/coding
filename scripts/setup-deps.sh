#!/usr/bin/env bash
# lowlat-video 依赖安装脚本 (Ubuntu 24.04)
# 幂等：已安装的包会被 apt 自动跳过，可重复执行。
set -euo pipefail

echo ">>> [0/4] 检查系统版本"
. /etc/os-release
echo "    当前系统: ${PRETTY_NAME}"

echo
echo ">>> [1/4] 刷新软件源索引"
sudo apt-get update

echo
echo ">>> [2/4] 构建 / 调试 / 分析工具"
sudo apt-get install -y \
    build-essential \
    ninja-build \
    ccache \
    gdb \
    pkg-config \
    linux-tools-common \
    linux-tools-generic \
    tcpdump

echo
echo ">>> [3/4] 多媒体、日志、测试与 Qt6 依赖"
sudo apt-get install -y \
    ffmpeg \
    libavcodec-dev \
    libavformat-dev \
    libswscale-dev \
    libavutil-dev \
    libx264-dev \
    libspdlog-dev \
    libfmt-dev \
    libgtest-dev \
    qt6-base-dev \
    qt6-charts-dev

echo
echo ">>> [4/4] 虚拟摄像头（需要编译内核模块）"
if sudo apt-get install -y v4l2loopback-dkms v4l2loopback-utils; then
    echo "    安装完成。加载方式:"
    echo "      sudo modprobe v4l2loopback video_nr=10 card_label=vcam exclusive_caps=1"
else
    echo "    [警告] v4l2loopback 安装失败，常见原因是 Secure Boot 阻止内核模块签名。"
    echo "           可关闭 Secure Boot 或用 mokutil 签名；失败时程序会自动回退到合成源。"
fi

echo
echo ">>> 依赖安装流程结束"
echo "    验证 perf: ls /usr/lib/linux-tools/*/perf"
