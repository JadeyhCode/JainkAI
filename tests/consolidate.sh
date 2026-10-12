#!/bin/sh
# tests/consolidate.sh — 推理沉淀：想通的东西留下来，被推翻就撤
#
# 这一套盯的是「越用越懂」那条链，它跨了四层（rel / derive / think / micro）：
#
#   ① 推导（健全规则）→ **沉淀**成一条缓存（边上标着 ASSOC_ROLE_DERIVED）
#   ② 下次直接用缓存 —— 不再从头推（「依据强度：推导缓存」）
#   ③ **观测压过缓存**：用户说的具体事实一来，缓存整条撤掉（具体优先于一般）
#   ④ **现场推理推翻缓存**：另一条规则算出反面 → 听现场的、当场改口、撤销缓存
#   ⑤ 两条继承路径真打架时，老实报「互相矛盾的依据」，不硬选一边
#   ⑥ 缓存随图存盘，重载之后仍然标着「推出来的」（不冒充观测）
#   ⑦ `::thinkcache off` 之后不再沉淀
#
# 用法：sh tests/consolidate.sh [模型文件]      默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/cons"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# 每次都从干净模型起跑（沉淀是**会写图**的，前一段不能污染后一段）
# `::notepad off`：**钉住"流水线模式"** —— 这样环境变量强制打开"AI 自己写草稿"时
# （`AIP_PATCH=记事本 AIP_NOTEPAD=on`），这套看护的断言依然测的是它要测的东西。
run() { cp model.bin m.bin; { printf '::notepad off\n::rel analogy off\n::rel analogy solver off\n'; printf '%b' "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 推导 → 沉淀（标成「推出来的」，不冒充观测） =="
run '猫 有 毛 吗\n::rel 猫\n::mod\n::delib\n' o1
grep -q '猫 --\[有\]--> 毛（推出来的，可撤）' o1 \
    && ok "猫 有 毛 被沉淀成一条「推出来的」缓存" || bad "沉淀标记" "$(grep '毛' o1)"
grep -q '沉淀 猫 rel[0-9]* 毛（沉淀） \[思维:沉淀\]' o1 \
    && ok "沉淀走的是意图队列（审计里看得见来源）" || bad "受审通道" "$(grep -A4 '最近应用' o1)"
grep -q '沉淀 1' o1 && ok "微内核单独记了沉淀数" || bad "沉淀计数" "$(grep '应用 ' o1)"
grep -q '推理沉淀：记住 1 条' o1 && ok "思维层记了「记住 1 条」" || bad "思维计数" "$(grep '推理沉淀' o1)"

echo "== ② 复用：下次直接用缓存，不再从头推 =="
run '猫 有 毛 吗\n猫 有 毛 吗\n::delib\n' o2
grep -q '猫 -\[有·记\]-> 毛' o2 && ok "第二次的链上是「·记」（走缓存）" || bad "复用" "$(grep 'AI >' o2)"
grep -q '依据强度：推导缓存' o2 && ok "依据强度标明是缓存（不是观测）" || bad "强度" "$(grep '依据强度' o2)"

echo "== ③ 观测压过缓存（具体优先于一般） =="
run '猫 有 毛 吗\n猫 没有 毛\n::rel 猫\n猫 有 毛 吗\n' o3
grep -q '猫 --\[不 有\]--> 毛$' o3 && ok "观测进来之后那条边不再是缓存" || bad "观测赢" "$(grep '毛' o3)"
grep -q '不 对 ， 猫 -\[不 有\]-> 毛' o3 && ok "回答跟着观测走" || bad "回答" "$(grep 'AI >' o3 | tail -1)"
if grep -q '互相矛盾的依据' o3; then
    bad "不该报矛盾（具体事实已经压过一般规则）" "$(grep '矛盾' o3)"
else
    ok "不报矛盾（那是例外，不是自相矛盾）"
fi

echo "== ④ 现场推理推翻缓存：当场改口 + 撤销 =="
run '猫 有 毛 吗\n猫 属于 宠物\n宠物 没有 毛\n猫 有 毛 吗\n::mod\n::delib\n' o4
grep -q '我原先记的是反的' o4 && ok "当场改口（不等下一轮）" || bad "改口" "$(grep 'AI >' o4 | tail -1)"
grep -q '删边 删 猫→毛 \[思维:撤销缓存\]' o4 \
    && ok "过时的缓存被撤销（安全内核允许删「推出来的」）" || bad "撤销" "$(grep -A5 '最近应用' o4)"
grep -q '推理沉淀：记住 1 条 / 撤销 1 条' o4 && ok "撤销计数在走" || bad "撤销计数" "$(grep '推理沉淀' o4)"

echo "== ⑤ 两条继承路径真打架 → 老实报矛盾 =="
run '猫 有 毛 吗\n猫 属于 宠物\n宠物 没有 毛\n猫 有 毛 吗\n猫 有 毛 吗\n' o5
grep -q '互相矛盾的依据' o5 \
    && ok "缓存撤掉之后，两条规则打架如实说" || bad "矛盾" "$(grep 'AI >' o5 | tail -1)"

echo "== ⑥ 缓存随图存盘，重载后仍标着「推出来的」 =="
run '猫 有 毛 吗\n::save slot\n' o6
[ -s slot ] && ok "存盘成功" || bad "存盘" "$(tail -2 o6)"
cp model.bin keep.bin
mv slot m.bin
printf '::rel 猫\n::quit\n' | ./aip --model m.bin > o7 2>&1 || true
grep -q '猫 --\[有\]--> 毛（推出来的，可撤）' o7 \
    && ok "重载后仍然是「推出来的」（没冒充观测）" || bad "存读" "$(grep '毛' o7)"

echo "== ⑦ ::thinkcache off：想还是照想，只是不记 =="
run '::thinkcache off\n猫 有 毛 吗\n::rel 猫\n::delib\n' o8
if grep -q '猫 --\[有\]--> 毛（推出来的，可撤）' o8; then
    bad "关掉之后不该沉淀" "$(grep '毛（' o8)"
else
    ok "关掉之后不再沉淀"
fi
grep -q '对 ， 猫' o8 && ok "结论照样给得出来（只是不记）" || bad "结论" "$(grep 'AI >' o8)"
grep -q '沉淀=关' o8 && ok "报告里写明沉淀关着" || bad "开关" "$(grep '沉淀=' o8)"

echo "== ⑧ 缓存跟着前提走：前提没了就撤（不用等人来推翻） =="
run '猫 有 毛 吗\n动物 没有 毛\n::delib\n::mod\n::rel 猫\n' o9
grep -q '前提没了 1 条' o9 && ok "收尾扫描发现「前提没了」" || bad "前提扫描" "$(grep '推理沉淀' o9)"
grep -q '删边 删 猫→毛 \[思维:前提没了\]' o9 \
    && ok "没依据的缓存被撤销（来源写的是「前提没了」）" || bad "撤销" "$(grep -A6 '最近应用' o9)"
if grep -q '猫 --\[有\]--> 毛（推出来的' o9; then
    bad "那条缓存该没了" "$(grep '推出来的' o9)"
else
    ok "图上那条缓存已经清掉"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS consolidate（推理沉淀：记住 / 复用 / 观测压过 / 撤销 / 存读，$pass 项）"
else
    echo "FAIL consolidate（$fail 项失败，$pass 项通过）"
    exit 1
fi
