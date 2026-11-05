#!/bin/sh
# tools/ab_dialog.sh — 两个模型的对话 A/B 对比（§6.750）
# 用法：sh tools/ab_dialog.sh <模型A> <模型B>
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A=${1:?}; B=${2:?}
QS="你是谁|你好|猫是什么|什么是学习|谢谢|对不起|再见|你能做什么|为什么天会下雨|1加1等于几"
i=0; oldIFS=$IFS; IFS='|'
printf '%-16s %-34s %-34s\n' "问" "A: $(basename "$A")" "B: $(basename "$B")"
printf '%.0s-' $(seq 1 88); echo
for q in $QS; do
    i=$((i + 1))
    ra=$(printf '%s\n::quit\n' "$q" | "$ROOT/build_aip" --model "$A" 2>/dev/null | grep -a 'AI > ' | head -1 | sed 's/.*AI > //' | cut -c1-32)
    rb=$(printf '%s\n::quit\n' "$q" | "$ROOT/build_aip" --model "$B" 2>/dev/null | grep -a 'AI > ' | head -1 | sed 's/.*AI > //' | cut -c1-32)
    printf '%-16s %-34s %-34s\n' "$q" "${ra:-（空）}" "${rb:-（空）}"
done
IFS=$oldIFS
