#!/bin/sh
# tools/probeunhit.sh — **未命中的 reply 行 → 自动造探针 → 逐条验证**（§6.407；§6.408 加期望对照）
#
# 背景：`patuse` 只告诉你"这些行在测例上没命中"，不告诉你"行坏了"还是"只是没测过"。
# §6.406 是**手写**探针逐条问的，容易漏行。这里机械化：
#   ① 把一批输入喂 patuse，拿"从未命中"的行；
#   ② 每行造一条探针：普通行 = 每组第一个选项拼接（`既,又`→`既又`）；整句精确行（`^`）= `^` 后面那串；
#      同时带上**期望**（该行答复去掉空格的前 8 个字符）；
#   ③ 每条探针在**新进程**里问一遍，分三类：
#        同答复  —— 答复里含期望 ⇒ 这一行确实在工作；
#        不同    —— 有答复但不含期望 ⇒ 被**别的行**抢答（或该行答复被改写）；
#        兜底    —— 掉兜底 ⇒ 这行要么不可达，要么关键词写死了。
#
# 用法：sh tools/probeunhit.sh [输入文件...]
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
    for f in tests/corpus_q100*.txt tests/*para*.txt tests/blind5.txt tests/holdout*.txt; do
        [ -f "$f" ] || continue
        grep -v '^#' "$f" | cut -f1 >> "$tmp/in.txt" || true
    done
fi

AIP_PATUSE_PROBES=1 timeout 900 ./build_patuse "$MODEL" < "$tmp/in.txt" > "$tmp/full.txt" 2>/dev/null
sed -n 's/^PROBE|//p' "$tmp/full.txt" > "$tmp/probes.txt"
unhit=$(grep -c '^  未见: ' "$tmp/full.txt" || true)

n=$(grep -c . "$tmp/probes.txt" || true)
echo "未命中行 $unhit 条；造出探针 $n 条"
[ "$n" -eq 0 ] && { echo "PASS probeunhit（没有未命中的行）"; exit 0; }

same=0; other=0; fb=0
while IFS="$(printf '\t')" read -r probe expect; do
    [ -n "$probe" ] || continue
    [ -n "$expect" ] || expect=$probe
    out=$(printf '%s\n::quit\n' "$probe" | timeout 30 "$BIN" --model "$MODEL" 2>&1 | sed -n 's/.*AI > //p' | head -1)
    flat=$(printf '%s' "$out" | tr -d ' ')
    want=$(printf '%s' "$expect" | tr -d ' ')
    case "$out" in
    *不\ 太\ 懂*|*还没\ 学*|"")
        printf '  [兜底] %-14s => %s\n' "$probe" "$out"
        fb=$((fb + 1)) ;;
    *)
        case "$flat" in
        *"$want"*)
            printf '  [同答复] %-12s => %s\n' "$probe" "$out"
            same=$((same + 1)) ;;
        *)
            printf '  [不同  ] %-12s => %s\n' "$probe" "$out"
            other=$((other + 1)) ;;
        esac ;;
    esac
done < "$tmp/probes.txt"

echo "小结：$same 条同答复（该行在工作）/ $other 条被抢答 / $fb 条仍兜底"
[ "$fb" -eq 0 ] && [ "$other" -eq 0 ] \
    && echo "PASS probeunhit（未命中的行都能被探针问出**它自己的**答复）"
exit 0
