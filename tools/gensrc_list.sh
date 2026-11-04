#!/bin/sh
# tools/gensrc_list.sh — 扫描镜像目录，生成"大源码树"清单（§6.742）
#
# 为什么：`bigsrc.conf` 是手写的 16 棵，而 GNU 镜像有 **330 个项目**目录，
# 每个项目 = 一次索引请求 + 一次 tarball 请求，密度远高于 Debian 平均包。
#
# 用法：sh tools/gensrc_list.sh <输出文件> [最多项目数，默认 60] [镜像根，默认 gnu]
set -u
OUT=${1:?用法: sh tools/gensrc_list.sh <输出文件> [最多项目数] [镜像根]}
MAXN=${2:-60}
BASE=${3:-https://mirrors.huaweicloud.com/gnu}
: > "$OUT"
timeout 60 curl -k -s "$BASE/" -o /tmp/gen_idx.html || { echo "FAIL: 目录取不到"; exit 1; }
projs=$(grep -o 'href="[a-z0-9_.+-]*/"' /tmp/gen_idx.html | sed 's/href="//; s|/$||' | grep -v '^\.\.$' | LC_ALL=C sort -u | head -"$MAXN")
n=0
for p in $projs; do
    timeout 40 curl -k -s "$BASE/$p/" -o /tmp/gen_p.html 2>/dev/null || continue
    # ⚠ 必须优先"名字以项目名开头"的 tarball：bash 目录里还放着 readline-*.tar.gz，
    #   直接 sort -V | tail -1 会选到 readline（实测踩过）
    f=$(grep -o 'href="[^"]*\.tar\.xz"' /tmp/gen_p.html | sed 's/href="//; s/"$//' | grep -E "^$p-" | LC_ALL=C sort -V | tail -1)
    [ -n "${f:-}" ] || f=$(grep -o 'href="[^"]*\.tar\.gz"' /tmp/gen_p.html | sed 's/href="//; s/"$//' | grep -E "^$p-" | LC_ALL=C sort -V | tail -1)
    [ -n "${f:-}" ] || f=$(grep -o 'href="[^"]*\.tar\.xz"' /tmp/gen_p.html | sed 's/href="//; s/"$//' | LC_ALL=C sort -V | tail -1)
    [ -n "${f:-}" ] || f=$(grep -o 'href="[^"]*\.tar\.gz"' /tmp/gen_p.html | sed 's/href="//; s/"$//' | LC_ALL=C sort -V | tail -1)
    [ -n "${f:-}" ] || continue
    printf '%s|%s/%s/%s\n' "$p" "$BASE" "$p" "$f" >> "$OUT"
    n=$((n + 1))
done
rm -f /tmp/gen_idx.html /tmp/gen_p.html
echo "== 生成 $n 个项目 ⇒ $OUT =="
