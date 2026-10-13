#!/bin/sh
# tests/revision.sh — 观测改口语义：**后说的观测替换先说的**
#
# 这一套修的是 docs/thinking.md 第十一节那条已知局限：
#   用户先说「猫 有 毛」、后说「猫 没有 毛」→ 同一对词上两个极性**并存**，
#   于是每次提问都报「关于猫有毛这件事，我这儿有互相矛盾的依据」——
#   用户教的修正留不下来，而且听起来像系统自己知识有毛病（其实是我没改口）。
#
# 现在：落观测时把**同一条边上的对立极性**改掉（边还在、改的是极性 ——
# 「关系边不许删」那条铁律没松），并且在回答里说明「我按后来那句改了」。
#
# 这一套本身就是 **A/B**：同一段对话跑两遍，只差 `::revise on|off`。
#   ① 默认（开）：回答跟着**后说**的那句走，不报矛盾，且说「改过来了」
#   ② 关（回到旧语义）：两个极性并存 → 报「互相矛盾的依据」——差异被真实量出来
#   ③ 不误伤：不同宾语的多值事实各自并存（猫 有 毛 / 猫 有 尾巴，两条都在）
#   ④ 不误伤「观测压过缓存」：那条路不算改口（确认话不变）
#   ⑤ 存读：改口之后存盘重载，还是只有后来那个极性（不冒充两条都真）
#
# 用法：sh tests/revision.sh [模型文件]        默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/revise"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# 每次从干净模型起跑（改口会写图，前一段不能污染后一段）
# `::notepad off`：**钉住"流水线模式"** —— 这样环境变量强制打开"AI 自己写草稿"时
# （`AIP_PATCH=记事本 AIP_NOTEPAD=on`），这套看护的断言依然测的是它要测的东西。
run() { cp model.bin m.bin; { printf '::notepad off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '%b' "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 默认（开）：后说的替换先说的，并如实说「改过来了」 =="
run '猫 有 毛\n猫 没有 毛\n猫 有 毛 吗\n::rel 猫\n' o1
grep -q '改 过来 了' o1 \
    && ok "第二次说反时说了「改过来了」（用户知道自己的修正生效了）" \
    || bad "改口提示" "$(grep 'AI >' o1 | tail -2)"
grep -q '猫 --\[不 有\]--> 毛$' o1 \
    && ok "图上只剩后来那个极性（不 有）" || bad "极性替换" "$(grep '毛' o1)"
if grep -q '猫 --\[有\]--> 毛$' o1; then
    bad "先说的那个极性还留在图上（两个极性并存）" "$(grep '毛' o1)"
else
    ok "先说的极性被改掉了（不是并存）"
fi
grep -q '不 对 ， 猫 -\[不 有\]-> 毛' o1 \
    && ok "回答跟着后说的走" || bad "回答" "$(grep 'AI >' o1 | tail -1)"
if grep -q '互相矛盾的依据' o1; then
    bad "改口被误报成「知识自相矛盾」" "$(grep '矛盾' o1)"
else
    ok "不报矛盾（改口不是自相矛盾）"
fi
grep -q '::revise' o1 || true

echo "== ② 关（旧语义）：两个极性并存 → 报矛盾（差异被量出来） =="
run '::revise off\n猫 有 毛\n猫 没有 毛\n猫 有 毛 吗\n::rel 猫\n' o2
grep -q '互相矛盾的依据' o2 \
    && ok "关掉之后回到旧语义：两个极性并存、报「互相矛盾的依据」" \
    || bad "旧语义" "$(grep 'AI >' o2 | tail -1)"
grep -q '猫 --\[有\]--> 毛$' o2 && grep -q '猫 --\[不 有\]--> 毛$' o2 \
    && ok "旧语义下两个极性确实并存（A/B 的对照组成立）" \
    || bad "对照组" "$(grep '毛' o2)"
if grep -q '改 过来 了' o2; then
    bad "关掉之后不该再说「改过来了」" "$(grep '改 过来' o2)"
else
    ok "关掉之后没有改口提示（开关真的在管）"
fi

echo "== ③ 不误伤：不同宾语的多值事实各自并存 =="
run '猫 有 毛\n猫 有 尾巴\n::rel 猫\n' o3
grep -q '猫 --\[有\]--> 毛$' o3 && grep -q '猫 --\[有\]--> 尾巴$' o3 \
    && ok "「猫 有 毛」与「猫 有 尾巴」并存（改口只针对同一对词）" \
    || bad "多值不误伤" "$(grep '有' o3)"
if grep -q '改 过来 了' o3; then
    bad "不同宾语也说「改过来了」（改口范围过宽）" "$(grep '改 过来' o3)"
else
    ok "不同宾语不算改口"
fi

echo "== ④ 不误伤「观测压过缓存」：那条路不是改口 =="
# 先让它**推出来并沉淀**（缓存），再用观测压掉 —— 确认话还是原来那句。
run '猫 有 毛 吗\n猫 没有 毛\n' o4
grep -q '记下 了 ： 猫 没有 毛' o4 \
    && ok "观测压过缓存时用的是「记下了」（不是改口）" || bad "缓存路径措辞" "$(grep 'AI >' o4 | tail -1)"
if grep -q '改 过来 了' o4; then
    bad "把「观测压过缓存」误报成改口" "$(grep '改 过来' o4)"
else
    ok "缓存路径没有改口提示（两件事分开了）"
fi

echo "== ⑤ 改口之后存读：重载还是只有后来那个极性 =="
run '猫 有 毛\n猫 没有 毛\n::save slot\n' o5
[ -s slot ] && ok "存盘成功" || bad "存盘" "$(tail -2 o5)"
mv slot m.bin
printf '::rel 猫\n::quit\n' | ./aip --model m.bin > o6 2>&1 || true
grep -q '猫 --\[不 有\]--> 毛$' o6 \
    && ok "重载后仍是后来那个极性" || bad "存读" "$(grep '毛' o6)"
if grep -q '猫 --\[有\]--> 毛$' o6; then
    bad "重载后先说的极性又回来了（改口没落盘）" "$(grep '毛' o6)"
else
    ok "先说的极性没有复活"
fi

echo "== ⑥ 装载边界：改口存盘后，重启不会被 relations.txt 改回去 =="
# 用户教的和基准知识（relations.txt 里的「猫 属于 动物」）相反 → 存盘、重载。
# 模型文件是**更晚**的状态（用户教过），基准知识每次启动重读 ——
# 让它把用户教的改回去，等于「重启就把用户教的忘了」。
run '猫 不 属于 动物\n::save slot2\n' o7
[ -s slot2 ] && ok "存盘成功" || bad "存盘" "$(tail -2 o7)"
mv slot2 m.bin
printf '::rel 猫\n::quit\n' | ./aip --model m.bin > o8 2>&1 || true
grep -q '猫 --\[不 属于\]--> 动物$' o8 \
    && ok "重载后用户教的否定还在" || bad "装载边界" "$(grep '属于' o8 | head -3)"
if grep -q '猫 --\[属于\]--> 动物$' o8; then
    bad "relations.txt 把用户教过的改回去了（重启就忘）" "$(grep '属于' o8 | head -3)"
else
    ok "基准知识给存盘观测让了路"
fi

echo "== ⑦ A/B 对照表（同一段对话，只差一个开关） =="
n_on=$(grep -c '互相矛盾的依据' o1 || true)
n_off=$(grep -c '互相矛盾的依据' o2 || true)
echo "   ::revise 开 → 矛盾 $n_on 次；关 → 矛盾 $n_off 次"
[ "${n_on:-0}" -eq 0 ] && [ "${n_off:-0}" -ge 1 ] \
    && ok "开关确实改变了行为（不是空转的开关）" \
    || bad "A/B 没差异" "开 $n_on / 关 $n_off"

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS revision（观测改口语义：后说的替换先说的，$pass 项）"
else
    echo "FAIL revision（$fail 项失败，$pass 项通过）"
    exit 1
fi
