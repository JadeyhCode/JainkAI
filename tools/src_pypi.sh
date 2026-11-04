#!/bin/sh
# tools/src_pypi.sh — 从**清华 PyPI 镜像**流式抽「Python 代码真知识」（§6.718）
#
# 为什么换源：Debian main 里"像 C 的"包只有 **20.7 GB ⇒ 约 45 M 条事实**（§6.717 实测），
# 到 7B 差 150 倍。PyPI 有 **901 797 个包**（`/simple/` 索引 47 MB，清华镜像 1.47 MB/s），
# 是不可比的体量；而且 tar.gz 里是纯源码，不用编译。
#
# 每个包：`/simple/<name>/` 页 → 取最新的 `.tar.gz` → 下载 → 解压 → 抽事实 → 记账 → 删源码。
# 事实（全部来自源码，不做推断）：
#   函数:<name>   定义于 文件:<pkg/相对路径>
#   类:<name>     定义于 文件:<pkg/相对路径>
#   方法:<Cls.m>  定义于 文件:<pkg/相对路径>
#   文件:<path>   导入   模块:<module>
#
# 断点续跑：`done.<k>.txt` 记包名；`facts.<k>.triples` 到 ROTATE_MB 就轮转成完成块，
# 交给 `tools/facts2shards.sh` 转 v2 + gzip（并发安全）。
#
# 用法：sh tools/src_pypi.sh <输出目录> [最多包数] [--slice k/N]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?用法: sh tools/src_pypi.sh <输出目录> [最多包数] [--slice k/N]}
MAXPKG=${2:-50}
SLICE_K=0
SLICE_N=1
MIRROR=${MIRROR:-https://pypi.tuna.tsinghua.edu.cn}
ROTATE_MB=${ROTATE_MB:-400}
shift 2 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
    --slice) SLICE_K=${2%%/*}; SLICE_N=${2##*/}; shift 2 ;;
    --mirror) MIRROR=$2; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$OUT" "$OUT/ptmp" || exit 1
FACTS="$OUT/facts.$SLICE_K.triples"
DONE="$OUT/done.$SLICE_K.txt"
IDX="$OUT/pypi_index.txt"
: >> "$FACTS"; : >> "$DONE"

if [ ! -s "$IDX" ]; then
    echo "== 取 PyPI 索引（47 MB，一次）=="
    curl -k -s --connect-timeout 15 --max-time 240 -o "$OUT/pypi_simple.html" "$MIRROR/simple/" || { echo "FAIL: 索引下不来"; exit 1; }
    # ⚠ 名字提取：`href="<name>/"`，名字里不会有 `/`；旧写法把结尾的 `/"` 留进了包名
    #   （实测：前 1 843 个包全部 404、0 条事实，查了半小时才看出包名尾巴多了两个字符）
    grep -o 'href="[^"/]*/"' "$OUT/pypi_simple.html" | sed 's|^href="||; s|/"$||' | grep -v '^$' > "$IDX"
    rm -f "$OUT/pypi_simple.html"
fi
echo "== PyPI 包 $(wc -l < "$IDX") 个（分片 $SLICE_K/$SLICE_N）；这次最多做 $MAXPKG 个 =="

# 只抽 .py 源码里的**声明类事实**（定义/导入/方法）——每条都来自真实代码
pyfacts() {   # $1=源码根目录 $2=包名 $3=输出文件
    find "$1" -name '*.py' -size -2M -print 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
        rel=${f#"$1"/}
        awk -v pkg="$2" -v file="$2/$rel" '
        BEGIN { cls = "" }
        {
            line = $0
            if (line ~ /^[ \t]*class[ \t]+[A-Za-z_][A-Za-z0-9_]*/) {
                sub(/^[ \t]*class[ \t]+/, "", line)
                sub(/[^A-Za-z0-9_].*$/, "", line)
                if (line != "") { print "类:" line "\t定义于\t文件:" file; cls = line }
                next
            }
            if (line ~ /^[ \t]*def[ \t]+[A-Za-z_][A-Za-z0-9_]*/) {
                indent = line
                sub(/[^ \t].*$/, "", indent)
                sub(/^[ \t]*def[ \t]+/, "", line)
                sub(/[^A-Za-z0-9_].*$/, "", line)
                if (line == "") next
                if (cls != "" && length(indent) > 0) print "方法:" cls "." line "\t定义于\t文件:" file
                else { print "函数:" line "\t定义于\t文件:" file; if (length(indent) == 0) cls = "" }
                next
            }
            if (line ~ /^[ \t]*from[ \t]+[A-Za-z0-9_.]+[ \t]+import/) {
                sub(/^[ \t]*from[ \t]+/, "", line); sub(/[ \t].*$/, "", line)
                print "文件:" file "\t导入\t模块:" line
                next
            }
            if (line ~ /^[ \t]*import[ \t]+[A-Za-z0-9_.]+/) {
                sub(/^[ \t]*import[ \t]+/, "", line); sub(/[,;# \t].*$/, "", line)
                print "文件:" file "\t导入\t模块:" line
            }
        }' "$f" >> "$3"
    done
}

did=0; lines=0; bytes=0; t0=$(date +%s)
while IFS= read -r pkg; do
    [ -n "${pkg:-}" ] || continue
    grep -q "^$pkg$" "$DONE" 2>/dev/null && continue
    [ "$did" -ge "$MAXPKG" ] && break
    # 包页：取最后一个 .tar.gz（通常是最新版本）
    pg="$OUT/ptmp/pg.html"
    curl -k -s --connect-timeout 15 --max-time 240 -o "$pg" "$MIRROR/simple/$pkg/" || { rm -f "$pg"; continue; }
    # ⚠ 包页里的链接**不是**按版本排的（`tail -1` 拿到过 requests-2.9.2 这种老版本）⇒ sort -V 取最新
    href=$(grep -o 'href="[^"]*\.tar\.gz[^"]*"' "$pg" | sed 's/^href="//; s/#.*//; s/"$//' | sort -V | tail -1)
    rm -f "$pg"
    [ -n "${href:-}" ] || { printf '%s\n' "$pkg" >> "$DONE"; continue; }   # 没有 sdist：记账跳过
    # 链接形如 `../../packages/64/20/<hash>/x.tar.gz`（相对 `/simple/<pkg>/`）⇒ 去掉前缀拼到镜像根
    case "$href" in
    ../../*) url="$MIRROR/${href#../../}" ;;
    /*)      url="$MIRROR$href" ;;
    *)       url="$MIRROR/$href" ;;
    esac
    tb="$OUT/ptmp/p.tgz"
    curl -k -sL --connect-timeout 15 --max-time 240 -o "$tb" "$url" || { rm -f "$tb"; continue; }
    sz=$(stat -c %s "$tb" 2>/dev/null || echo 0)
    if [ "$sz" -lt 1000 ] || [ "$(head -c 2 "$tb" | od -An -tx1 | tr -d ' \n')" != "1f8b" ]; then
        rm -f "$tb"; continue;          # 404/HTML/空包：**不记 done**，下次还试
    fi
    src="$OUT/ptmp/src"; rm -rf "$src"; mkdir -p "$src"
    tar -xzf "$tb" -C "$src" 2>/dev/null || { rm -rf "$src" "$tb"; continue; }
    : > "$OUT/ptmp/f.triples"
    pyfacts "$src" "$pkg" "$OUT/ptmp/f.triples"
    n=$(wc -l < "$OUT/ptmp/f.triples" 2>/dev/null || echo 0)
    cat "$OUT/ptmp/f.triples" >> "$FACTS"
    printf '%s\n' "$pkg" >> "$DONE"
    lines=$((lines + n)); bytes=$((bytes + sz))
    rm -rf "$src" "$tb" "$OUT/ptmp/f.triples"
    did=$((did + 1))
    el=$(( $(date +%s) - t0 )); [ "$el" -gt 0 ] || el=1
    [ $((did % 10)) -eq 0 ] && printf '  [%d] %-28s %6.0f KB ⇒ %6d 条；累计 %d 条 / %.0f 条/秒\n' \
        "$did" "$pkg" "$(awk -v b="$sz" 'BEGIN{printf "%.0f", b/1024}')" "$n" "$lines" \
        "$(awk -v a="$lines" -v t="$el" 'BEGIN{printf "%.0f", a/t}')"
    fsz=$(stat -c %s "$FACTS" 2>/dev/null || echo 0)
    if [ "$fsz" -ge $((ROTATE_MB * 1048576)) ]; then
        seq=$(ls "$OUT"/facts.$SLICE_K.[0-9]*.triples 2>/dev/null | wc -l)
        mv "$FACTS" "$OUT/facts.$SLICE_K.$(printf '%04d' "$seq").triples"
        echo "  ↻ 轮转：facts.$SLICE_K.$seq.triples（$((fsz / 1048576)) MB）"
    fi
    used=$(du -sm "$OUT" 2>/dev/null | cut -f1)
    [ "${used:-0}" -ge 40000 ] && { echo "  ⚠ 文本事实到 40 GB ⇒ 停（先跑 facts2shards）"; break; }
done < "$IDX"

echo
printf '== 本批：%d 个包 / 下载 %.1f MB / 事实 %d 条 ==\n' "$did" \
    "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')" "$lines"
printf '   %s（%d 行）；账本 %s（%d 包）\n' "$FACTS" "$(wc -l < "$FACTS")" "$DONE" "$(wc -l < "$DONE")"
