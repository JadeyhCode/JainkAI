#!/bin/sh
# tools/watchdog.sh — 流水线看护：边数不涨但积压在涨 ⇒ 立刻转（§6.728）
#
# 为什么需要：前面三次"进度停住"全是一个形状 ——
#   **积压文本在涨、分片边数不涨**（块名撞车 / 轮转阈值太大 / 转换卡死占锁），
#   而人在盯着别的事。所以让机器自己盯：
#
#   每轮（默认 300 s）：
#     1. 记录 边数 / 待转字节 / 各源进程数
#     2. 若"边数连续 WARN_ROUNDS 轮没涨"且"待转 > 200 MB" ⇒ 跑一次 facts2shards
#     3. 若某源进程数为 0 ⇒ 记 WARN（不自动重启：重启由人/目标轮决定，避免反复拉起半死进程）
#
# 用法：sh tools/watchdog.sh [--interval 秒] [--rounds N]
set -u
M=${MODELS:-/mnt/models}
LOG="$M/watchdog.log"
INTERVAL=300
ROUNDS=0
while [ $# -gt 0 ]; do
    case "$1" in
    --interval) INTERVAL=$2; shift 2 ;;
    --rounds) ROUNDS=$2; shift 2 ;;
    *) shift ;;
    esac
done
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$M" || exit 1

edges() {   # Debian + Ubuntu 两个语料目录的边数（只看 codecorpus 会一直显示"不涨"）
    for f in "$M"/codecorpus/shards/*.gz "$M"/ubucorpus/shards/*.gz; do
        [ -f "$f" ] || continue
        gunzip -c "$f" 2>/dev/null | head -c 64 | od -An -tu8 -j 16 -N 8 | tr -d ' '
    done | awk '{s += $1} END {print s + 0}'
}
pend_mb() { du -sm "$M"/codecorpus/facts*.[0-9]*.triples "$M"/ubucorpus/facts*.[0-9]*.triples 2>/dev/null | awk '{s += $1} END {print s + 0}'; }
nproc_src() { [ -n "$1" ] && ps -eo args 2>/dev/null | grep -c "^sh tools/$1" || echo 0; }

prev_e=0; stale=0; round=0
while :; do
    round=$((round + 1))
    e=$(edges); p=$(pend_mb)
    d=$((e - prev_e))
    printf '[%s] 边=%s 待转=%s MB 增=%s | Debian=%s 转换=%s PyPI=%s gems=%s npm=%s\n' \
        "$(date '+%H:%M:%S')" "$e" "$p" "$d" \
        "$(nproc_src src_harvest.sh)" "$(nproc_src facts2shards.sh)" \
        "$(nproc_src src_pypi.sh)" "$(nproc_src src_gems.sh)" "$(nproc_src src_npm.sh)" >> "$LOG"

    if [ "$d" -le 0 ]; then
        stale=$((stale + 1))
    else
        stale=0
    fi
    # 边不涨 + 有积压 ⇒ 立刻转（这是三次卡死的共同形状）
    if [ "$stale" -ge 2 ] && [ "$p" -ge 200 ]; then
        printf '[%s] ⚠ 边数 %d 轮不涨但积压 %s MB ⇒ 触发 facts2shards\n' "$(date '+%H:%M:%S')" "$stale" "$p" >> "$LOG"
        sh "$ROOT/tools/facts2shards.sh" "$M/codecorpus" >> "$LOG" 2>&1
        sh "$ROOT/tools/facts2shards.sh" "$M/ubucorpus" >> "$LOG" 2>&1
        stale=0
    fi
    # 某个源进程数为 0 ⇒ 记 WARN（人工/目标轮决定是否重启）
    for s in src_harvest.sh facts2shards.sh src_pypi.sh src_gems.sh src_npm.sh; do
        n=$(nproc_src "$s")
        [ "$n" -eq 0 ] && printf '[%s] ⚠ %s 没有进程在跑\n' "$(date '+%H:%M:%S')" "$s" >> "$LOG"
    done
    prev_e=$e
    [ "$ROUNDS" -gt 0 ] && [ "$round" -ge "$ROUNDS" ] && break
    sleep "$INTERVAL"
done
