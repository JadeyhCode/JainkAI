#!/bin/sh
# tests/biases.sh — **反糟粕**（CLI 级）：人的思考原理要学，人的偏误不许学
#
# 配套 `tools/biastest.c`（C 级，35 项）与 `docs/人思考的原理-采用与不采用.md`。
# CLI 这一层盯的是三件在**真模型**上看得见的事：
#
#   ① 因果猜想：一次时序相邻 = 「暂时」，反复协变才升格成「可信的猜想」，
#      而且**永远是猜想**（`[因果]` 标记、状态可见、不进关系表）——
#      反的是「一次巧合当规律」（post hoc 迷信）。
#   ② 反虚构：不知道就带保留地说 / 挂成未解问题，不许把猜的讲成事实。
#   ③ 反从众：别人说的进他的信念表，**不进我的关系表**。
#
# 用法：sh tests/biases.sh [模型文件]        默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/biases"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o aip "$ROOT/main.c" -lm
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
run() { cp model.bin m.bin; printf "$1" | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 因果猜想：一次=暂时，协变才升格，且永远是猜想 =="
# 「我 知道 ： 猫 吃 鱼」是同一输入下的稳定答复，所以 (我的答复词 → 你的输入词)
# 这一对会**反复协变** —— 正好用来量「一次 vs 多次」的差别。
run '猫 吃 鱼\n猫 吃 鱼\n::edges 我\n' o1
grep -q '语境\[\[因果\]\]' o1 \
    && ok "因果猜想带 [因果] 标记（可辨识，不是知识）" || bad "因果标记" "$(grep '→' o1 | tail -3)"
grep -qE '→ 猫 +hits=1 +seq=[0-9]+ +角色=0 状态=1 +语境\[\[因果\]\]' o1 \
    && ok "只见一次：hits=1、状态=1（暂时）" \
    || bad "一次巧合是暂时的" "$(grep '因果' o1 | head -3)"

run '猫 吃 鱼\n猫 吃 鱼\n猫 吃 鱼\n猫 吃 鱼\n::edges 我\n::stats\n' o2
grep -qE '→ 猫 +hits=3 +seq=[0-9]+ +角色=0 状态=0 +语境\[\[因果\]\]' o2 \
    && ok "协变到第 3 次：升格成「可信的猜想」（状态=0）" \
    || bad "协变升格" "$(grep '因果' o2 | head -3)"
grep -qE '因果=[0-9]+\(升格[1-9]' o2 \
    && ok "::stats 里报得出「因果=N(升格M)」" || bad "因果计数" "$(grep '因果=' o2 | head -1)"
# 反迷信的最后一道闸：猜想**不许**变成关系知识
run '猫 吃 鱼\n猫 吃 鱼\n猫 吃 鱼\n猫 吃 鱼\n::rel 我\n' o3
if grep -q -- '--\[吃\]-->' o3; then
    bad "因果猜想漏进了关系表（那就成「我说过就算事实」了）" "$(grep '吃' o3 | head -3)"
else
    ok "反复出现的猜想**没有**变成关系知识（它只是猜想）"
fi

echo "== ② 反虚构：不知道就带保留地说，不硬凑断言 =="
run '猫 会 跳舞 吗\n::delib\n' o4
r=$(sed -n 's/.*AI > //p' o4 | tail -1)
if printf '%s\n' "$r" | grep -qE '对 *(，|,) *猫 *-\['; then
    bad "把没有依据的事说成了断言" "$r"
else
    ok "没有把「没有依据」说成断言（实得「$r」）"
fi
if printf '%s\n' "$r" | grep -qE '应该|大概|可能|不 太 懂|不 知道'; then
    ok "答复带着保留/降级（不像知道那样说）"
else
    bad "答复没有保留语气" "$r"
fi
grep -q '互 相 矛 盾' o4 && bad "把「不知道」说成了矛盾" "$(grep '矛盾' o4 | head -2)" || true
if grep -q '停因=有结论' o4; then
    bad "思维层说「有结论」，可它其实没有依据" "$(grep '停因' o4 | head -2)"
else
    ok "思维层的停因不是「有结论」（它没装懂）"
fi

echo "== ③ 反从众：他的看法不进我的关系表 =="
run '他 以为 猫 会 飞\n::rel 猫\n::others\n' o5
grep -q '猫 --\[会\]--> 飞' o5 \
    && bad "他的看法进了**我的**关系图" "$(grep '会' o5 | head -3)" \
    || ok "「他认为 猫 会 飞」没有变成我的关系边"
grep -q '他' o5 && ok "他的看法挂在他名下（::others 看得到）" \
                 || bad "他者表" "$(grep -A3 '他者' o5 | head -5)"

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS biases（反糟粕：因果要协变 / 不知道不说成知道 / 别人的看法不是我的知识，$pass 项）"
else
    echo "FAIL biases（$fail 项失败，$pass 项通过）"
    exit 1
fi
