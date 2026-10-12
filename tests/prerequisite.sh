#!/bin/sh
# tests/prerequisite.sh — 训练数据里没有「猫 会不会唱歌」，也能推出来
#
# 这正是「缺前提 → 否定能力」（规则 R9）要干的事：
#     唱歌 需要 灵活的声带        （做这件事的前提）
#     猫 没有 灵活的声带          （这个主语没有那个前提）
#   ⟹ 猫 不 会 唱歌              （结论在语料里**从来没出现过**）
#
# 看护四件事：
#   ① 真能推出来，而且链上写明依据是「缺前提」，验证器独立复核过（缺口=0）；
#   ② 结论确实**来自数据**：把「需要」那两条知识拿掉，就不许再推否定
#      （不是写死「猫不会唱歌」，而是按知识推的）；
#   ③ 不许误伤：直接知道的「猫 会 抓老鼠」仍然是「对」；
#   ④ 前提也会被上位能力继承（唱戏 属于 唱歌 ⟹ 唱戏 也需要声带）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT/build_aip}
MODEL=${MODEL:-$ROOT/model.bin}
WORK="$ROOT/_t/prereq"
rm -rf "$WORK"; mkdir -p "$WORK"
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }

# run <关系库文件> <输入行...>：在 $WORK 里跑（relations.txt 是按 cwd 读的）
run() {
    printf '%s\n' "$1" > "$WORK/relations.txt"
    { printf '::load %s\n::notepad off\n::rel analogy off\n::rel analogy solver off\n' "$MODEL"; shift
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'; } | (cd "$WORK" && "$BIN") 2>&1
}
reps() { sed -n 's/.*AI > //p'; }

KNOW='唱歌 需要 灵活的声带
唱歌 需要 声带
猫 没有 灵活的声带
鱼 没有 声带
'

echo "① 缺前提 → 否定能力（语料里没有这条结论）"
out=$(run "$KNOW" "猫 会 唱歌 吗" "::der")
r=$(printf '%s\n' "$out" | reps | head -1)
case "$r" in *"不 对"*"唱歌"*) chk "推出「猫 不会唱歌」（$r）" 1 ;;
                           *)   chk "推出「猫 不会唱歌」（实得「$r」）" 0 ;;
esac
printf '%s\n' "$out" | grep -aq '缺前提' \
    && chk "链上写明依据是「缺前提」" 1 || chk "链上写明依据是「缺前提」" 0
printf '%s\n' "$out" | grep -aq '缺口=0' \
    && chk "验证器独立复核过（缺口=0）" 1 || chk "验证器独立复核过（缺口=0）" 0
r=$(printf '%s\n' "$(run "$KNOW" "鱼 会 唱歌 吗")" | reps | head -1)
case "$r" in *"不 对"*) chk "换个主语也成立（鱼 没有 声带）（$r）" 1 ;;
                 *)     chk "换个主语也成立（实得「$r」）" 0 ;;
esac

echo "② 结论确实来自数据：把「需要」拿掉就不许再推否定"
out=$(run '猫 有 尾巴
猫 属于 动物
' "猫 会 唱歌 吗")
r=$(printf '%s\n' "$out" | reps | head -1)
case "$r" in *"不 对"*) chk "没有前提知识时**不再**说「不对」（实得「$r」）" 0 ;;
                 *)     chk "没有前提知识时不再说「不对」（$r）" 1 ;;
esac

echo "③ 不许误伤已经直接知道的事实（用仓库里真实的 relations.txt）"
# 注意：$WORK 里的 relations.txt 是**替换**掉的，里面没有「猫 会 抓老鼠」这类
# 事实（它们来自仓库的 relations.txt），所以这一条必须在仓库根目录跑。
run_root() {
    { printf '::load %s\n::notepad off\n::rel analogy off\n::rel analogy solver off\n' "$MODEL"; for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'; } | (cd "$ROOT" && "$BIN") 2>&1
}
out=$(run_root "猫 会 抓老鼠 吗" "猫 会 飞 吗" "猫 会 唱歌 吗")
r=$(printf '%s\n' "$out" | reps | head -1)
case "$r" in *"对"*) chk "直接知道的「猫 会 抓老鼠」仍答对（$r）" 1 ;;
                 *)   chk "直接知道的仍答对（实得「$r」）" 0 ;;
esac
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"不 对"*) chk "直接否定边仍然生效（$r）" 1 ;;
                 *)     chk "直接否定边仍然生效（实得「$r」）" 0 ;;
esac
r=$(printf '%s\n' "$out" | reps | sed -n '3p')
case "$r" in *"不 对"*) chk "同一份知识里，唱歌那条仍然是推出来的（$r）" 1 ;;
                 *)     chk "同一份知识里，唱歌那条仍然是推出来的（实得「$r」）" 0 ;;
esac

echo "④ 前提会被上位能力继承（唱戏 属于 唱歌）"
out=$(run '唱戏 属于 唱歌
唱歌 需要 灵活的声带
猫 没有 灵活的声带
' "猫 会 唱戏 吗")
r=$(printf '%s\n' "$out" | reps | head -1)
case "$r" in *"不 对"*) chk "从上位能力继承前提（$r）" 1 ;;
                 *)     chk "从上位能力继承前提（实得「$r」）" 0 ;;
esac

echo
[ "$fail" -eq 0 ] && echo "PASS prerequisite（缺前提 → 否定能力：能推、靠数据、不误伤、可继承）" \
                  || { echo "FAIL prerequisite"; exit 1; }
