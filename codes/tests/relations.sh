#!/bin/sh
# tests/relations.sh — L2 语义关系层的回归
#
# 看护四件事：
#   ① 静态知识 + 传递推理：`猫 有 毛 吗` 靠 猫→动物→毛 推出来，
#      而且**把推导路径打出来**（这才是「明确思维链」）
#   ② 对话中动态学知识：用户陈述一句事实就当场学进图里，之后能用来回答，
#      并且**存盘再读还在**（rel 位掩码写进了模型 v6）
#   ③ 闭合世界的「不是」只对知识库里的实体说，不许抢答普通问句
#   ④ 不误学：疑问句（你 是 谁）不当事实记下来
#   ⑥ 合写否定（不会/不喜欢/不是）与两词写法（不 会）走**同一份声明**、同答案
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }
[ -f relations.txt ] || { echo "FAIL: 缺 relations.txt"; exit 1; }

TMP=./_rel_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi }

run() {   # <输入...>  → 打印回复行
    { printf '::load %s\n::notepad off\n' "$MODEL"
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'
    } | "$BIN" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }
nth()  { sed -n "${1}p"; }

echo "① 静态知识 + 多跳推理（带推导路径）"
out=$(run "猫 有 毛 吗" "猫 属于 生物 吗" "鸟 会 飞 吗" "猫 吃 鱼 吗")
r1=$(printf '%s\n' "$out" | reps | nth 1)
r2=$(printf '%s\n' "$out" | reps | nth 2)
r3=$(printf '%s\n' "$out" | reps | nth 3)
r4=$(printf '%s\n' "$out" | reps | nth 4)
echo "     猫 有 毛 吗      → $r1"
echo "     猫 属于 生物 吗  → $r2"
echo "     鸟 会 飞 吗      → $r3"
echo "     猫 吃 鱼 吗      → $r4"
case "$r1" in *"猫 -[属于·继承]-> 动物 ； 动物 -[有]-> 毛"*) chk "猫 有 毛：多跳推出且打了**带规则名**的链" 1 ;;
             *) chk "猫 有 毛：多跳推出且打了带规则名的链（实得「$r1」）" 0 ;; esac
case "$r2" in *"猫 -[属于·传递]-> 动物 ； 动物 -[属于]-> 生物"*) chk "猫 属于 生物：传递推理且标了「传递」" 1 ;;
             *) chk "猫 属于 生物：传递推理且标了「传递」（实得「$r2」）" 0 ;; esac
case "$r3" in "对 ， 鸟 -[会]-> 飞"*) chk "鸟 会 飞：直接事实" 1 ;;
             *) chk "鸟 会 飞：直接事实（实得「$r3」）" 0 ;; esac
case "$r4" in "对 ， 猫 -[吃]-> 鱼"*) chk "猫 吃 鱼：直接事实" 1 ;;
             *) chk "猫 吃 鱼：直接事实（实得「$r4」）" 0 ;; esac

echo "② 闭合世界：知识库里的实体才答「不是」"
out=$(run "猫 会 飞 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in "不 对"*"猫 -[不 会]-> 飞"*) chk "猫 会 飞 吗 → 不 对，且给出否定依据（$r）" 1 ;;
             *) chk "猫 会 飞 吗 → 不 对，且给出否定依据（实得「$r」）" 0 ;; esac
# 普通问句不许被关系层抢答
out=$(run "你 会 游泳 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in "我 会 试 着 游泳") chk "你 会 游泳 吗：没被关系层抢答（$r）" 1 ;;
             *) chk "你 会 游泳 吗：没被关系层抢答（实得「$r」）" 0 ;; esac

echo "③ 对话中动态学知识，并且**存盘再读还在**"
out=$(run "猫 属于 宠物" "猫 属于 宠物 吗")
r1=$(printf '%s\n' "$out" | reps | nth 1)
r2=$(printf '%s\n' "$out" | reps | nth 2)
echo "     陈述 → $r1"
echo "     追问 → $r2"
case "$r1" in "记下 了 ： 猫 属于 宠物") chk "陈述句被当场学下来（$r1）" 1 ;;
             *) chk "陈述句被当场学下来（实得「$r1」）" 0 ;; esac
case "$r2" in "对 ， 猫 -[属于]-> 宠物"*) chk "刚学的知识立刻能用来回答（$r2）" 1 ;;
             *) chk "刚学的知识立刻能用来回答（实得「$r2」）" 0 ;; esac
# 存盘 → 重载 → 还在吗？（rel 位掩码写进模型 v6）
{ printf '::load %s\n猫 属于 宠物\n::save %s/m.bin\n::load %s/m.bin\n猫 属于 宠物 吗\n::quit\n' \
      "$MODEL" "$TMP" "$TMP"; } | "$BIN" 2>&1 | sed -n 's/.*AI > //p' > "$TMP/persist.txt"
last=$(tail -1 "$TMP/persist.txt")
case "$last" in "对 ， 猫 -[属于]-> 宠物"*) chk "存盘重载后动态知识还在（$last）" 1 ;;
              *) chk "存盘重载后动态知识还在（实得「$last」）" 0 ;; esac

echo "④ 不误学：疑问句不当事实"
out=$(run "你 是 谁")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"记下 了"*) chk "「你 是 谁」不该被当成事实记下" 0 ;;
             *) chk "「你 是 谁」没被当成事实记下（现答：$r）" 1 ;; esac
out=$(run "猫 属于 动物")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in "我 知道 ： 猫 属于 动物") chk "已知事实回「我 知道」而不是重复学（$r）" 1 ;;
             *) chk "已知事实回「我 知道」（实得「$r」）" 0 ;; esac

echo "⑤ ::why 打出推导，::rel 列出知识，::relinfer off 可关"
out=$(run "::why 猫 有 毛" "::rel 猫")
printf '%s\n' "$out" | grep -q '猫 -\[属于·继承\]-> 动物' \
    && chk "::why 打出带规则名的推导路径" 1 || chk "::why 打出带规则名的推导路径" 0
printf '%s\n' "$out" | grep -q -- '--\[属于\]--> 动物' \
    && chk "::rel 列出关系知识" 1 || chk "::rel 列出关系知识" 0
out=$(run "::relinfer off" "猫 有 毛 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"对 ， 猫 -[属于·继承]-> 动物 ； 动物 -[有]-> 毛"*) chk "::relinfer off 时不再推理" 0 ;;
             *) chk "::relinfer off 时不再推理（现答：$r）" 1 ;; esac

echo "== ⑥ 合写否定（不会/不喜欢/不是）：同一份声明的**两种写法**都要认 =="
# 关系表声明的是两词形式（`不 会`），但分词器把最常见的「不会」切成**一个** token，
# 而「不属于/不能/不敢」却是两词 —— 修之前合写的整条掉进降级答复（audit §6.69）。
out=$(run "猫 不会 飞 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"不 太 懂"*|*"不 太 懂"*) chk "合写「不会」不再掉进降级答复（现答：$r）" 0 ;;
             *) chk "合写「不会」认出来了（$r）" 1 ;; esac
printf '%s\n' "$out" | grep -aq '对 ， 猫 -\[不 会\]-> 飞' \
    && chk "而且答对了（猫 不会 飞 → 对）" 1 || chk "答案（实得：$r）" 0
# 两种写法必须**同答案**（同一条声明，两个切法）
a=$(run "猫 不会 飞 吗" | reps | nth 1)
b=$(run "猫 不 会 飞 吗" | reps | nth 1)
[ "$a" = "$b" ] && chk "合写与两词写法**同答案**（$a）" 1 \
                || chk "两种写法不一致（合写=$a / 两词=$b）" 0
out=$(run "猫 不是 动物 吗")
printf '%s\n' "$out" | grep -aq '猫 -\[属于\]-> 动物' \
    && chk "「不是」也认（按 属于 的否定走）" 1 \
    || chk "不是（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0
# 归属：合写否定说给他听时，存进**他名下**（并且不把我自己的知识改掉）
out=$(run "甲 说 猫 不会 游泳" "::others")
printf '%s\n' "$out" | grep -aq '记下 了 ： 甲 说 猫 不 会 游泳' \
    && chk "合写否定进他的信念表，而且**不双重否定**（甲 说 猫 不 会 游泳）" 1 \
    || chk "归属（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0
# 肯定句不受影响（别把关系词本身当否定）
out=$(run "猫 会 飞 吗")
printf '%s\n' "$out" | grep -aq '猫 -\[不 会\]-> 飞' \
    && chk "肯定句照旧（猫 会 飞 吗 → 依据仍是「不 会」那条否定边）" 1 \
    || chk "肯定句（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0


rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS relations（L2：静态知识/传递推理/对话中现学/持久化/不误学/合写否定）" \
                  || { echo "FAIL relations"; exit 1; }
