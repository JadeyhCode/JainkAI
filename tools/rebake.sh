#!/bin/sh
# tools/rebake.sh — **行为基线重标定**（§6.774）
#
# 背景（§6.771）：`model.bin` 是标定产物 —— 图的内容本身就是行为的一部分。
# 想让对话模型吸收新知识（帧、真知识、API 事实…），就必须**显式重标定**：
#   ① 用固定探针集问一遍，把答案存成基线（`tests/baselines/<名字>.txt`）；
#   ② 以后换模型时，跟基线 diff，**逐条确认**变化是有意的，才更新基线。
#
# 用法：
#   sh tools/rebake.sh <模型> <基线名>          # 生成/更新基线
#   sh tools/rebake.sh <模型> <基线名> --check   # 只比对，不改基线（有差异就 exit 1）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
M=${1:?用法: sh tools/rebake.sh <模型> <基线名> [--check]}
NAME=${2:?用法: sh tools/rebake.sh <模型> <基线名> [--check]}
MODE=${3:-write}
DIR="$ROOT/tests/baselines"; mkdir -p "$DIR"
BASE="$DIR/$NAME.txt"
PROBES="$ROOT/tests/fixtures/behavior_probes.txt"
[ -s "$PROBES" ] || { echo "FAIL: 缺探针集 $PROBES"; exit 1; }
TMP="$DIR/$NAME.tmp"; : > "$TMP"
while IFS= read -r q; do
    [ -n "$q" ] || continue
    a=$(printf '%s\n::quit\n' "$q" | "$ROOT/build_aip" --model "$M" 2>/dev/null | sed -n 's/^.*AI > //p' | head -1)
    printf '%s\t%s\n' "$q" "${a:-（空）}" >> "$TMP"
done < "$PROBES"
if [ "$MODE" = "--check" ]; then
    if [ -f "$BASE" ] && diff -q "$BASE" "$TMP" >/dev/null 2>&1; then
        echo "  基线 $NAME 一致（$(wc -l < "$TMP") 条探针）"; rm -f "$TMP"; exit 0
    fi
    echo "  基线 $NAME 有差异："
    diff "$BASE" "$TMP" 2>/dev/null | head -20 | sed 's/^/    /'
    rm -f "$TMP"; exit 1
fi
mv "$TMP" "$BASE"
echo "  基线 $NAME 已更新（$(wc -l < "$BASE") 条探针 ⇒ $BASE）"
