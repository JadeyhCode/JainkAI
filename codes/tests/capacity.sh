#!/bin/sh
# tests/capacity.sh — **一次会话里灌 N 条新事实**，看记得住吗、慢不慢（§6.327）
#
# 做法：造 N 个假词（前缀 × 天干），逐条教「X 属于 动物」，再问最早/中间/最晚的几个。
# 这不是回归套件（不进 suites.txt）：它出的是**容量/耗时基线**。
# 用法：sh tests/capacity.sh [模型] [N，默认 60]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
N=${2:-60}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }

GAN=$(printf '甲乙丙丁戊己庚辛壬癸' | sed 's/./& /g')
PRE=$(printf '兽 灵 玄 黄 白 赤 青 黑')
TMP=${TMPDIR:-/tmp}/cap.$$
trap 'rm -f "$TMP.in" "$TMP.out"' EXIT INT TERM
: > "$TMP.in"
i=0
NAMES=""
for p in $PRE; do
    for g in $GAN; do
        [ "$i" -lt "$N" ] || break 2
        i=$((i+1))
        NAMES="$NAMES $p$g"
        printf '教我%s属于动物\n' "$p$g" >> "$TMP.in"
    done
done
set -- $NAMES
first=$1
mid=$(eval "echo \${$(( (i+1)/2 ))}")
last=$(eval "echo \${$i}")
{ printf '%s属于什么\n' "$first"; printf '%s属于什么\n' "$mid"; printf '%s属于什么\n' "$last"; } >> "$TMP.in"
printf '::quit\n' >> "$TMP.in"

t0=$(date +%s%N 2>/dev/null || echo 0)
timeout 300 "$BIN" --model "$MODEL" < "$TMP.in" > "$TMP.out" 2>&1
t1=$(date +%s%N 2>/dev/null || echo 0)
ms=$(( (t1 - t0) / 1000000 ))

teach=$(sed -n 's/.*AI > //p' "$TMP.out" | grep -cE "记下|我 知道" || true)
ok=0
for w in "$first" "$mid" "$last"; do
    ans=$(grep -a "AI > " "$TMP.out" | sed -n "s/.*AI > //p" | tail -3 | grep -c "$w 属于 动物" || true)
    [ "${ans:-0}" -ge 1 ] && ok=$((ok+1))
done
echo "容量：教 $i 条，被记住 $teach 条；抽查最早/中间/最晚 3 个 ⇒ $ok/3 答对；耗时 ${ms} ms"
