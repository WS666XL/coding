#!/usr/bin/env bash
#
# 弱网压测：按丢包档位 × 恢复策略 扫描，输出 CSV 与汇总表。
#
# 用法：
#   ./scripts/bench.sh                        # 0/2/5/10/20/30% × 3 种策略 × 2 次
#   LEVELS="0 5" REPS=1 ./scripts/bench.sh    # 小规模快速验证
#
# 设计说明：
#   1. 脚本用 unshare -rn 把自己重新拉起在独立网络命名空间里，
#      tc netem 只操作 lo，不影响宿主网络。
#   2. 测量用 headless 接收端（落盘 .h264）而不是 Qt 界面：
#      界面会引入 llvmpipe 渲染抖动，压测结果不可复现。
#   3. 参考流由本项目编码器生成，保证与发送端编码参数完全一致，
#      否则 PSNR 没有可比性。

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SENDER="$ROOT/build/src/app/lowlat_sender"
RECEIVER="$ROOT/build/src/app/lowlat_receiver"
OUTDIR="$ROOT/out/bench"
CSV="$OUTDIR/bench.csv"

FRAMES=${FRAMES:-300}
REPS=${REPS:-2}
LEVELS=${LEVELS:-"0 2 5 10 20 30"}
CONFIGS=${CONFIGS:-"none fec fec+nack"}

# ---------------------------------------------------------------- 进入 netns
if [ "${BENCH_IN_NS:-0}" != "1" ]; then
    if ! command -v unshare >/dev/null 2>&1 || ! unshare -rn true 2>/dev/null; then
        echo "错误: unshare -rn 不可用，无法创建独立网络命名空间" >&2
        exit 1
    fi
    exec unshare -rn env BENCH_IN_NS=1 "$0" "$@"
fi

ip link set lo up || { echo "错误: 无法启用 lo" >&2; exit 1; }
tc qdisc show dev lo >/dev/null 2>&1 || { echo "错误: tc 不可用" >&2; exit 1; }

for bin in "$SENDER" "$RECEIVER"; do
    [ -x "$bin" ] || { echo "错误: 找不到可执行文件 $bin，请先构建" >&2; exit 1; }
done

mkdir -p "$OUTDIR"

# 单实例保护：两个压测同时跑会抢同一批端口、互相覆盖 CSV，
# 事后无法分辨哪行来自哪一次，数据就废了。
LOCK_FILE="$OUTDIR/.bench.lock"
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    echo "错误: 已有压测在运行（$LOCK_FILE），请等待其结束或手动清理" >&2
    exit 1
fi

# 参考码流：必须与本项目编码器参数一致
REF="$OUTDIR/reference.h264"
if [ ! -f "$REF" ]; then
    echo "[bench] 生成参考码流 ($FRAMES 帧)..."
    "$SENDER" --frames "$FRAMES" --no-realtime --output "$REF" --log-level error \
        > /dev/null 2>&1 || { echo "错误: 参考码流生成失败" >&2; exit 1; }
fi

# ---------------------------------------------------------------- 解析工具
# 从日志里提取 key=value 中的 value（取最后一处匹配，即最终汇总行）。
#
# 注意 sed 的 s/// 只替换匹配到的片段，所以必须用 .* 吃掉整行，
# 否则时间戳等前后文会一起留在结果里（第一版就踩了这个坑）。
extract() {
    local file=$1 pattern=$2
    [ -f "$file" ] || { echo ""; return; }
    sed -nE "s/.*${pattern}.*/\1/p" "$file" 2>/dev/null | tail -1 | tr -d '\r\n'
}

# PSNR 可能返回 inf（两路完全相同）或 nan（无法比较），都要保留原样
psnr_of() {
    local decoded=$1 value
    value=$(ffmpeg -hide_banner -i "$decoded" -i "$REF" -lavfi "[0:v][1:v]psnr" -f null - 2>&1 |
        grep -oE "average:[0-9.]+|average:inf|average:nan" | tail -1 | cut -d: -f2)
    echo "${value:-nan}"
}

cleanup() {
    pkill -f 'lowlat_(sender|receiver)' 2>/dev/null
    tc qdisc del dev lo root 2>/dev/null
}
trap cleanup EXIT

# ---------------------------------------------------------------- 主循环
level_count=$(echo "$LEVELS" | wc -w)
config_count=$(echo "$CONFIGS" | wc -w)
total=$((level_count * config_count * REPS))
run_seconds=$((FRAMES / 30 + 8))

echo "[bench] 共 $total 次运行，每次 $FRAMES 帧（最长 ${run_seconds}s/次）"
echo "loss_pct,config,rep,frames,nals,gaps,seq_breaks,fec_repaired,nack_sent,retransmit,e2e_p50_ms,e2e_p99_ms,e2e_max_ms,jitter_ms,rtt_ms,psnr_db" > "$CSV"

port=6100
index=0
for loss in $LEVELS; do
    for config in $CONFIGS; do
        for rep in $(seq 1 "$REPS"); do
            index=$((index + 1))
            port=$((port + 1))
            suffix=$(echo "$config" | tr '+' '-')
            tag="l${loss}_${suffix}_r${rep}"
            ulog="$OUTDIR/${tag}.recv.log"
            slog="$OUTDIR/${tag}.send.log"
            decoded="$OUTDIR/${tag}.h264"

            tc qdisc replace dev lo root netem loss "${loss}%" >/dev/null 2>&1

            recv_flags=""
            send_flags=""
            case "$config" in
                fec)      send_flags="--fec --fec-group 8" ;;
                fec+nack) send_flags="--fec --fec-group 8"; recv_flags="--nack" ;;
                none)     ;;
            esac

            # shellcheck disable=SC2086
            "$RECEIVER" --bind "127.0.0.1:$port" --output "$decoded" \
                --frames "$FRAMES" --duration "$run_seconds" \
                --log-level info $recv_flags > "$ulog" 2>&1 &
            recv_pid=$!

            sleep 1   # 等接收端绑定端口，避免启动竞态

            # shellcheck disable=SC2086
            "$SENDER" --frames "$FRAMES" --dest "127.0.0.1:$port" \
                --log-level error $send_flags > "$slog" 2>&1

            wait $recv_pid 2>/dev/null

            frames=$(extract "$ulog" 'frames=([0-9]+)')
            nals=$(extract "$ulog" ' nals=([0-9]+)')
            gaps=$(extract "$ulog" 'frame_gaps=([0-9]+)')
            seq_breaks=$(extract "$ulog" 'sequence_breaks=([0-9]+)')
            fec_fix=$(extract "$ulog" '恢复包=([0-9]+)')
            nack=$(extract "$ulog" '请求次数=([0-9]+)')
            retx=$(extract "$ulog" '收到重传=([0-9]+)')
            p50=$(extract "$ulog" 'p50=([0-9.]+)ms')
            p99=$(extract "$ulog" 'p99=([0-9.]+)ms')
            pmax=$(extract "$ulog" 'max=([0-9.]+)ms')
            jitter=$(extract "$ulog" '估计=([0-9.]+)ms')
            rtt=$(extract "$ulog" 'srtt=([0-9.]+)ms')
            psnr=$(psnr_of "$decoded")

            echo "  [$index/$total] loss=${loss}% ${config} #${rep}: nals=${nals:-?} p50=${p50:-?}ms psnr=${psnr}dB"
            echo "${loss},${config},${rep},${frames:-0},${nals:-0},${gaps:-0},${seq_breaks:-0},${fec_fix:-0},${nack:-0},${retx:-0},${p50:-0},${p99:-0},${pmax:-0},${jitter:-0},${rtt:-0},${psnr}" >> "$CSV"
        done
    done
done

echo
echo "[bench] CSV: $CSV"
echo

# ---------------------------------------------------------------- 汇总
echo "汇总（每档算术平均；psnr 列 inf 表示与参考流完全一致）:"
printf "  %-5s %-9s %8s %9s %11s %9s\n" "loss" "config" "nals" "p99(ms)" "psnr(dB)" "fec/nack"
awk -F, 'NR > 1 {
    key = $1 "|" $2;
    n[key]++;
    nals[key] += $5;
    p99[key]  += $12;
    fix[key]  += $8;
    nack[key] += $9;
    p = $16;
    if (p == "inf") { inf[key]++; }
    else if (p != "nan" && p != "") { psnr[key] += p; psnr_n[key]++; }
}
END {
    for (k in n) {
        split(k, part, "|");
        psnr_text = "n/a";
        if (inf[k] > 0) {
            psnr_text = "inf";
        } else if (psnr_n[k] > 0) {
            psnr_text = sprintf("%.2f", psnr[k] / psnr_n[k]);
        }
        printf "  %-5s %-9s %8.0f %9.1f %11s %5.0f/%-5.0f\n",
            part[1], part[2], nals[k]/n[k], p99[k]/n[k], psnr_text, fix[k]/n[k], nack[k]/n[k];
    }
}' "$CSV" | sort -n -k1 -k2

echo
echo "[bench] 提示："
echo "  - psnr=inf 表示该档位恢复后与参考流完全一致（5% 丢包 + FEC + NACK 常见）"
echo "  - psnr=nan 表示输出无法解码，通常是该档位丢包过重导致关键帧缺失"
