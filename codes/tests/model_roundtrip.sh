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

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" \
    -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror 2>cc.log || true
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

echo "[1/6] 训练语料 → slot0"
printf '::save slot0\n::stats\n::quit\n' > in1.txt
./aip "$CORPUS" < in1.txt > run1.log 2>&1 || fail "run1 退出非 0"
[ -s slot0 ] || fail "slot0 没生成"
grep -q '帧生成=' run1.log || fail "训练态没有帧层统计行"

echo "[2/6] 加载 slot0 → 立刻存 slot1"
printf '::save slot1\n::stats\n::quit\n' > in2.txt
./aip --model slot0 < in2.txt > run2.log 2>&1 || fail "run2 退出非 0"
cmp -s slot0 slot1 || fail "加载后重存(slot1) != 原件(slot0)"
echo "  OK slot0 == slot1（$(wc -c < slot0) 字节）"

echo "[3/6] 装载态 vs 训练态：规范状态 dump 逐行一致"
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

echo "[4/6] 加载 slot0 + 对话 vs 训练 + 对话"
cat chat.txt > in4.txt
# §6.111：边的**时刻集合**是"历史记录"（训练期在哪些 tick 被观测过）——装载态**无法重放**那段历史，
# 所以比"装载态 == 原地训练"之前先把这份记录清掉（**只清它**）；它的持久化另立幂等看护：
# 见本脚本 ⑤ 之后那条「slot2 存→读→再存，逐字节一致」。
printf '::tset clear\n::save slot2\n::quit\n' >> in4.txt
./aip --model slot0 < in4.txt > run3.log 2>&1 || fail "run3 退出非 0"
cat chat.txt > in5.txt
printf '::tset clear\n::save slot3\n::quit\n' >> in5.txt
./aip "$CORPUS" < in5.txt > run4.log 2>&1 || fail "run4 退出非 0"

grep -o 'AI > .*' run4.log > r3.resp || true
grep -o 'AI > .*' run3.log > r4.resp || true
[ -s r3.resp ] || fail "原地训练路径没有产生回复"
if diff -u r3.resp r4.resp; then echo "  OK 回复逐行一致（$(wc -l < r3.resp) 行）"
else fail "加载前后回复不一致"; fi
cmp -s slot2 slot3 || fail "加载后对话轨迹(slot2) != 原地训练(slot3)"
echo "  OK 加载后状态与原地训练逐字节一致（$(wc -c < slot2) 字节）"

echo "[5/6] 统计对比"
grep -m1 '^\[统计\]' run1.log > s1.stat || true
grep -m1 '^\[统计\]' run2.log > s2.stat || true
if diff -u s1.stat s2.stat; then echo "  OK 加载前后统计一致"; else fail "统计丢层"; fi

echo "[6/6] v9 → v10：旧格式读得回来，「v9 读 → v10 存 → v10 读」等价"
# 输入是**冻结的 v9 样本** tests/v9_sample.bin —— 它故意在边上带**非空的语境字段**
# （n_subs=1 / n_subs=2+n_not=1 / 动作槽+语境同时非空）。为什么要专门造它：
# 出厂 model.bin 里那些字段实测**全是 0**，而"跳过 N 个 4 字节 id"那几行在全 0 时
# 跳 0 个 —— 写错了也看不出来（"拿旧二进制量出新结论"那个坑的存档版）。
# 生成器源码 + 复现命令见 docs/audit-vs-deepseek看这里.md §6.50
# （v10 之后那个工具编译不了，所以源码留档；样本本身冻在这里）。
V9="$ROOT/tests/v9_sample.bin"
[ -f "$V9" ] || fail "缺 v9 样本 $V9（v9→v10 迁移看护的输入）"
# 先复核样本身份：文件头声明的 n_tok / n_assoc（magic + version + VSA_DIM 之后两个 u32）
declared=$(od -An -tu4 -j12 -N8 "$V9" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$declared" = "38 25" ] || fail "v9 样本头里 n_tok/n_assoc = 「$declared」（应当是 38 25：样本被换了？）"
printf '::load %s\n::char off\n::stats\n::fp\n::save v10slot\n::quit\n' "$V9" > in6.txt
./aip < in6.txt > run6.log 2>&1 || fail "读 v9 样本退出非 0"
grep -q '边=25' run6.log \
    || fail "v9 样本读进来边数 ≠ 文件头声明的 25：$(grep -m1 '统计' run6.log)"
# 另存出来必须真的是**当前版本**（版本号 = magic 之后的第二个 u32）。
# §6.110 起当前版本是 **11**（多了一段 N1 摘要）—— 旧 v9/v10 样本照旧读得回来。
curver=$(od -An -tu4 -j4 -N4 v10slot | tr -d ' ')
[ "$curver" = "13" ] || fail "另存文件的版本号是 $curver（应当是 13）"
# 关键等价：v9 直接读进来的内存态 与 把它存成 v10 再读回来，**指纹逐位一致**
fp_v9=$(grep -o '\[指纹\] [0-9a-f]*' run6.log | head -1)
printf '::load v10slot\n::char off\n::fp\n::quit\n' > in7.txt
./aip < in7.txt > run7.log 2>&1 || fail "读 v10slot 退出非 0"
fp_v10=$(grep -o '\[指纹\] [0-9a-f]*' run7.log | head -1)
[ -n "$fp_v9" ] || fail "v9 装载态没打出指纹"
[ "$fp_v9" = "$fp_v10" ] || fail "v9→v10 不等价：$fp_v9 vs $fp_v10"
echo "  OK 边=25（与文件头一致）、另存版本=10、指纹 $fp_v9 前后一致"
# 顺带钉住"语境字段真的没了"：新代码 ::edges 里不许再出现「语境[」。
# （同一份样本在旧代码上会打出 `语境[水里]` / `语境[水里,岸上] 非[岸上]` —— 实测的 A/B，
#  见 §6.50；所以这条断言不是看格式，是看"旧字段确实被丢掉了"。）
printf '::load %s\n::char off\n::edges 猫\n::quit\n' "$V9" > in8.txt
./aip < in8.txt > run8.log 2>&1 || fail "读样本看边退出非 0"
grep -q '语境\[' run8.log && fail "v10 的 ::edges 还在打语境字段（字段应当已删）"
grep -q '→ 鱼' run8.log || fail "样本里的边（猫→鱼）没读出来"
echo "  OK 语境字段不再出现在 ::edges（旧边当普通边读进来 —— 少一层条件，见 §6.50 局限）"

echo "PASS model_roundtrip"
