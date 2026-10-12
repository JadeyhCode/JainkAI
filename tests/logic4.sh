#!/bin/sh
# tests/logic4.sh — 上一轮列的四个局限，逐个看护
#
#   ① 只支持单条假设         → 现在是**栈式多层**，后一个「如果」叠上去而不是替换
#   ② 缺完备性（推不出否定） → 加了**独占公理**（可健全地推出否定）+ 开放/闭合世界开关
#   ③ 假设不参与检索/帧层     → 假设开着时，关系问题**不会被看不见假设的层悄悄接管**
#   ④ 没有嵌套假设           → 每层可以归属某个人（「如果 他 以为 …」）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }
run() {
    { printf '::load %s\n::rel analogy off\n::rel analogy solver off\n::notepad off\n::rel analogy off\n::rel analogy solver off\n' "$MODEL"
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'
    } | "$BIN" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }

echo "① 多层假设：叠加而不是替换"
out=$(run "如果 猫 属于 鸟" "猫 会 飞 吗" "如果 猫 属于 鱼" "猫 会 游泳 吗" "猫 会 飞 吗" "::hypo")
printf '%s\n' "$out" | grep -a -E 'AI >|第 . 层' | sed 's/^你 > /     /'
n=$(printf '%s\n' "$out" | grep -ac '当前假设世界：2 层' || true)
[ "$n" -eq 1 ] && chk "两条「如果」共存（2 层）" 1 || chk "两条「如果」共存（2 层）" 0
r5=$(printf '%s\n' "$out" | reps | sed -n 5p)
case "$r5" in *"猫 -[属于·假设]-> 鸟"*) chk "第 1 层的假设仍然算数（叠加不是替换）" 1 ;;
             *) chk "第 1 层的假设仍然算数（实得「$r5」）" 0 ;; esac
# 「算了」弹一层
out=$(run "如果 猫 属于 鸟" "如果 猫 属于 鱼" "算了" "::hypo")
printf '%s\n' "$out" | grep -aq '当前假设世界：1 层' \
    && chk "「算了」弹掉最深一层（还剩 1 层）" 1 || chk "「算了」弹掉最深一层" 0

echo "② 独占公理：健全地推出否定（不靠「没写就算假」）"
out=$(run "猫 属于 植物 吗" "猫 属于 鸟 吗" "猫 在 天上 吗")
printf '%s\n' "$out" | grep -a -E 'AI >' | sed 's/^你 > /     /'
r1=$(printf '%s\n' "$out" | reps | sed -n 1p)
r2=$(printf '%s\n' "$out" | reps | sed -n 2p)
r3=$(printf '%s\n' "$out" | reps | sed -n 3p)
case "$r1" in *"不 对"*"独占"*) chk "猫 属于 植物 → 否定（链上标明「独占」）" 1 ;;
             *) chk "猫 属于 植物 → 否定（实得「$r1」）" 0 ;; esac
case "$r3" in *"不 对"*) chk "猫 在 天上 → 否定（「在」也是独占的）" 1 ;;
             *) chk "猫 在 天上 → 否定（实得「$r3」）" 0 ;; esac
# 关键：鸟 ⊂ 动物，所以「猫 是 鸟」**不能**否定 —— 这是健全性的体现
case "$r2" in *"不 太 懂"*|*"不 确定"*) chk "猫 属于 鸟 → 仍说不知道（鸟⊂动物，否定不健全）" 1 ;;
             *) chk "猫 属于 鸟 → 应说不知道（实得「$r2」）" 0 ;; esac
# 开放/闭合世界
out=$(run "::world closed" "猫 属于 鸟 吗" "::world open" "猫 属于 鸟 吗")
r_closed=$(printf '%s\n' "$out" | reps | sed -n 1p)
r_open=$(printf '%s\n' "$out" | reps | sed -n 2p)
case "$r_closed" in *"不 对"*"闭合世界"*) chk "闭合世界下答否定，且链上标明「闭合世界」" 1 ;;
                  *) chk "闭合世界下答否定（实得「$r_closed」）" 0 ;; esac
case "$r_open" in *"不 太 懂"*|*"不 确定"*) chk "切回开放世界又说不知道" 1 ;;
               *) chk "切回开放世界又说不知道（实得「$r_open」）" 0 ;; esac

echo "③ 假设开着时，推不出来要说出来 —— 不许被看不见假设的层接管"
# 探针换过：`猫 会 唱歌` 现在由「缺前提」规则推出来了（唱歌 需要 灵活的声带，
# 猫 没有 灵活的声带 ⟹ 不 会），它不再是「推不出来」的例子，见 tests/prerequisite.sh。
# 这里用知识库里确实没有的「跳舞」，才测得到「假设开着时不许被看不见假设的层接管」。
out_no=$(run "猫 会 跳舞 吗")
out_hy=$(run "如果 猫 会 飞" "猫 会 跳舞 吗")
rno=$(printf '%s\n' "$out_no" | reps | head -1)
rhy=$(printf '%s\n' "$out_hy" | reps | tail -1)
echo "     无假设 → $rno"
echo "     有假设 → $rhy"
case "$rhy" in *"在 假设 下"*"推不出来"*) chk "有假设时明说「在假设下我推不出来」" 1 ;;
             *) chk "有假设时明说「在假设下我推不出来」（实得「$rhy」）" 0 ;; esac
[ "$rhy" != "$rno" ] && chk "与无假设时的答复不同（没被静默接管）" 1 \
                     || chk "与无假设时的答复不同" 0
# 理由要写清楚，且不能截断成乱码
out=$(run "如果 猫 会 飞" "猫 会 跳舞 吗" "::der")
printf '%s\n' "$out" | grep -aq '需要完备性' \
    && chk "链上写清了为什么推不出来（需要完备性）" 1 || chk "链上写清了为什么推不出来" 0
printf '%s\n' "$out" | grep -aq '我的知识是开放的' \
    && chk "长理由没被截断（note 够大）" 1 || chk "长理由没被截断" 0

echo "④ 嵌套假设 + 归属（「如果 他 以为 …」）"
out=$(run "如果 猫 属于 鸟" "如果 他 以为 猫 会 飞" "::hypo" "猫 会 飞 吗")
printf '%s\n' "$out" | grep -a -E '第 . 层|以为|AI >' | sed 's/^你 > /     /'
printf '%s\n' "$out" | grep -aq '这一层算在「他」头上' \
    && chk "第二层标成「算在他头上」" 1 || chk "第二层标成「算在他头上」" 0
printf '%s\n' "$out" | grep -aq '他 以为： 猫 会 飞' \
    && chk "列假设时写出了「他 以为：猫 会 飞」" 1 || chk "列假设时写出了归属" 0
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"在 假设 下"*) chk "嵌套两层时推理仍然生效" 1 ;;
             *) chk "嵌套两层时推理仍然生效（实得「$r」）" 0 ;; esac
# 嵌套也不能漏进图：存盘 → 重新加载 → 假设必须没了
TMP=./_logic4_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
{ printf '::load %s\n::rel analogy off\n::rel analogy solver off\n如果 他 以为 猫 会 飞\n::save %s/m.bin\n::load %s/m.bin\n::rel analogy off\n::rel analogy solver off\n猫 会 飞 吗\n::quit\n' \
      "$MODEL" "$TMP" "$TMP"; } | "$BIN" 2>&1 | sed -n 's/.*AI > //p' > "$TMP/p.txt"
r=$(tail -1 "$TMP/p.txt")
rm -rf "$TMP"
case "$r" in *"不 对"*) chk "嵌套假设同样不进图（存盘重载后就没了）" 1 ;;
             *) chk "嵌套假设同样不进图（实得「$r」）" 0 ;; esac

echo
[ "$fail" -eq 0 ] && echo "PASS logic4（多层 / 独占健全否定 / 假设不被接管 / 嵌套+归属）" \
                  || { echo "FAIL logic4"; exit 1; }
