#!/bin/sh
# tools/probematrix.sh — 一次跑完「22 批探针 + 全部留出集」并打一张表（§6.499）
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
for i in 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22; do
    f="tests/genprobe${i}.txt"
    [ -f "$f" ] && row "genprobe${i}" "$f"
done
for f in tests/blind4.txt tests/blind5.txt tests/blind9.txt tests/holdout.txt tests/holdout_pert.txt; do
    [ -f "$f" ] && row "$(basename "$f" .txt)" "$f"
done
