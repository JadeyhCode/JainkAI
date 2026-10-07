#!/bin/sh
# tests/model_roundtrip.sh — model.bin 存 / 读 的无损往返回归
#
# 五段（同一份二进制）：
#   ① 训练语料（不回对话） → :save slot0 → :quit        slot0 = 纯训练态
#   ② 加载 slot0 → 立刻 :save slot1                     slot1 必须与 slot0 字节一致
#   ③ 装载态 vs 训练态的**规范状态 dump**逐行一致：
#        ::pooldump    整个词池 + 每一条边（按 id，不按指针）
#        ::derivedump  派生层（idf/hv/emb/槽类/句库/话题/关系表/特殊 Token）
#      ← 这一步是 2026-09 加的关键回归。原来只比「回复」，而
#        ai->split_tok 在存盘格式里**漏了**：装载后它是 NULL，
#        ai_edge_weight() 里「回复首词权重 ×4」整档失效，
#        于是每条边的权重都只有训练态的 1/4 —— 词池 dump 逐字节相同，
#        只有走出来的句子不一样。规范 dump 里 split=-1 与 split=NNN 一眼就分出来了。
#   ④ 加载 slot0 + 对话 → :save slot2
#      训练语料 + 对话   → :save slot3                   slot2 必须与 slot3 字节一致
#   ⑤ ①与②的统计行一致
#
# 为了让 ③④ 真正覆盖到帧层与关系层，本脚本会把 <root>/frames.txt 与
# <root>/relations.txt 复制进工作目录（程序是按 cwd 读这两个文件的）。
# 踩过的坑：不复制的时候两条路径都读不到 frames.txt，
# 「读盘后帧库是空的」这种 bug 会让这个测试**照样通过**。
#
# 用法：tests/model_roundtrip.sh [语料文件]   默认 <repo>/seed_full.txt
#       （sh tests/train_model.sh 会生成 _corpus.txt，也可以传它做更大规模的验证）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CORPUS=${1:-$ROOT/seed_full.txt}
# **必须转成绝对路径**：下面会 cd 进 $WORK，相对路径那时就找不到了 ——
# 找不到语料时程序只是训了个空模型，测试照样「通过」（两条路径同样空），
# 于是这个 bug 藏了很久。实测症状：加了「装载时把关系词 intern 进词池」之后，
# 训练态 32 个 Token、装载态 42 个 Token，slot0 != slot1 才暴露出来。
case "$CORPUS" in
    /*) ;;
    *) CORPUS="$ROOT/$CORPUS" ;;
esac
[ -f "$CORPUS" ] || { echo "FAIL: 语料不存在 $CORPUS"; exit 1; }
WORK="$ROOT/_t/rt"
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    -o aip "$ROOT/main.c" -lm 2>cc.log || true
if [ ! -x aip ]; then cat cc.log; echo "FAIL: 编译失败"; exit 1; fi

# 帧库 / 关系库也要一起进工作目录，否则这两层在两条路径里都是空的（测试会假通过）
for f in frames.txt relations.txt; do
    [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .
done

fail() { echo "FAIL: $1"; exit 1; }

# 对话输入（六句，不落盘）。
# 最后一句是「他 以为 …」：归属（他者信念表）是**持久状态**（模型 v8），
# 存盘/读盘必须一致 —— 放进对话里，[4/5] 的逐字节比较就顺带看护了它。
printf '猫\n你好\n我\n猫 吃 鱼\n谢谢\n他 以为 猫 会 飞\n' > chat.txt

echo "[1/5] 训练语料 → slot0"
printf '::save slot0\n::stats\n::quit\n' > in1.txt
./aip "$CORPUS" < in1.txt > run1.log 2>&1 || fail "run1 退出非 0"
[ -s slot0 ] || fail "slot0 没生成"
grep -q '帧生成=' run1.log || fail "训练态没有帧层统计行"

echo "[2/5] 加载 slot0 → 立刻存 slot1"
printf '::save slot1\n::stats\n::quit\n' > in2.txt
./aip --model slot0 < in2.txt > run2.log 2>&1 || fail "run2 退出非 0"
cmp -s slot0 slot1 || fail "加载后重存(slot1) != 原件(slot0)"
echo "  OK slot0 == slot1（$(wc -c < slot0) 字节）"

echo "[3/5] 装载态 vs 训练态：规范状态 dump 逐行一致"
printf '::pooldump\n::derivedump\n::quit\n' > in3.txt
./aip --model slot0 < in3.txt > dump_load.log 2>&1 || fail "dump_load 退出非 0"
./aip "$CORPUS"  < in3.txt > dump_train.log 2>&1 || fail "dump_train 退出非 0"
# 只留规范状态行：T=Token  E=边  D=idf/hv/emb  S=槽类  Z=特殊 Token  Y=标量
#                    R=动态关系名  P=话题  Q=整句库  N=边容量/父节点  X=关系独占
#                    O=他者模型（他的名字 + 他相信的事实）
grep -a '^[TEDSXRPQNO ZY]' dump_load.log  | grep -av '^ ' > st_load.txt  || true
grep -a '^[TEDSXRPQNO ZY]' dump_train.log | grep -av '^ ' > st_train.txt || true
[ -s st_load.txt ] || fail "规范状态 dump 是空的（::pooldump/::derivedump 坏了）"
# N 行里的 cap= 是分配容量的痕迹（一边倍增、一边按 count 精确分配），语义无关，比之前先抹掉。
sed 's/cap=[0-9]*/cap=*/' st_load.txt  > st_load.canon
sed 's/cap=[0-9]*/cap=*/' st_train.txt > st_train.canon
if diff -u st_train.canon st_load.canon > st_diff.txt; then
    echo "  OK 规范状态逐行一致（$(wc -l < st_load.canon) 行）"
else
    head -30 st_diff.txt
    fail "装载态与训练态的规范状态不一致（见上面 diff）"
fi
grep -q '^Z ' st_load.txt || fail "状态 dump 里没有特殊 Token 行（Z）"
grep -q 'split=[0-9]' st_load.txt || fail "装载后 [原语:分割] 指针没有还原（split=-1）"

echo "[4/5] 加载 slot0 + 对话 vs 训练 + 对话"
cat chat.txt > in4.txt
printf '::save slot2\n::quit\n' >> in4.txt
./aip --model slot0 < in4.txt > run3.log 2>&1 || fail "run3 退出非 0"
cat chat.txt > in5.txt
printf '::save slot3\n::quit\n' >> in5.txt
./aip "$CORPUS" < in5.txt > run4.log 2>&1 || fail "run4 退出非 0"

grep -o 'AI > .*' run4.log > r3.resp || true
grep -o 'AI > .*' run3.log > r4.resp || true
[ -s r3.resp ] || fail "原地训练路径没有产生回复"
if diff -u r3.resp r4.resp; then echo "  OK 回复逐行一致（$(wc -l < r3.resp) 行）"
else fail "加载前后回复不一致"; fi
cmp -s slot2 slot3 || fail "加载后对话轨迹(slot2) != 原地训练(slot3)"
echo "  OK 加载后状态与原地训练逐字节一致（$(wc -c < slot2) 字节）"

echo "[5/5] 统计对比"
grep -m1 '^\[统计\]' run1.log > s1.stat || true
grep -m1 '^\[统计\]' run2.log > s2.stat || true
if diff -u s1.stat s2.stat; then echo "  OK 加载前后统计一致"; else fail "统计丢层"; fi

echo "PASS model_roundtrip"
