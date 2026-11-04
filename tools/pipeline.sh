#!/bin/sh
# tools/pipeline.sh — 7B 抓取管道的**唯一管理器**（start / stop / status）§6.747
#
# 为什么需要：之前用"散装 setsid + 反复手工重启"，结果出现过
#   · 87 个续跑器同时派生 worker（互相抢索引、抢带宽）
#   · 4 个 curl 抢同一个 Sources.gz
#   · 杀不干净（按名字匹配常把当前 shell 也杀了）
# 现在所有组件都由这一个脚本启动，PID 记在 $STATE/pipeline.pids，按**进程组**停。
#
# 用法：sh tools/pipeline.sh start|stop|status
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
M=${MODELS:-/mnt/models}
STATE="$M"
PIDF="$STATE/pipeline.pids"
W=${WORKERS:-6}          # 每个语料的 worker 数（keepgo 会一直续跑，k 自动轮转）
slots="ubucorpus:universe:noble:ubuntu ubumain:main:noble:ubuntu debtrix:main:trixie:debian"

start_one() {  # $1=名字 $2...=命令
    name=$1; shift
    setsid nohup "$@" >> "$STATE/pipeline.$name.log" 2>&1 < /dev/null &
    printf '%s %s\n' "$name" "$!" >> "$PIDF"
}

do_stop() {
    [ -s "$PIDF" ] || echo "（没有 PID 文件，只做兜底清理）"
    if [ -s "$PIDF" ]; then
        while read -r name pid; do
            [ -n "${pid:-}" ] || continue
            kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null
        done < "$PIDF"
    fi
    # 兜底：按"进程名是 sh 且命令行含组件路径"清一遍（**绝对路径也能匹配**，
    # 同时排除当前 shell/父 shell，避免第 N 次"杀到自己"）
    for rx in 'tools/keepgo.sh' 'tools/src_harvest.sh' 'tools/facts2shards.sh' 'tools/refresh_index.sh' 'tools/watchdog.sh'; do
        for p in $(pgrep -f "$rx" 2>/dev/null); do
            [ "$p" = "$$" ] && continue
            [ "$p" = "$PPID" ] && continue
            c=$(ps -o comm= -p "$p" 2>/dev/null)
            [ "$c" = "sh" ] || [ "$c" = "bash" ] || [ "$c" = "timeout" ] || continue
            kill "$p" 2>/dev/null
        done
    done
    : > "$PIDF"
    echo "已停止"
}

do_start() {
    mkdir -p "$STATE"; : > "$PIDF"
    start_one refresh sh "$ROOT/tools/refresh_index.sh" 600
    start_one watchdog sh "$ROOT/tools/watchdog.sh" --interval 300
    for s in $slots; do
        d=${s%%:*}; rest=${s#*:}; suite=${rest%%:*}; rest=${rest#*:}; dist=${rest%%:*}; mirror=${rest##*:}
        base="https://mirrors.huaweicloud.com/$mirror"
        for k in $(seq 1 "$W"); do
            start_one "keepgo-$d-$k" sh "$ROOT/tools/keepgo.sh" "$M/$d" "$k" 33 "$base" "$dist" "$suite"
        done
        for i in 1 2; do
            start_one "conv-$d-$i" sh "$ROOT/tools/facts2shards.sh" "$M/$d" --watch 60
        done
    done
    echo "已启动：见 $PIDF"
}

do_status() {
    echo "== 管道组件 =="
    for rx in 'tools/keepgo.sh' 'tools/src_harvest.sh /mnt' 'tools/src_pypi.sh' 'tools/src_gems.sh' \
              'tools/src_npm.sh' 'tools/src_bigsrc.sh' 'tools/facts2shards.sh' 'tools/refresh_index.sh' 'tools/watchdog.sh'; do
        printf '  %-28s %s\n' "$rx" "$(ps -eo args | grep -c "$rx")"
    done
    echo "== 进度 =="
    sh "$ROOT/tools/progress.sh" 2>/dev/null | tail -4
}

case "${1:-status}" in
start) do_start ;;
stop) do_stop ;;
status) do_status ;;
*) echo "用法：sh tools/pipeline.sh start|stop|status"; exit 2 ;;
esac
