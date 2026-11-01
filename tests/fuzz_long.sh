#!/bin/sh
# tests/fuzz_long.sh — **加量模糊测试**（手动跑；阶段三 M4-3；§6.691）
#
# 为什么不进整包：这两段加起来十几分钟（bughunt 每个 3000 步 ≈ 100 s × 5 个种子），
# 而整包要在两分钟内跑完 68 套。所以：**整包里跑短版**（`cmdfuzz 20 200` + `bughunt 300×3`），
# **这一支手动跑**（发版前 / 改过状态机以后）。
#
# 为什么值得单独一支：扩量之后它真的抓到过一个 **segfault**
# （`::brain save N` 顺着被 `::brain drop` 置空的槽解引用，§6.691）——
# 短版 6×40 抓不到，20×200 一跑就崩。
#
# 用法：sh tests/fuzz_long.sh [bughunt 步数] [cmdfuzz 种子数] [cmdfuzz 步数]
#   默认 3000 / 24 / 240（约 10 分钟）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STEPS=${1:-3000}
SEEDS=${2:-24}
CSTEPS=${3:-240}
cat "$ROOT/model.bin" > /dev/null 2>&1 || { echo "FAIL: 缺 model.bin"; exit 1; }
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

echo "== ① 随机动作 + 不变式（bughunt ${STEPS} 步 × 5 个固定种子） =="
mkdir -p "$ROOT/_t"
if ! gcc -std=c11 -O2 -Wall -Wextra -o "$ROOT/_t/fuzz_long_bughunt" "$ROOT/tools/bughunt.c" -lm 2> /tmp/fuzz_long_build.log; then
    echo "FAIL: bughunt 编不过"; tail -5 /tmp/fuzz_long_build.log; exit 1
fi
for s in 20261025 1234 777 42 9001; do
    t0=$(date +%s)
    if ( cd "$ROOT" && "./_t/fuzz_long_bughunt" model.bin "$s" "$STEPS" > "/tmp/fuzz_long_$s.log" 2>&1 ); then
        ok "种子 $s：$(( $(date +%s) - t0 )) s，$(tail -1 "/tmp/fuzz_long_$s.log")"
    else
        bad "种子 $s 红了" "$(tail -3 "/tmp/fuzz_long_$s.log" | tr '\n' ' ')"
    fi
done

echo "== ② 命令面随机会话（cmdfuzz ${SEEDS} × ${CSTEPS}） =="
t0=$(date +%s)
if ( cd "$ROOT" && sh tests/cmdfuzz.sh "$SEEDS" "$CSTEPS" > /tmp/fuzz_long_cmd.log 2>&1 ); then
    ok "$(( $(date +%s) - t0 )) s：$(grep -a '命令面随机会话' /tmp/fuzz_long_cmd.log | tail -1)"
else
    bad "cmdfuzz 有种子红了" "$(grep -a 'FAIL' /tmp/fuzz_long_cmd.log | head -3 | tr '\n' ' ')"
fi

echo
printf '加量模糊：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS fuzz_long（bughunt %s×5 + cmdfuzz %s×%s）\n' "$STEPS" "$SEEDS" "$CSTEPS" || printf 'FAIL fuzz_long\n'
[ "$fail" -eq 0 ]
