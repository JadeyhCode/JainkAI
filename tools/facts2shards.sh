#!/bin/sh
# tools/facts2shards.sh — 把抓取出来的**文本事实**转成 `.c7b` v2 分片并**压缩**（§6.716）
#
# 为什么必须转：文本 ~54 B/条 ⇒ 7e9 条 = 378 GB；v2 裸 34 B/条 ⇒ 248 GB；**gzip 后 6.4 B/条 ⇒ 45 GB**。
# 只处理**轮转出来的完成块**（`facts.<k>.<序号>.triples`）—— 抓取还在写的 `facts.<k>.triples` 不碰。
#
# 用法：sh tools/facts2shards.sh <目录> [--watch 秒]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=${1:?用法: sh tools/facts2shards.sh <目录> [--watch 秒]}
WATCH=0
[ "${2:-}" = "--watch" ] && WATCH=${3:-60}
mkdir -p "$DIR/shards"
BIN="$ROOT/_t/c7b2/triples2c7b"
mkdir -p "$ROOT/_t/c7b2"
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o "$BIN" "$ROOT/tools/triples2c7b.c" -lm || { echo "FAIL: triples2c7b 编不过"; exit 1; }

one_round() {
    n=0
    for blk in "$DIR"/facts.*.[0-9]*.triples; do
        [ -f "$blk" ] || continue
        base=$(basename "$blk" .triples)
        [ -f "$DIR/shards/$base.done" ] && continue
        printf '  转换 %s（%s MB）… ' "$base" "$(awk -v b="$(stat -c %s "$blk")" 'BEGIN{printf "%.0f", b/1048576}')"
        # 分片前缀用块名（一个块可能出多个分片）
        if ! "$BIN" "$blk" "$DIR/shards/$base" --max-edges 5000000 >> "$DIR/convert.log" 2>&1; then
            echo "失败（见 convert.log）"; continue
        fi
        gz=0; raw=0
        for sh in "$DIR/shards/$base".*.c7b2; do
            [ -f "$sh" ] || continue
            raw=$((raw + $(stat -c %s "$sh")))
            gzip -c -6 "$sh" > "$sh.gz" && rm -f "$sh"
            gz=$((gz + $(stat -c %s "$sh.gz")))
        done
        rm -f "$blk"
        : > "$DIR/shards/$base.done"
        n=$((n + 1))
        printf 'OK（裸 %d MB ⇒ gz %d MB）\n' "$((raw / 1048576))" "$((gz / 1048576))"
    done
    return "$n"
}

while :; do
    one_round
    tot=$(ls "$DIR"/shards/*.c7b2.gz 2>/dev/null | wc -l)
    sz=$(du -sm "$DIR/shards" 2>/dev/null | cut -f1)
    printf '  [分片 %s 个 / %s MB] %s\n' "$tot" "${sz:-0}" "$(date '+%H:%M:%S')"
    [ "$WATCH" -eq 0 ] && break
    sleep "$WATCH"
done
