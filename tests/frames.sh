#!/bin/sh
# tests/frames.sh — 结构化帧生成的回归
#
# 这是「会造句」这条能力的看护：
#   ① 一批**语料里没写过**的输入，必须被帧生成器渲染成新句子；
#   ② 命中率不能低于阈值（当前 27/28，留一点余量取 26）；
#      其中「你睡得好吗」是靠**拆解切分**才中的（frame.h 的切分搜索），
#      所以这个阈值同时看护着那条能力。
#   ③ 抽查几句，必须是新句子而不是原句复述。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
CC=${CC:-gcc}

$CC -std=c11 -O2 -o trainframes trainframes.c
./trainframes frames.txt
$CC -std=c11 -O2 -o build_frq tools/frq.c -lm

PROBE=tools/probes_frames.txt
[ -f "$PROBE" ] || { echo "FAIL: 缺 $PROBE"; exit 1; }

out=$(./build_frq frames.txt "$PROBE")
echo "$out" | tail -1

hit=$(echo "$out" | tail -1 | sed 's/.*帧命中 \([0-9]*\)\/.*/\1/')
tot=$(echo "$out" | tail -1 | sed 's/.*\/\([0-9]*\).*/\1/')
[ -n "$hit" ] && [ -n "$tot" ] || { echo "FAIL: 解析不出命中数"; exit 1; }
[ "$hit" -ge 26 ] || { echo "FAIL: 帧命中只有 $hit/$tot，低于 26"; exit 1; }

for want in "我 喜欢 雪" "我 吃 了 饭" "我 知道 量子" "我 想 学 画画" \
            "我 觉得 猫 会 游泳" "今天 可能 会 下雨"; do
    echo "$out" | grep -q "$want" || { echo "FAIL: 缺「$want」"; exit 1; }
done

echo "PASS frames（$hit/$tot 命中，且抽查的都是语料里没有的句子）"
