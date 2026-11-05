#!/bin/sh
# tools/code_ability.sh — 编程能力评测（分级：通过 / 编过但测试不过 / 没有匹配的任务 / 编不过）§6.754
#
# 为什么单独做：`codebench.sh` 只跑**它自己那 15 个**任务（都是骨架里已有的）。
# 要判断"到底会不会写代码"，得用**没见过的任务**，并把失败原因分开记：
#   · 没有匹配的任务  ⇒ 缺**骨架**（可以教）
#   · 编过但测试不过  ⇒ 骨架有，但程序/自测不对（要修）
#   · 编不过          ⇒ 代码生成层的问题
#
# 用法：sh tools/code_ability.sh [任务清单] [模型]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TASKS=${1:-$ROOT/tests/fixtures/codebattery.txt}
MODEL=${2:-$ROOT/code/code_model.bin}
WORK="$ROOT/_t/codeability"
rm -rf "$WORK"; mkdir -p "$WORK"
pass=0; testfail=0; matchfail=0; ccfail=0; total=0
i=0
while IFS= read -r t; do
    [ -n "$t" ] || continue
    i=$((i + 1)); total=$((total + 1))
    printf '::act on\n::code synth %s\n::quit\n' "$t" > "$WORK/ask.in"
    out=$(cd "$ROOT" && timeout 120 "$ROOT/build_aip" --model "$MODEL" < "$WORK/ask.in" 2>&1)
    if echo "$out" | grep -qa '测试全绿'; then
        pass=$((pass + 1)); printf '  [✓] %s\n' "$t"
    elif echo "$out" | grep -qa '没有匹配的任务'; then
        matchfail=$((matchfail + 1)); printf '  [缺骨架] %s\n' "$t"
    elif echo "$out" | grep -qa '编过但\*\*测试不过\*\*\|编过但\*\*测试不过'; then
        testfail=$((testfail + 1)); printf '  [测试不过] %s\n' "$t"
    else
        ccfail=$((ccfail + 1)); printf '  [编不过] %s\n' "$t"
    fi
done < "$TASKS"
printf '\n模型 %s：**%d / %d 通过**（缺骨架 %d / 测试不过 %d / 编不过 %d）\n' \
    "$(basename "$MODEL")" "$pass" "$total" "$matchfail" "$testfail" "$ccfail"
[ "$pass" -eq "$total" ]
