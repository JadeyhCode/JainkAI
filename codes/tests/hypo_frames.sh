#!/bin/sh
# tests/hypo_frames.sh — 假设也要**进检索/帧层**，不只是「不被代偿」
#
# 以前假设只做到「关系层被叫停」：可检索层和帧层照样按**没有假设**的世界答话，
# 于是「如果 我 不 怕 黑」之后问「你害怕黑吗」，帧层照样渲染出「我 怕 黑」——
# 和自己刚说的假设正面矛盾。现在帧/检索给的句子会先过一遍**冲突检测**
# （句面 + 假设层），和假设相反就作废，并在思维链上记一步「假设否决」。
#
# 看护四件事：
#   ① 没有假设时行为不变（帧层照常渲染）；
#   ② 假设相反时，帧层的说法被否决，思维链上写明原因；
#   ③ 假设一致时不否决（别把该说的也堵了）；
#   ④ 假设下的关系问题仍然由关系层负责到底（原有行为不能退）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT/build_aip}
MODEL=${MODEL:-$ROOT/model.bin}
cd "$ROOT"
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }

run() {
    { printf '::load %s\n::notepad off\n' "$MODEL"; for q in "$@"; do printf '%s\n' "$q"; done
      printf '::cot\n::quit\n'; } | "$BIN" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }

echo "① 没有假设：帧层照常（行为不变）"
out=$(run "你 害怕 黑 吗")
r=$(printf '%s\n' "$out" | reps | head -1)
case "$r" in *"我 怕 黑"*) chk "帧层渲染出「我 怕 黑」（$r）" 1 ;;
                     *)   chk "帧层渲染出「我 怕 黑」（实得「$r」）" 0 ;;
esac

echo "② 假设相反：帧层的说法被否决，链上写明"
out=$(run "如果 我 不 怕 黑" "你 害怕 黑 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"我 怕 黑"*) chk "与假设相反的说法**不许**出口（实得「$r」）" 0 ;;
                 *)     chk "与假设相反的说法被挡住了（$r）" 1 ;;
esac
printf '%s\n' "$out" | grep -aq '假设否决' \
    && chk "思维链上记了「假设否决」" 1 || chk "思维链上记了「假设否决」" 0

echo "③ 假设一致：不否决"
out=$(run "如果 我 怕 黑" "你 害怕 黑 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"我 怕 黑"*) chk "和假设一致时照说不误（$r）" 1 ;;
                 *)   chk "和假设一致时照说不误（实得「$r」）" 0 ;;
esac

echo "④ 假设下的关系问题仍由关系层负责到底"
out=$(run "如果 猫 会 飞" "猫 会 飞 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"在 假设 下"*"对"*) chk "关系层仍然在假设下作答（$r）" 1 ;;
                             *)    chk "关系层仍然在假设下作答（实得「$r」）" 0 ;;
esac
# 知识库里真的没有的（跳舞），假设开着时必须由关系层明说推不出来，
# 而不是悄悄丢给帧层答一句「猫 应该 会 跳舞」（那正是「不被代偿」的旧行为）。
out=$(run "如果 猫 会 飞" "猫 会 跳舞 吗")
r=$(printf '%s
' "$out" | reps | tail -1)
case "$r" in *"推不出来"*|*"不 确定"*) chk "推不出来时明说，不硬答（$r）" 1 ;;
                                 *)    chk "推不出来时明说（实得「$r」）" 0 ;;
esac
# 对照：没有假设时，同一条问题由帧层兜底（说明上面那条确实是被假设接管的）
out=$(run "猫 会 跳舞 吗")
r=$(printf '%s
' "$out" | reps | tail -1)
case "$r" in *"推不出来"*) chk "没有假设时不会说这句（$r）" 0 ;;
                 *)     chk "没有假设时由帧层兜底（$r）" 1 ;;
esac

echo
[ "$fail" -eq 0 ] && echo "PASS hypo_frames（假设进帧/检索层：冲突否决、一致放行、关系层不变）" \
                  || { echo "FAIL hypo_frames"; exit 1; }
