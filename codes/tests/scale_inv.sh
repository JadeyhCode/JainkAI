#!/bin/sh
# tests/scale_inv.sh — **扩图不改语义**（阶段三 M5 判据 #8；§6.695）
#
# 判什么：同一批问题，在
#   ① 原图（出厂 model.bin）
#   ② 原图 + **一批无关事实**（2000 条合成事实，词都是新词）
# 上问，答复必须**逐字相同**。
#
# 为什么这样测（而不是"在 S/M/K10 三张合成图上问同一批问题"）：
#   S/M/K10 是**不同**的图（内容不同），同一批问题在三张图上的答复本来就该不同 ——
#   那样比"一致"是在测"图一样"，没意义。真正要防的是：**图大了以后，无关数据把已有答案挤掉**
#   （候选集上限、剪枝、缓存、反查表截断…这类"规模一上来就悄悄改答案"的东西）。
#   所以：原图 vs 原图+噪声，是能真正判定的那个版本。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/scale_inv"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }
cp "$ROOT/relations.txt" . 2>/dev/null || true
NOISE=${1:-2000}

pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

# ① 从 relations.txt 里取前 300 条**事实**做问题（问法统一：`A R B 吗`）
awk 'NF>=3 && $1 !~ /^#/ { print $1" "$2" "$3" 吗"; n++; if (n>=300) exit }' relations.txt > q.txt || true
NQ=$(wc -l < q.txt)
[ "${NQ:-0}" -ge 50 ] || { echo "FAIL: 问题太少（$NQ）—— relations.txt 读不到？"; exit 1; }
printf '  [问题] %s 条（取自 relations.txt 前三元组）\n' "$NQ"

ask_all() {   # $1=输出文件
    printf '::notepad off\n::char off\n' > ask.in
    cat q.txt >> ask.in
    printf '::quit\n' >> ask.in
    ./build_aip --model m.bin < ask.in > "$1" 2>&1
    grep -a 'AI >' "$1" | sed 's/^你 > AI > //' > "$1.ans"
}

echo "== ① 原图上的答复 =="
ask_all base.log
printf '  答复 %s 条\n' "$(wc -l < base.log.ans)"

echo "== ② 教 $NOISE 条**无关**事实（全新词，不碰问题里的词） =="
{
    i=1
    while [ "$i" -le "$NOISE" ]; do
        printf '::teach 噪声%d 属于 类%d\n' "$i" "$i"
        i=$((i + 1))
    done
    printf '::quit\n'
} > teach.in
./build_aip --model m.bin < teach.in > teach.log 2>&1 || true
printf '  教完：%s\n' "$(grep -ac '教' teach.log)"

echo "== ③ 同一批问题再问一遍（图里已经多了 $NOISE 条无关事实） =="
ask_all noisy.log
printf '  答复 %s 条\n' "$(wc -l < noisy.log.ans)"

echo "== ④ 两次答复必须逐字相同 =="
if diff -q base.log.ans noisy.log.ans > /dev/null 2>&1; then
    ok "$NQ 条答复**逐字相同**（无关数据没有把已有答案挤掉）"
else
    D=$(diff base.log.ans noisy.log.ans | grep -c '^[<>]' || true)
    bad "有 $D 行答复变了" "$(diff base.log.ans noisy.log.ans | head -4 | tr '\n' ' ')"
fi
# 空答复也算"答案一致"，但要确认不是"两边都空"（那样这条测试是假的）
NE=$(grep -ac . base.log.ans || true)
[ "${NE:-0}" -gt 0 ] && ok "答复非空（$NE 条有内容，不是两边都是空串）" || bad "答复全是空" ""

echo
printf '扩图不改语义：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS scale_inv（原图 vs 原图+%s 条无关事实：%s 条答复逐字相同）\n' "$NOISE" "$NQ" || printf 'FAIL scale_inv\n'
[ "$fail" -eq 0 ]
