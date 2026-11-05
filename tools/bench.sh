#!/bin/sh
# tools/bench.sh — 这个"大图符号 AI"到底多快（§6.752）
#
# 四个数：
#   ① 载入时间（冷启动读盘 + 建索引）
#   ② 单条查询延迟（**差分法**：3000 条 vs 0 条，扣掉进程启动/收尾的固定开销）
#   ③ 吞吐（条/秒，含进程启动 ⇒ 指"一问一答"的端到端）
#   ④ 常驻内存（RSS 峰值）
#
# 用法：sh tools/bench.sh <模型> [查询条数，默认 3000]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
M=${1:?用法: sh tools/bench.sh <模型> [条数]}
N=${2:-3000}
ASK="猫是什么"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

now_ms() { date +%s%N | cut -c1-13; }

# ① 载入（三次取最小）
best=999999
for i in 1 2 3; do
    t0=$(now_ms)
    printf '::quit\n' | "$ROOT/build_aip" --model "$M" > /dev/null 2>&1
    t1=$(now_ms)
    d=$((t1 - t0)); [ "$d" -lt "$best" ] && best=$d
done
load=$best

# ② 差分延迟：N 条 vs 0 条
yes "$ASK" | head -n "$N" > "$tmp/q.txt"
echo '::quit' >> "$tmp/q.txt"
printf '::quit\n' > "$tmp/e.txt"
b0=$(now_ms); "$ROOT/build_aip" --model "$M" < "$tmp/q.txt" > /dev/null 2>&1; b1=$(now_ms)
e0=$(now_ms); "$ROOT/build_aip" --model "$M" < "$tmp/e.txt" > /dev/null 2>&1; e1=$(now_ms)
run_q=$((b1 - b0)); run_e=$((e1 - e0))
per=$(awk -v a="$run_q" -v b="$run_e" -v n="$N" 'BEGIN{printf "%.0f", (a-b)*1000/n}')   # µs/条
qps=$(awk -v a="$run_q" -v b="$run_e" -v n="$N" 'BEGIN{printf "%.0f", n/((a-b)/1000)}')

# ④ RSS 峰值
rss=$("$ROOT/build_aip" --model "$M" --help > /dev/null 2>&1; /usr/bin/time -f '%M' "$ROOT/build_aip" --model "$M" < "$tmp/e.txt" 2>&1 >/dev/null | tail -1)

printf '模型 %-20s 载入 %4s ms / 单查询 %4s µs / 吞吐 %6s 条每秒 / RSS %5s MB\n' \
    "$(basename "$M")" "$load" "$per" "$qps" "$(awk -v k="$rss" 'BEGIN{printf "%.0f", k/1024}')"
