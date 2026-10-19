#!/bin/sh
# tests/datahealth.sh — **数据健康检查**（§6.359）：一条命令跑完三件事
#
#   ① 桶计数（patcount）：文件行数 vs 加载数，防"桶满静默丢弃"；
#   ② 行使用率（patuse）：哪些 `[reply]` 在任何测例上都没命中（冗余/覆盖缺口线索）；
#   ③ 误伤/学不到（datacheck）：普通说法在"有数据"时反而不学/不答的（`，对吗`、`教我` 那类）。
# 出的是**清单**，不是判分。不是回归套件（不进 suites.txt）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }

echo "① 桶计数"
sh tests/patcount.sh || true

echo "② 行使用率（全部测例输入）"
[ -x ./build_patuse ] || gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_patuse tools/patuse.c -lm
cat tests/holdout*.txt tests/blind*.txt tests/corpus_q100*.txt 2>/dev/null \
    | grep -v '^#' | cut -f1 | grep -v '^[[:space:]]*$' > /tmp/dh_inputs.txt
./build_patuse "$MODEL" < /tmp/dh_inputs.txt 2>/dev/null > /tmp/dh_pu.txt || true
grep -E '^输入 |^  未见:|^形状行 |^  形状未见:' /tmp/dh_pu.txt | head -40 || true

echo "③ 误伤 / 学不到"
for c in tests/datacorpus.txt tests/datacorpus2.txt; do
    [ -f "$c" ] || continue
    printf '   %s：' "$(basename "$c")"
    line=$(sh tests/datacheck.sh "$MODEL" "$c" 2>&1 | tail -1) || true
    echo "${line:-（没有输出）}"
done
