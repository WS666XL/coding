#!/usr/bin/env bash
#
# 抖动缓冲对照实验：固定保持窗口 vs 自适应保持窗口。
#
# 用法：
#   ./scripts/jitter_test.sh --mode fixed
#   ./scripts/jitter_test.sh --mode adaptive --k 6
#   ./scripts/jitter_test.sh --delay none --mode adaptive      # 无抖动对照组
#
# 输出一行结果，可直接对照 docs/benchmark.md 的实验二。
#
# 与 bench.sh 一样，脚本会把自己重新拉起在独立 netns 里，
# tc netem 只作用于 lo，不影响宿主网络。

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SENDER="$ROOT/build/src/app/lowlat_sender"
RECEIVER="$ROOT/build/src/app/lowlat_receiver"

MODE="fixed"
LOSS=5
DELAY="20ms 20ms"
K=6
FRAMES=600
OUTDIR="$ROOT/out/jitter"

# ---------------------------------------------------------------- 进入 netns
# 必须放在参数解析之前：解析会 shift 掉 $@，之后再 exec 就传不过去了。
if [ "${JITTER_IN_NS:-0}" != "1" ]; then
    if ! command -v unshare >/dev/null 2>&1 || ! unshare -rn true 2>/dev/null; then
        echo "错误: unshare -rn 不可用" >&2
        exit 1
    fi
    exec unshare -rn env JITTER_IN_NS=1 "$0" "$@"
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --mode)   MODE=${2:-fixed}; shift 2 ;;
        --loss)   LOSS=${2:-5}; shift 2 ;;
        --delay)  DELAY=${2:-none}; shift 2 ;;
        --k)      K=${2:-6}; shift 2 ;;
        --frames) FRAMES=${2:-600}; shift 2 ;;
        --out)    OUTDIR=${2:-$OUTDIR}; shift 2 ;;
        -h|--help)
            sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

case "$MODE" in
    fixed|adaptive) ;;
    *) echo "错误: --mode 只能是 fixed 或 adaptive" >&2; exit 1 ;;
esac

ip link set lo up || { echo "错误: 无法启用 lo" >&2; exit 1; }
for bin in "$SENDER" "$RECEIVER"; do
    [ -x "$bin" ] || { echo "错误: 找不到 $bin，请先构建" >&2; exit 1; }
done

mkdir -p "$OUTDIR"

if [ "$DELAY" = "none" ]; then
    tc qdisc replace dev lo root netem loss "${LOSS}%" >/dev/null 2>&1
    DELAY_LABEL="none"
else
    # shellcheck disable=SC2086
    tc qdisc replace dev lo root netem delay $DELAY loss "${LOSS}%" >/dev/null 2>&1
    DELAY_LABEL="$DELAY"
fi

# 参考流由本项目编码器生成，保证编码参数一致
REF="$OUTDIR/reference_${FRAMES}.h264"
if [ ! -f "$REF" ]; then
    "$SENDER" --frames "$FRAMES" --no-realtime --output "$REF" --log-level error \
        > /dev/null 2>&1 || { echo "错误: 参考流生成失败" >&2; exit 1; }
fi

tag="${MODE}_loss${LOSS}_$(echo "$DELAY_LABEL" | tr ' ' '_')_k${K}"
ulog="$OUTDIR/${tag}.recv.log"
decoded="$OUTDIR/${tag}.h264"
port=$((6300 + RANDOM % 400))

recv_flags="--hold 60"
if [ "$MODE" = "adaptive" ]; then
    recv_flags="--adaptive --jitter-k $K"
fi

# shellcheck disable=SC2086
"$RECEIVER" --bind "127.0.0.1:$port" --output "$decoded" \
    --frames "$FRAMES" --duration $((FRAMES / 30 + 20)) \
    --nack --log-level info $recv_flags > "$ulog" 2>&1 &
recv_pid=$!
sleep 1

"$SENDER" --frames "$FRAMES" --dest "127.0.0.1:$port" --fec --fec-group 8 \
    --log-level error > "$OUTDIR/${tag}.send.log" 2>&1

wait $recv_pid 2>/dev/null
rc=$?

field() {
    sed -nE "s/.*$2.*/\1/p" "$1" 2>/dev/null | tail -1 | tr -d '\r\n'
}

nals=$(field "$ulog" ' nals=([0-9]+)')
p50=$(field "$ulog" 'p50=([0-9.]+)ms')
p99=$(field "$ulog" 'p99=([0-9.]+)ms')
dmin=$(field "$ulog" '缓冲深度=([0-9]+)\.\.')
dmax=$(field "$ulog" '缓冲深度=[0-9]+\.\.([0-9]+)ms')
jitter=$(field "$ulog" '估计=([0-9.]+)ms')
psnr=$(ffmpeg -hide_banner -i "$decoded" -i "$REF" -lavfi "[0:v][1:v]psnr" -f null - 2>&1 |
    grep -oE "average:[0-9.]+|average:inf|average:nan" | tail -1 | cut -d: -f2)

printf '[result] mode=%-8s loss=%-3s delay=%-10s k=%s nals=%-5s p50=%-7s p99=%-7s depth=%s..%sms jitter=%sms psnr=%s\n' \
    "$MODE" "$LOSS" "$DELAY_LABEL" "$K" "${nals:-?}" "${p50:-?}" "${p99:-?}" \
    "${dmin:-?}" "${dmax:-?}" "${jitter:-?}" "${psnr:-nan}"

if [ "$rc" -ne 0 ] && [ "$rc" -ne 3 ]; then
    echo "[warn] 接收端退出码 $rc" >&2
fi
