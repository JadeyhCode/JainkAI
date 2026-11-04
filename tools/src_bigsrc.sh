#!/bin/sh
# tools/src_bigsrc.sh — 抓**大源码树**（每个 URL 一次请求）§6.740
#
# 为什么：Debian/Ubuntu 源是"每包一次请求"，延迟开销吃掉大量吞吐（实测 100 个 worker 也只有
# 7–9 k 边/秒）。大源码树（glibc/gcc/binutils/gdb/make/coreutils/tar…）**一个 URL 就几百 MB**，
# 一次请求换大量事实，是提高"每请求产出"的最直接办法。
#
# 用法：sh tools/src_bigsrc.sh <输出目录> [--list 文件] [--max N]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?用法: sh tools/src_bigsrc.sh <输出目录> [--list 文件]}
LIST=$ROOT/tools/bigsrc.conf
MAXN=99
shift 1 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
    --list) LIST=$2; shift 2 ;;
    --max) MAXN=$2; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$OUT" "$OUT/tmp" || exit 1
FACTS="$OUT/facts.0.triples"
DONE="$OUT/done.txt"
: >> "$FACTS"; : >> "$DONE"
n=0
while IFS='|' read -r name url; do
    case "${name:-}" in ''|\#*) continue ;; esac
    grep -q "^$name$" "$DONE" && continue
    n=$((n + 1)); [ "$n" -gt "$MAXN" ] && break
    tb="$OUT/tmp/$name.tgz"
    echo "== $name：$url"
    if ! timeout 3600 curl -k -sL --connect-timeout 20 --max-time 3500 -o "$tb" "$url"; then
        rm -f "$tb"; echo "  下载失败（跳过，下次再试）"; n=$((n - 1)); continue
    fi
    sz=$(stat -c %s "$tb" 2>/dev/null || echo 0)
    [ "$sz" -gt 100000 ] || { rm -f "$tb"; n=$((n - 1)); continue; }
    src="$OUT/tmp/tree"; rm -rf "$src"; mkdir -p "$src"
    case "$url" in
    *.tar.xz) tar -xJf "$tb" -C "$src" 2>/dev/null ;;
    *.tar.bz2) tar -xjf "$tb" -C "$src" 2>/dev/null ;;
    *.tar.gz|*.tgz) tar -xzf "$tb" -C "$src" 2>/dev/null ;;
    *) tar -xf "$tb" -C "$src" 2>/dev/null ;;
    esac
    if [ ! -d "$src" ] || [ -z "$(ls -A "$src" 2>/dev/null)" ]; then
        rm -rf "$src" "$tb"; echo "  解压失败（跳过）"; n=$((n - 1)); continue
    fi
    : > "$OUT/tmp/f.triples"
    INC="$src" sh "$ROOT/tools/headers2triples.sh" "$OUT/tmp/h.triples" 100000000 > /dev/null 2>&1
    [ -s "$OUT/tmp/h.triples" ] && cat "$OUT/tmp/h.triples" >> "$OUT/tmp/f.triples"
    sh "$ROOT/tools/c2triples.sh" "$OUT/tmp/c.triples" --ngate "$src" > /dev/null 2>&1
    [ -s "$OUT/tmp/c.triples" ] && cat "$OUT/tmp/c.triples" >> "$OUT/tmp/f.triples"
    cnt=$(wc -l < "$OUT/tmp/f.triples" 2>/dev/null || echo 0)
    cat "$OUT/tmp/f.triples" >> "$FACTS"
    printf '%s\n' "$name" >> "$DONE"
    echo "  ⇒ $cnt 条事实 / 下载 $((sz / 1048576)) MB / $(du -sm "$src" 2>/dev/null | cut -f1) MB 源码"
    # 每棵树立刻切成完成块（转换交给 facts2shards）
    ts=$(date +%s)
    mv "$FACTS" "$OUT/facts.0.$ts.0001.triples"
    rm -rf "$src" "$tb" "$OUT"/tmp/*.triples
done < "$LIST"
echo "== 完：$OUT/done.txt 里 $(wc -l < "$DONE") 棵 =="
