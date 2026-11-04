#!/bin/sh
# tools/src_local.sh — 挖**本机已有的源码**（网络被限速时的唯一增量来源）§6.745
#
# 四个去处（都不需要网络）：
#   /usr/src           内核/驱动源码与头文件
#   ~/.npm/_cacache    npm 缓存里的 .tgz（JS/TS 真源码）
#   ~/.cache/pip        pip 缓存里的 wheel/sdist（Python）
#   ~/.cargo/registry  crates（Rust，只抽 `fn`/`struct`/`impl` 的粗事实）
#
# 用法：sh tools/src_local.sh <输出目录>
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?用法: sh tools/src_local.sh <输出目录>}
mkdir -p "$OUT/tmp" || exit 1
F="OUT/tmp/f.triples"
: > "$OUT/facts.0.triples"

emit() { cat "$1" >> "$OUT/facts.0.triples"; }

echo "== /usr/src（C/C++ 头与源码）=="
if [ -d /usr/src ]; then
    INC=/usr/src sh "$ROOT/tools/headers2triples.sh" "$OUT/tmp/h.triples" 100000000 > /dev/null 2>&1
    [ -s "$OUT/tmp/h.triples" ] && { emit "$OUT/tmp/h.triples"; echo "  头事实 $(wc -l < "$OUT/tmp/h.triples")"; }
    sh "$ROOT/tools/c2triples.sh" "$OUT/tmp/c.triples" --ngate /usr/src > /dev/null 2>&1
    [ -s "$OUT/tmp/c.triples" ] && { emit "$OUT/tmp/c.triples"; echo "  片段 $(wc -l < "$OUT/tmp/c.triples")"; }
fi

echo "== ~/.npm/_cacache（缓存的 .tgz）=="
if [ -d "$HOME/.npm/_cacache/content-v2" ]; then
    cnt=0
    find "$HOME/.npm/_cacache/content-v2" -type f 2>/dev/null | while IFS= read -r f; do
        head -c 2 "$f" | od -An -tx1 | grep -q '1f 8b' || continue
        src="$OUT/tmp/js"; rm -rf "$src"; mkdir -p "$src"
        tar -xzf "$f" -C "$src" 2>/dev/null || continue
        pkg=$(basename "$(dirname "$(find "$src" -maxdepth 2 -name package.json | head -1)")" 2>/dev/null)
        [ -n "${pkg:-}" ] || pkg=cache
        sh "$ROOT/tools/jsfacts.sh" "$src" "$pkg" "$OUT/tmp/j.triples" > /dev/null 2>&1
        [ -s "$OUT/tmp/j.triples" ] && emit "$OUT/tmp/j.triples"
        rm -rf "$src" "$OUT/tmp/j.triples"
        cnt=$((cnt + 1))
    done
    echo "  npm 缓存已扫"
fi

echo "== ~/.cache/pip（wheel / sdist）=="
if [ -d "$HOME/.cache/pip" ]; then
    find "$HOME/.cache/pip" -type f \( -name '*.whl' -o -name '*.tar.gz' \) 2>/dev/null | while IFS= read -r f; do
        src="$OUT/tmp/py"; rm -rf "$src"; mkdir -p "$src"
        case "$f" in
        *.whl) unzip -q -o "$f" -d "$src" 2>/dev/null ;;
        *.tar.gz) tar -xzf "$f" -C "$src" 2>/dev/null ;;
        esac
        [ -d "$src" ] || continue
        sh "$ROOT/tools/pyfacts.sh" "$src" cache "$OUT/tmp/p.triples" > /dev/null 2>&1
        [ -s "$OUT/tmp/p.triples" ] && emit "$OUT/tmp/p.triples"
        rm -rf "$src" "$OUT/tmp/p.triples"
    done
    echo "  pip 缓存已扫"
fi

echo "== 完：$OUT/facts.0.triples = $(wc -l < "$OUT/facts.0.triples") 行 =="
