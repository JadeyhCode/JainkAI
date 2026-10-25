#!/bin/sh
# tools/probematrix.sh — 一次跑完「全部 genprobe 探针 + 全部留出集」并打一张表（§6.499；§6.528 起自动收全）
#
# 用途：回归轮专用。改完引擎/数据后跑一次，看有没有哪一批掉了。
# 用法：sh tools/probematrix.sh [模型]     默认 session.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MODEL=${1:-session.bin}
[ -f tests/holdout.sh ] || { echo "FAIL: 缺 tests/holdout.sh"; exit 1; }

row() {   # $1=标签 $2=文件
    _o=$(sh tests/holdout.sh "$MODEL" "$2" 2>&1 | head -1)
    _n=$(printf '%s' "$_o" | sed -n 's/.*：\([0-9]*\) 通过 \/ \([0-9]*\) 条.*/\1\/\2/p')
    [ -n "$_n" ] || _n="?"
    printf '| %-18s | %s |\n' "$1" "$_n"
}

echo "| 集合 | 通过/总数 |"
echo "|---|---|"
row "genprobe" tests/genprobe.txt
# §6.528：这里原来是写死的 `2 3 … 22` —— genprobe23 加了却没进表（不进表＝回归看不见）。
# 改成「有就跑到没有」：以后新增探针只要文件在，自动进表。
i=2
while [ -f "tests/genprobe${i}.txt" ]; do
    row "genprobe${i}" "tests/genprobe${i}.txt"
    i=$((i + 1))
done
# §6.606 带标点的教法单独一行：以前矩阵里没有一行带标点，§6.605 的修复在 A/B 里看不见
[ -f tests/teachpunct.txt ] && row "teachpunct" tests/teachpunct.txt
for f in tests/blind4.txt tests/blind5.txt tests/blind9.txt tests/holdout.txt tests/holdout_pert.txt; do
    [ -f "$f" ] && row "$(basename "$f" .txt)" "$f"
done
