#!/bin/sh
# tests/thinkout_ab.sh — A/B 测量：让不让「思维」接管输出
#
# 为什么单独一个脚本、不并进 tests/all.sh：
#   ① 它要把同一批看护跑**两遍**（约 2× 时间）；
#   ② 它要**故意**打开一个默认关着的开关 —— 那是测量，不是回归。
# 它回答的是「翻默认值之前必须先回答」的那个问题：
#   接管输出到底会不会动既有行为基线？动了哪些、为什么？
#
# 用法：sh tests/thinkout_ab.sh
# 结果：_t/ab/{off,on}/<suite>.{log,res}，屏幕上一张对照表。
#
# 已知结论（2026-10 实测，两次）：
#   第一次：接管开 → 8 套里 5 套红（hypothesis / logic4 / exclusivity /
#           hypo_frames / thinking）。查下来是思维层自己的三个 bug：
#             ① rel_solve 的 status 是**正关系的真值**，被当成了「问法的答案」；
#             ② 没分证据强度（独占/闭合世界/假设 与 事实 并列判矛盾）；
#             ③ 只查到直接否定就下结论（规则或假设能推翻它）。
#   第二次（三个 bug 修掉之后）：7/7 行为套全绿 —— 于是接管输出改成**默认开**。
# 这个脚本留着：以后再动默认值，先跑它。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-gcc}
$CC -std=c11 -O2 -o build_aip main.c -lm

# 受影响的几套：**从 tests/ab_suites.txt 读**（唯一一份；SUITES= 可临时盖掉）
SUITES=${SUITES:-$(grep -v '^#' "$ROOT/tests/ab_suites.txt" 2>/dev/null | tr '\n' ' ')}
[ -n "$SUITES" ] || { echo "读不到 tests/ab_suites.txt 的套件清单"; exit 2; }
OUT="$ROOT/_t/ab"
rm -rf "$OUT"; mkdir -p "$OUT/off" "$OUT/on"

run_all() {   # $1 = off|on
    mode=$1
    for s in $SUITES; do
        if [ "$mode" = on ]; then
            if AIP_THINKOUT=1 sh "tests/$s.sh" > "$OUT/$mode/$s.log" 2>&1; then
                echo PASS > "$OUT/$mode/$s.res"
            else
                echo FAIL > "$OUT/$mode/$s.res"
            fi
        else
            if AIP_THINKOUT=0 sh "tests/$s.sh" > "$OUT/$mode/$s.log" 2>&1; then
                echo PASS > "$OUT/$mode/$s.res"
            else
                echo FAIL > "$OUT/$mode/$s.res"
            fi
        fi
    done
}
echo "跑两遍（默认 off / AIP_THINKOUT=1 on）……"
run_all off
run_all on

printf '\n%-14s %-6s %-6s %s\n' 看护 接管关 接管开 变化
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
