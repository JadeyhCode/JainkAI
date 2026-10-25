#!/bin/sh
# tests/e2e_session.sh — **真实用户会话验收**（§6.606）：一晚上教/问/改口/存读的完整戏
#
# 为什么单独一套：其它套件都是"一个特性一个断言"（tokenop 382 项、learnprobe 68 项…）。
# 但用户不会按特性用；他要的是"我教它、它记住、我说不对它改、我关掉再打开它还记得"。
# 这一套就用**一段像人说话一样的剧本**走一遍，逐步断言；任何一环断了都算"学习不可用"。
#
# 剧本（一个真实的学习之夜）：
#   ① 先问它不会的（应该说不知道，不能编）
#   ② 用最自然的话教两条（带标点、带引导语）
#   ③ 立刻回问（该答对）
#   ④ 问"你怎么知道"（该说是我教的）
#   ⑤ 说「不对」改口（该改过来，并说旧的那条还在、可以撤）
#   ⑥ 撤掉旧值后回问（该答不对）
#   ⑦ 教一条会与既有知识冲突的（该提示冲突，给裁决建议）
#   ⑧ 存盘、退出、重开：学会的两条还在；"你怎么知道"仍认得出
#   ⑨ 中途切脑：新脑不知道旧脑的事；切回来还记得
#
# 用法：sh tests/e2e_session.sh [模型文件]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/e2e_session"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.base
for f in patterns.txt frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# 一轮 = 一个进程：`run <脚本> <输出文件>`（前置固定关掉记事本/逐字，钉住流水线模式）
run() { cp model.base m.bin; { printf '::notepad off\n::char off\n'; printf '%b' "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }
# 第 N 条 AI 答复（`AI > ` 之后那行）
reply() { grep -a 'AI > ' "$1" | sed -n "${2}p" | sed 's/.*AI > //'; }

echo "== ① 先问不会的：不能编 =="
run '凤凰 属于 鸟类 吗\n' o1
r=$(reply o1 1)
case "$r" in *"不 知道"*|*"不 会"*|*"没 学 过"*|*"不 太 懂"*) ok "没教过的说「不知道」（没编）: $r" ;;
  *) bad "①不知道" "答复：$r" ;; esac

echo "== ② 用最自然的话教两条（带标点 + 带引导语）=="
run '我 教 你 ， 麒麟 属于 神兽\n还有 ， 凤凰，属于 鸟类\n' o2
r1=$(reply o2 1); r2=$(reply o2 2)
case "$r1" in *"记下"*) ok "引导语 + 逗号能教进去：$r1" ;; *) bad "②教-1" "$r1" ;; esac
case "$r2" in *"记下"*) ok "人名后逗号也能教进去：$r2" ;; *) bad "②教-2" "$r2" ;; esac

echo "== ③ 立刻回问：该答对 =="
run '我 教 你 ， 麒麟 属于 神兽\n麒麟 属于 神兽 吗\n' o3
r=$(reply o3 2)
case "$r" in *"对"*) ok "教完立刻问 ⇒ 对：$r" ;; *) bad "③回问" "$r" ;; esac

echo "== ④ 问出处：该说是我教的 =="
run '我 教 你 ， 麒麟 属于 神兽\n你 怎么 知道 麒麟 属于 神兽\n' o4
r=$(reply o4 2)
case "$r" in *"你 教"*|*"教学"*|*"你 告诉"*) ok "出处可以说清（是我教的）：$r" ;;
  *) bad "④出处" "$r" ;; esac

echo "== ⑤ 说「不对」：该改过来，并交代旧值 =="
run '我 教 你 ， 麒麟 属于 神兽\n不对 ， 麒麟 不 属于 神兽\n' o5
r=$(reply o5 2)
case "$r" in *"改 过来"*|*"改口"*) ok "「不对」⇒ 改过来：$r" ;; *) bad "⑤改口" "$r" ;; esac

echo "== ⑥ 改口之后回问：该答不对 =="
run '我 教 你 ， 麒麟 属于 神兽\n不对 ， 麒麟 不 属于 神兽\n麒麟 属于 神兽 吗\n' o6
r=$(reply o6 3)
case "$r" in *"不 对"*|*"不 是"*|*"不 属于"*) ok "撤了就答不对：$r" ;; *) bad "⑥撤后回问" "$r" ;; esac

echo "== ⑦ 教一条会冲突的：该提示冲突 =="
run '::askconflict on\n我 教 你 ， 猫 属于 植物\n' o7
r=$(reply o7 1)
case "$r" in *"要 撤"*|*"改 掉"*|*"冲突"*|*"还 记 着"*) ok "冲突当场说清：$r" ;; *) bad "⑦冲突提示" "$r" ;; esac

echo "== ⑧ 存盘 → 重开：学到的还在、出处还认得出 =="
run '我 教 你 ， 麒麟 属于 神兽\n凤凰，属于 鸟类\n::save m.bin\n' o8
{ printf '::notepad off\n::char off\n麒麟 属于 神兽 吗\n凤凰 属于 鸟类 吗\n你 怎么 知道 麒麟 属于 神兽\n'; } | ./aip --model m.bin > o9 2>&1 || true
r1=$(reply o9 1); r2=$(reply o9 2); r3=$(reply o9 3)
case "$r1" in *"对"*) ok "重开之后第一条还在：$r1" ;; *) bad "⑧-1" "$r1" ;; esac
case "$r2" in *"对"*) ok "重开之后第二条还在：$r2" ;; *) bad "⑧-2" "$r2" ;; esac
case "$r3" in *"你 教"*|*"教学"*) ok "重开之后出处仍认得出：$r3" ;; *) bad "⑧-3" "$r3" ;; esac

echo "== ⑨ 切脑：新脑不知道旧脑的，切回来还记得 =="
{ printf '::notepad off\n::char off\n'; printf '我 教 你 ， 麒麟 属于 神兽\n::brain new\n麒麟 属于 神兽 吗\n::brain use 0\n麒麟 属于 神兽 吗\n'; } | ./aip --model model.base > o10 2>&1 || true
r1=$(reply o10 1); r2=$(reply o10 2); r3=$(reply o10 3)
case "$r2" in *"不 知道"*|*"不 会"*|*"没 学 过"*|*"不 太 懂"*) ok "新脑不知道旧脑学的事：$r2" ;;
  *) bad "⑨-新脑" "$r2" ;; esac
case "$r3" in *"对"*) ok "切回旧脑还记得：$r3" ;; *) bad "⑨-切回" "$r3" ;; esac

echo
echo "真实用户会话：通过 $pass / 失败 $fail"
[ "$fail" -eq 0 ] && echo "PASS e2e_session（教→问→改口→存读→切脑 这一整条戏能走完）" \
                  || echo "FAIL e2e_session（$fail 项）"
[ "$fail" -eq 0 ] || exit 1
exit 0
