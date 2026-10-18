#!/bin/sh
# tests/attrib.sh — **归因**：当前能力里有多少来自数据行、多少来自内核本身（§6.306）
#
# 做法：同一套度量跑两遍 —— 正常读 `patterns.txt` vs `AIP_NODATA=1`（数据全空）。
# 两列之差 = **数据行的贡献**；不差的部分 = 内核/模型本来就会的。
# 这不是回归套件，不进 suites.txt。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }

one() {   # $1=命令，$2=文件 → 打印 "通过/总数"
    sh "$1" "$MODEL" "$2" 2>/dev/null | sed -n 's/.*：\([0-9]*\) 通过 \/ \([0-9]*\).*/\1\/\2/p' | head -1
}
printf '%-22s %10s %10s %8s\n' "度量" "有数据" "无数据" "数据贡献"
tot_on=0; tot_off=0; tot_all=0
for f in tests/holdout.txt tests/holdout2.txt tests/holdout3.txt \
         tests/holdout4.txt tests/holdout5.txt tests/holdout6.txt tests/holdout7.txt tests/holdout8.txt tests/holdout9.txt \
         tests/conv.txt tests/conv2.txt; do
    case "$f" in
        *conv.txt|*conv2.txt) run=tests/conv.sh ;;
        *) run=tests/holdout.sh ;;
    esac
    on=$(one "$run" "$f")
    off=$(AIP_NODATA=1 sh "$run" "$MODEL" "$f" 2>/dev/null | sed -n 's/.*：\([0-9]*\) 通过 \/ \([0-9]*\).*/\1\/\2/p' | head -1)
    a=${on%/*}; b=${on#*/}; c=${off%/*}
    [ -n "$a" ] && [ -n "$c" ] || continue
    printf '%-22s %10s %10s %8s\n' "$(basename "$f")" "$on" "$off" "$((a-c))"
    tot_on=$((tot_on+a)); tot_off=$((tot_off+c)); tot_all=$((tot_all+b))
done
echo "合计：$tot_on / $tot_all 通过；无数据时 $tot_off / $tot_all（**数据行贡献 $((tot_on-tot_off)) 项**）"
