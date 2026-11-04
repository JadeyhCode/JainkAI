#!/bin/sh
# tools/src_gems.sh — 从**清华 RubyGems 镜像**流式抽「Ruby 代码真知识」（§6.721）
#
# 为什么加它：Debian（C）20.7 GB、PyPI（Python）~270 GB 之外，RubyGems 还有 ~20 万个 gem、
# 镜像 **1.28 MB/s**；`.gem` 是 tar（内含 `data.tar.gz`），解出来就是 `.rb` 源码，不用编译。
#
# 每个 gem（`<name>-<version>.gem`）：
#   函数:<name>  定义于 文件:<gem/相对路径>       （def）
#   类:<name>    定义于 文件:<gem/相对路径>       （class）
#   模块:<name>  定义于 文件:<gem/相对路径>       （module）
#   文件:<path>  导入   模块:<lib>               （require）
#   包:<gem>     依赖   包:<dep>                 （.gemspec 的 add_*dependency）
#
# 断点续跑 + 轮转 + 40 GB 护栏，与 Debian/PyPI 两条同构；转换交给 tools/facts2shards.sh。
#
# 用法：sh tools/src_gems.sh <输出目录> [最多包数] [--slice k/N]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?用法: sh tools/src_gems.sh <输出目录> [最多包数] [--slice k/N]}
MAXPKG=${2:-50}
SLICE_K=0
SLICE_N=1
MIRROR=${MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/rubygems}
ROTATE_MB=${ROTATE_MB:-400}
shift 2 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
    --slice) SLICE_K=${2%%/*}; SLICE_N=${2##*/}; shift 2 ;;
    --mirror) MIRROR=$2; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$OUT" "$OUT/gtmp" || exit 1
FACTS="$OUT/facts.$SLICE_K.triples"
DONE="$OUT/done.$SLICE_K.txt"
IDX="$OUT/gems_index.txt"
: >> "$FACTS"; : >> "$DONE"

if [ ! -s "$IDX" ]; then
    echo "== 取 RubyGems 索引（versions，约 24 MB）=="
    curl -k -s --connect-timeout 15 --max-time 240 -o "$IDX.raw" "$MIRROR/versions" || { echo "FAIL: 索引下不来"; exit 1; }
    # 每行：<名字> <版本> <校验/平台>；跳过头两行与空行
    awk 'NR > 2 && NF >= 2 && $1 != "" { print $1 "\t" $2 }' "$IDX.raw" > "$IDX"
    rm -f "$IDX.raw"
fi
echo "== RubyGems 包 $(wc -l < "$IDX") 个（分片 $SLICE_K/$SLICE_N）；这次最多做 $MAXPKG 个 =="

# 抽 .rb / .gemspec 里的**声明类事实**
rubyfacts() {   # $1=源码根 $2=gem名 $3=输出
    find "$1" -type f \( -name '*.rb' -o -name '*.gemspec' -o -name '*.rake' \) -size -2M -print 2>/dev/null \
        | LC_ALL=C sort | while IFS= read -r f; do
        rel=${f#"$1"/}
        awk -v gem="$2" -v file="$2/$rel" '
        {
            line = $0
            if (line ~ /^[ \t]*def[ \t]+(self\.)?[A-Za-z_][A-Za-z0-9_]*[?!]?/) {
                sub(/^[ \t]*def[ \t]+/, "", line)
                if (line ~ /^self\./) sub(/^self\./, "", line)
                sub(/[^A-Za-z0-9_?!].*$/, "", line)
                if (line != "") print "函数:" line "\t定义于\t文件:" file
                next
            }
            if (line ~ /^[ \t]*class[ \t]+[A-Z][A-Za-z0-9_:]*/) {
                sub(/^[ \t]*class[ \t]+/, "", line); sub(/[^A-Za-z0-9_:].*$/, "", line)
                if (line != "") print "类:" line "\t定义于\t文件:" file
                next
            }
            if (line ~ /^[ \t]*module[ \t]+[A-Z][A-Za-z0-9_:]*/) {
                sub(/^[ \t]*module[ \t]+/, "", line); sub(/[^A-Za-z0-9_:].*$/, "", line)
                if (line != "") print "模块:" line "\t定义于\t文件:" file
                next
            }
            if (line ~ /^[ \t]*require(_relative)?[ \t]+["\047]/) {
                sub(/^[ \t]*require(_relative)?[ \t]+["\047]/, "", line)
                sub(/["\047].*$/, "", line)
                if (line != "") print "文件:" file "\t导入\t模块:" line
                next
            }
            # .gemspec 依赖：包:X 依赖 包:dep
            if (line ~ /^[ \t]*[a-z_]*add_(runtime_)?dependency[ \t]+["\047]/) {
                sub(/^[ \t]*[a-z_]*add_(runtime_)?dependency[ \t]+["\047]/, "", line)
                sub(/["\047].*$/, "", line)
                if (line != "") print "包:" gem "\t依赖\t包:" line
            }
        }' "$f" >> "$3"
    done
}

did=0; lines=0; bytes=0; t0=$(date +%s)
while IFS='	' read -r name ver; do
    [ -n "${name:-}" ] && [ -n "${ver:-}" ] || continue
    key="$name-$ver"
    grep -q "^$key$" "$DONE" 2>/dev/null && continue
    [ "$did" -ge "$MAXPKG" ] && break
    url="$MIRROR/gems/$key.gem"
    tb="$OUT/gtmp/p.gem"
    curl -k -s --connect-timeout 15 --max-time 240 -o "$tb" "$url" || { rm -f "$tb"; continue; }
    sz=$(stat -c %s "$tb" 2>/dev/null || echo 0)
    if [ "$sz" -lt 500 ] || [ "$(file -b "$tb" 2>/dev/null | cut -c1-3)" != "POS" ]; then
        rm -f "$tb"; continue;                    # 404/不是 tar：不记 done
    fi
    src="$OUT/gtmp/src"; rm -rf "$src"; mkdir -p "$src"
    tar -xf "$tb" -C "$src" 2>/dev/null || { rm -rf "$src" "$tb"; continue; }
    if [ -f "$src/data.tar.gz" ]; then
        tar -xzf "$src/data.tar.gz" -C "$src" 2>/dev/null || true
    fi
    : > "$OUT/gtmp/f.triples"
    rubyfacts "$src" "$name" "$OUT/gtmp/f.triples"
    n=$(wc -l < "$OUT/gtmp/f.triples" 2>/dev/null || echo 0)
    cat "$OUT/gtmp/f.triples" >> "$FACTS"
    printf '%s\n' "$key" >> "$DONE"
    lines=$((lines + n)); bytes=$((bytes + sz))
    rm -rf "$src" "$tb" "$OUT/gtmp/f.triples"
    did=$((did + 1))
    el=$(( $(date +%s) - t0 )); [ "$el" -gt 0 ] || el=1
    [ $((did % 10)) -eq 0 ] && printf '  [%d] %-30s %6.0f KB ⇒ %5d 条；累计 %d 条 / %.0f 条/秒\n' \
        "$did" "$key" "$(awk -v b="$sz" 'BEGIN{printf "%.0f", b/1024}')" "$n" "$lines" \
        "$(awk -v a="$lines" -v t="$el" 'BEGIN{printf "%.0f", a/t}')"
    fsz=$(stat -c %s "$FACTS" 2>/dev/null || echo 0)
    if [ "$fsz" -ge $((ROTATE_MB * 1048576)) ]; then
        ctr="$OUT/.rot.$SLICE_K"
        seq=$(cat "$ctr" 2>/dev/null || echo 0); seq=$((seq + 1)); printf '%d\n' "$seq" > "$ctr"
        mv "$FACTS" "$OUT/facts.$SLICE_K.$(printf '%04d' "$seq").triples"
        echo "  ↻ 轮转：facts.$SLICE_K.$seq.triples（$((fsz / 1048576)) MB）"
    fi
    used=$(du -sm "$OUT" 2>/dev/null | cut -f1)
    [ "${used:-0}" -ge 40000 ] && { echo "  ⚠ 文本到 40 GB ⇒ 停（先跑 facts2shards）"; break; }
done < "$IDX"

echo
printf '== 本批：%d 个 gem / 下载 %.1f MB / 事实 %d 条 ==\n' "$did" \
    "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')" "$lines"
printf '   %s（%d 行）；账本 %s（%d 个）\n' "$FACTS" "$(wc -l < "$FACTS")" "$DONE" "$(wc -l < "$DONE")"
