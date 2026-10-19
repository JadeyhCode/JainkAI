#!/bin/sh
# tests/abdiff.sh — **逐条 diff 消融结果**（§6.381）：某桶的贡献是正是负，到底差哪几条
#
# 为什么单有一份：`bucketabl.sh` 只给数字（比如 reply 是 −1）。−1 这种小数字**不能当噪声划过**——
# §6.380 的 −1 就藏着一行过宽的 `[reply] 你能,吗`。定位方法是：同一组留出集跑两次（开/关该桶），
# 把失败清单逐条 diff。这个脚本干这件事。
#
# 用法：
#   sh tests/abdiff.sh tests/blind5.txt reply        # 关掉 reply 桶比一比
#   sh tests/abdiff.sh tests/blind5.txt              # 不带桶名 = 全部数据关掉（AIP_NODATA=1）
# 不是回归套件（不进 suites.txt）：它只打印差异，退出码恒 0。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MODEL=${MODEL:-session.bin}
SET=${1:-tests/blind5.txt}
BUCKET=${2:-}
[ -f "$SET" ] || { echo "用法：sh tests/abdiff.sh <留出集文件> [桶名]"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

fails() {   # $1 = 额外的 AIP_NODATA 值（空 = 全开）
    if [ -n "$1" ]; then
        env AIP_NODATA="$1" sh tests/holdout.sh "$MODEL" "$SET" 2>&1
    else
        sh tests/holdout.sh "$MODEL" "$SET" 2>&1
    fi | grep '✗' | sed 's/  ⇒.*//' | sed 's/^ *✗ *//'
}

fails "" | sort > "$TMP/on.txt" || true
fails "$BUCKET" | sort > "$TMP/off.txt" || true

echo "组：$SET    关掉的桶：${BUCKET:-（全部数据）}"
echo "  开：$(wc -l < "$TMP/on.txt" | tr -d ' ') 条失败"
echo "  关：$(wc -l < "$TMP/off.txt" | tr -d ' ') 条失败"
echo

improved=$(comm -23 "$TMP/on.txt" "$TMP/off.txt" || true)
worsened=$(comm -13 "$TMP/on.txt" "$TMP/off.txt" || true)
if [ -n "$improved" ]; then
    echo "关掉这个桶之后**变好**（说明这桶在这几条上是帮倒忙）："
    printf '%s\n' "$improved" | sed 's/^/  + /'
fi
if [ -n "$worsened" ]; then
    echo "关掉这个桶之后**变坏**（说明这桶在这几条上是承重的）："
    printf '%s\n' "$worsened" | sed 's/^/  - /'
fi
[ -z "$improved$worsened" ] && echo "  两边失败清单完全相同 —— 这个桶在**这组**上没有可观察的贡献。" || true
