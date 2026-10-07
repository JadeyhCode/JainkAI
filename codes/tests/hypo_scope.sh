#!/bin/sh
# tests/hypo_scope.sh — 假设的作用域回溯：撤哪一层由你说了算
#
# 原来只有「算了」= 弹**最深**那层。可多层嵌套时经常要抽掉中间那层：
#   如果 猫 属于 鸟      ← 这条作废
#   如果 他 以为 猫 会 飞 ← 这条还得留着
# 现在四种说法都认：
#   撤回 第 2 个 假设        （按序号，从 1 起；中文/阿拉伯数字都行）
#   撤回 关于 猫 的 假设     （哪层提到它就撤哪层，从深往浅找）
#   撤回 猫 属于 鸟          （按内容）
#   猫 属于 鸟 不算 了       （按内容，另一种语序 —— 最常用）
# 看护：撤的确实是**指定**那层、剩下的层顺序不变、找不着时如实说不乱撤。
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
    { printf '::load %s\n' "$MODEL"; for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'; } | "$BIN" 2>&1
}
# ::hypo 那一段的输出（第几层、每层写了什么、算在谁头上）
layers() { printf '%s\n' "$1" | sed -n '/当前假设世界/,$p' | head -12; }

echo "① 先压两层：第 1 层我的、第 2 层算在「他」头上"
out=$(run "如果 猫 属于 鸟" "如果 他 以为 猫 会 飞" "::hypo")
printf '%s\n' "$(layers "$out")" | grep -aq '第 1 层：' \
    && chk "第 1 层在" 1 || chk "第 1 层在" 0
printf '%s\n' "$(layers "$out")" | grep -aq '第 2 层（这一层算在「他」头上）' \
    && chk "第 2 层归他" 1 || chk "第 2 层归他" 0

echo "② 「猫 属于 鸟 不算 了」抽掉第 1 层，第 2 层要还在（不从最深的弹）"
out=$(run "如果 猫 属于 鸟" "如果 他 以为 猫 会 飞" "猫 属于 鸟 不算 了" "::hypo")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | tail -1)
case "$r" in *"撤了 第 1 层"*) chk "撤的是第 1 层（$r）" 1 ;;
                        *)     chk "撤的是第 1 层（实得「$r」）" 0 ;;
esac
l=$(layers "$out")
printf '%s\n' "$l" | grep -aq '他 以为： 猫 会 飞' \
    && chk "第 2 层（他以为）还在，且顺序没乱" 1 || chk "第 2 层还在（实得「$l」）" 0
printf '%s\n' "$l" | grep -aq '猫 属于 鸟' \
    && chk "被撤的第 1 层不该再出现在列表里" 0 || chk "被撤的第 1 层不在列表里" 1
printf '%s\n' "$l" | grep -aq '当前假设世界：1 层' \
    && chk "只剩 1 层" 1 || chk "只剩 1 层" 0

echo "③ 按序号撤：撤回 第 2 个 假设（第 1 层留着）"
out=$(run "如果 猫 属于 鸟" "如果 他 以为 猫 会 飞" "撤回 第 2 个 假设" "::hypo")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | tail -1)
case "$r" in *"撤了 第 2 层"*) chk "撤的是第 2 层（$r）" 1 ;;
                        *)     chk "撤的是第 2 层（实得「$r」）" 0 ;;
esac
l=$(layers "$out")
printf '%s\n' "$l" | grep -aq '猫 属于 鸟' \
    && chk "第 1 层（猫属于鸟）留着" 1 || chk "第 1 层留着" 0

echo "④ 按主语撤：撤回 关于 猫 的 假设（从深往浅找）"
out=$(run "如果 猫 属于 鸟" "如果 他 以为 猫 会 飞" "撤回 关于 猫 的 假设" "::hypo")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | tail -1)
case "$r" in *"撤了 第 2 层"*) chk "先撤最近提到「猫」的那层（$r）" 1 ;;
                        *)     chk "先撤最近提到「猫」的那层（实得「$r」）" 0 ;;
esac

echo "⑤ 找不着就如实说，且一层都不许乱撤"
out=$(run "如果 猫 属于 鸟" "如果 他 以为 猫 会 飞" "撤回 猫 会 游泳 不算 了" "::hypo")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | tail -1)
case "$r" in *"没 找到"*) chk "如实说没找到（$r）" 1 ;;
                 *)     chk "如实说没找到（实得「$r」）" 0 ;;
esac
printf '%s\n' "$(layers "$out")" | grep -aq '当前假设世界：2 层' \
    && chk "两层都还在（没有默默撤错一层）" 1 || chk "两层都还在" 0

echo "⑥ 来源账说清**在哪一层**（假设本来就是栈，不是"只有一层"）"
out=$(run "如果 猫 会 飞" "如果 他 以为 猫 会 游泳" "::src 猫 会 飞" "::src 猫 会 游泳" "::hypo drop 1" "::hypo")
printf '%s\n' "$out" | grep -aq '假设层：第 1 层（共 2 层' \
    && chk "第 1 层那条：来源账报「第 1 层（共 2 层）」" 1 \
    || chk "第 1 层报层号（实得：$(printf '%s\n' "$out" | grep '假设层：第' | head -1)）" 0
printf '%s\n' "$out" | grep -aq '假设层：第 2 层（共 2 层' \
    && chk "第 2 层那条（算在「他」头上）：也报得对" 1 \
    || chk "第 2 层报层号" 0
printf '%s\n' "$(layers "$out")" | grep -aq '当前假设世界：1 层' \
    && chk "抽掉第 1 层之后只剩 1 层（整体下移，不弹最深）" 1 || chk "抽层" 0

echo
[ "$fail" -eq 0 ] && echo "PASS hypo_scope（作用域回溯：按序号/主语/内容撤指定那层 + 来源账报层号）" \
                  || { echo "FAIL hypo_scope"; exit 1; }
