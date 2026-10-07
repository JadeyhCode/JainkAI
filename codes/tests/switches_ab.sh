#!/bin/sh
# tests/switches_ab.sh — 把所有"默认关"的开关**一次跑一遍**，看它们各自会不会动既有基线
#
# 为什么单独一个脚本、不并进 tests/all.sh：
#   ① 它要把整包跑**多遍**（每遍一个开关）；② 它量的是"打开会怎样"，那是测量不是回归。
# 它回答的是每次翻默认值之前都必须先回答的那个问题：
#   **这个开关打开之后，31 套里有没有哪一套变了？**
#
# 用法：sh tests/switches_ab.sh            （全部开关，约 5× 整包时间）
#       ONLY=wgate sh tests/switches_ab.sh （只跑一个）
# 结果：_t/sw/{off,<开关>}/<suite>.res + 屏幕上一张对照表。
#
# 约定（前面几轮踩出来的）：
#   · 每个开关都要有一条**自己那套**的断言，且在反向测量时**自动跳过默认值断言**
#     （`if [ "${AIP_X:-}" = ... ]`），否则整包在反向跑法下必红；
#   · 开关只影响"默认关"的行为时，两种跑法都该全绿 —— 那才是"机制在、行为不动"。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-gcc}
$CC -std=c11 -O2 -o build_aip main.c -lm

# 开关名 → 环境变量赋值（一行一个；加开关只加一行）
# 一个开关一行（**行**分隔，不是空格 —— 值里可能有空格，比如成对测）：
#   notepad+patch 这一对是**已知会改输出**的（AI 自己写草稿并决定发送时机）；
#   单独跑 patch 或单独跑 notepad 都不改基线，合起来才改。
cat > /tmp/.sw_list.$$ <<'EOF'
wgate:AIP_WGATE=1
premhold-verified:AIP_PREMHOLD=verified
route:AIP_ROUTE=1
affect:AIP_AFFECT=1
notepad-on:AIP_NOTEPAD=on
patch-note:AIP_PATCH=记事本
notepad+patch:AIP_PATCH=记事本 AIP_NOTEPAD=on
thinkout-off:AIP_THINKOUT=0
premiseout-off:AIP_PREMISEOUT=0
trust:AIP_TRUST=1
d4:AIP_D4=1
EOF
if [ -n "${ONLY:-}" ]; then
    grep "^$ONLY:" /tmp/.sw_list.$$ > /tmp/.sw_use.$$ || { echo "没有这个开关：$ONLY"; exit 2; }
else
    cp /tmp/.sw_list.$$ /tmp/.sw_use.$$
fi

OUT="$ROOT/_t/sw"
rm -rf "$OUT"; mkdir -p "$OUT"

# 省时间的两档（默认仍是"整包 × 每个开关"，最慢但最全）：
#   SKIP_BASE=1  复用上次的基线（`_t/sw/off.res`），少跑一轮整包（并行后 ≈70 秒；串行 6 分钟）
#   SUITES="a b c"  只跑受影响的几套（`tests/<名字>.sh`），每档从 ~70 秒降到 ~30 秒
# 什么时候能用 SUITES：改的东西只可能影响那几套时。改动一多就别省 —— 整包该跑还得跑。
if [ -n "${SKIP_BASE:-}" ] && [ -f "$OUT/off.res" ]; then
    echo "复用上次基线（SKIP_BASE=1）：$(cat "$OUT/off.res")"
else
    echo "先跑一遍基线（都不开）……"
    if sh tests/all.sh > "$OUT/off.log" 2>&1; then echo PASS > "$OUT/off.res"; else echo FAIL > "$OUT/off.res"; fi
fi
SUITES=${SUITES:-}
run_under() {   # run_under <环境变量串> <日志>
    if [ -n "$SUITES" ]; then
        rc=0
        : > "$2"
        for s in $SUITES; do
            if env $1 sh "tests/$s.sh" >> "$2" 2>&1; then :; else rc=1; fi
        done
        return $rc
    fi
    env $1 sh tests/all.sh < /dev/null > "$2" 2>&1
}

printf '\n%-20s %-6s %-6s %s\n' 开关 基线 打开 变化
printf '%-20s %-6s %-6s %s\n' ---- ---- ---- ----
changed=0
while IFS= read -r sw; do
    [ -n "$sw" ] || continue
    name=${sw%%:*}
    env=${sw#*:}
    echo "跑 $name（$env）……"
    # 不引用 $env：让 shell 按空格拆成多个"VAR=值"（支持成对/成组测）
    # `< /dev/null` 是必须的：被测脚本会读 stdin，否则会把 while 的输入吃光（只跑第一个）
    if run_under "$env" "$OUT/$name.log"; then
        echo PASS > "$OUT/$name.res"
    else
        echo FAIL > "$OUT/$name.res"
    fi
    a=$(cat "$OUT/off.res"); b=$(cat "$OUT/$name.res")
    mark="-"
    [ "$a" != "$b" ] && { mark="**变了**"; changed=$((changed+1)); }
    printf '%-20s %-6s %-6s %s\n' "$name" "$a" "$b" "$mark"
done < /tmp/.sw_use.$$
rm -f /tmp/.sw_list.$$ /tmp/.sw_use.$$
printf '\n开关动过基线的：%d 个；日志在 %s\n' "$changed" "$OUT"
