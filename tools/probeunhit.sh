#!/bin/sh
# tests/probeunhit.sh — **未命中的 reply 行 → 自动造探针 → 逐条验证**（§6.407）
#
# 背景：`patuse` 只告诉你"这些行在测例上没命中"，不告诉你"行坏了"还是"只是没测过"。
# §6.406 是**手写** 13 条探针逐条问的，容易漏行。这里把它机械化：
#   ① 把一批输入喂 patuse，拿"从未命中"的行；
#   ② 每行按"每组取第一个选项"造一条探针（`既,又`→`既又`，`写|作,诗|歌|词`→`写诗`）；
#   ③ 每条探针在**新进程**里问一遍，分成"给了答复 / 掉兜底"。
#
# 用法：sh tools/probeunhit.sh [输入文件...]
#       不给参数时用默认集（四样本 + 各探针 + blind5 + 留出集第一列）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${MODEL:-session.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -x ./build_patuse ] || gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_patuse tools/patuse.c -lm

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

if [ $# -gt 0 ]; then
    cat "$@" > "$tmp/in.txt"
else
    : > "$tmp/in.txt"
    for f in tests/corpus_q100*.txt tests/*para*.txt tests/blind5.txt; do
        [ -f "$f" ] || continue
        grep -v '^#' "$f" | cut -f1 >> "$tmp/in.txt" || true
    done
    for f in tests/holdout*.txt; do
        [ -f "$f" ] || continue
        grep -v '^#' "$f" | cut -f1 >> "$tmp/in.txt" || true
    done
fi

AIP_PATUSE_PROBES=1 timeout 900 ./build_patuse "$MODEL" < "$tmp/in.txt" > "$tmp/full.txt" 2>/dev/null
sed -n 's/^PROBE|//p' "$tmp/full.txt" > "$tmp/probes.txt"
unhit=$(grep -c '^  未见: ' "$tmp/full.txt" || true)

n=$(grep -c . "$tmp/probes.txt" || true)
echo "未命中行 $unhit 条；能造探针 $n 条（整句精确「^」行造不出来，会少算）"
[ "$n" -eq 0 ] && { echo "PASS probeunhit（没有未命中的行）"; exit 0; }

ok=0; bad=0
while IFS= read -r probe; do
    [ -n "$probe" ] || continue
    out=$(printf '%s\n::quit\n' "$probe" | timeout 30 "$BIN" --model "$MODEL" 2>&1 | sed -n 's/.*AI > //p' | head -1)
    case "$out" in
    *不\ 太\ 懂*|*还没\ 学*|*还没学过*|"")
        printf '  [兜底] %-18s => %s\n' "$probe" "$out"
        bad=$((bad + 1)) ;;
    *)
        printf '  [答复] %-18s => %.40s\n' "$probe" "$out"
        ok=$((ok + 1)) ;;
    esac
done < "$tmp/probes.txt"

echo "小结：$ok 条给了答复，$bad 条仍掉兜底"
[ "$bad" -eq 0 ] && echo "PASS probeunhit（未命中的行都能被探针问出答复 ⇒ 是覆盖缺口，不是坏行）"
exit 0
