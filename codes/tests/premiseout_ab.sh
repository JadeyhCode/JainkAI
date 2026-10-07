#!/bin/sh
# tests/premiseout_ab.sh — A/B 测量：让不让「思维」前提说明进答复
#
# 为什么单独一个脚本、不并进 tests/all.sh：
#   ① 它要把同一批看护跑**两遍**（约 2× 时间）；
#   ② 它要**故意**打开一个默认关着的开关 —— 那是测量，不是回归。
# 它回答的是「翻默认值之前必须先回答」的那个问题：
#   把「有前提、没成立」追加进答复，会不会动既有行为基线？动了哪些、为什么？
#   （这个开关**默认关**；脚本量的是"打开会怎样"。）
#
# 用法：sh tests/premiseout_ab.sh
# 结果：_t/pab/{off,on}/<suite>.{log,res}，屏幕上一张对照表。
#
# 为什么它不会乱动基线：只有**这一轮真的有一条连结因为前提没成立而没用**时，
# 才会追加一句；现成模型与语料里一条前提链接都没有（0 条），所以默认开/关
# 对既有会话是同一个行为 —— 这一条正是脚本要量的。
#
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-gcc}
$CC -std=c11 -O2 -o build_aip main.c -lm

# 受影响的几套：**从 tests/ab_suites.txt 读**（唯一一份；SUITES= 可临时盖掉）
SUITES=${SUITES:-$(grep -v '^#' "$ROOT/tests/ab_suites.txt" 2>/dev/null | tr '\n' ' ')}
[ -n "$SUITES" ] || { echo "读不到 tests/ab_suites.txt 的套件清单"; exit 2; }
OUT="$ROOT/_t/pab"
rm -rf "$OUT"; mkdir -p "$OUT/off" "$OUT/on"

run_all() {   # $1 = off|on
    mode=$1
    for s in $SUITES; do
        if [ "$mode" = on ]; then
            if AIP_PREMISEOUT=1 sh "tests/$s.sh" > "$OUT/$mode/$s.log" 2>&1; then
                echo PASS > "$OUT/$mode/$s.res"
            else
                echo FAIL > "$OUT/$mode/$s.res"
            fi
        else
            if AIP_PREMISEOUT=0 sh "tests/$s.sh" > "$OUT/$mode/$s.log" 2>&1; then
                echo PASS > "$OUT/$mode/$s.res"
            else
                echo FAIL > "$OUT/$mode/$s.res"
            fi
        fi
    done
}
echo "跑两遍（默认 off / AIP_PREMISEOUT=1 on）……"
run_all off
run_all on

printf '\n%-14s %-6s %-6s %s\n' 看护 说明关 说明开 变化
printf '%-14s %-6s %-6s %s\n' ---- ------ ------ ----
changed=0
for s in $SUITES; do
    a=$(cat "$OUT/off/$s.res"); b=$(cat "$OUT/on/$s.res")
    mark="-"
    if [ "$a" != "$b" ]; then mark="**变了**"; changed=$((changed+1)); fi
    if [ "$a" = PASS ] && [ "$b" = FAIL ]; then mark="**被接管弄坏**"; fi
    printf '%-14s %-6s %-6s %s\n' "$s" "$a" "$b" "$mark"
done
printf '\n变了 %d / %d 套；日志在 %s\n' "$changed" "$(printf '%s' "$SUITES" | wc -w)" "$OUT"
