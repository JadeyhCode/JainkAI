#!/bin/sh
# tools/src_harvest.sh — 从**快镜像**流式抽「代码真知识」（Debian/Ubuntu 源码包；§6.715）
#
# 为什么是这条路：wikimedia 只有 13–70 KB/s（§6.714），而清华 TUNA/阿里/魔搭 **2.6–3.1 MB/s**
# （52 倍）。真实源码的密度实测 **2 182 行事实/MB**（coreutils 59 MB ⇒ 128 737 行）
# ⇒ 7B 代码知识 ≈ **3 133 GB 源码 ≈ 12.4 天**（3 MB/s），所以必须**流式**：
# 下载 → 解压 → 抽事实 → 记一行账 → **删源码**（不留 3 TB）。
#
# 每个包出两份事实（都是 §6.709/§6.710 用过的抽取器，口径没变）：
#   · `headers2triples.sh`（.h → 函数原型/头/宏/枚举/字段）—— 快，无编译
#   · `c2triples.sh --ngate`（.c → 函数片段）—— **不带编译门**（规模优先；门的版本在 §6.710）
#
# 断点续跑：`done.txt` 记"包名 + 事实行数 + 字节数"，已做过的包**跳过**。
#
# 用法：sh tools/src_harvest.sh <输出目录> [最多包数] [--suite main|universe] [--mirror URL]
#   $OUT/facts.triples   —— 事实（`:ingest` 直接吃）
#   $OUT/done.txt        —— 每包一行：包名 <tab> 事实行数 <tab> 源字节
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?用法: sh tools/src_harvest.sh <输出目录> [最多包数]}
MAXPKG=${2:-5}
SUITE_DIR="main"
MIRROR="https://mirrors.tuna.tsinghua.edu.cn/debian"
SLICE_K=0
SLICE_N=1
MAXMB=${MAXMB:-60}          # 单个 orig 包上限（MB）：超大的多半是数据/美术资源，不是代码
CAPGB=${CAPGB:-40}          # 本目录事实文本的上限（GB）：超了**停下**，等转成 v2 二进制（~10 B/条）再继续
                            # ⚠ 不设护栏的话：7e9 × ~54 B = 378 GB 文本，本机只剩 274 GB（会被写爆）
shift 2 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
    --suite) SUITE_DIR=$2; shift 2 ;;
    --mirror) MIRROR=$2; shift 2 ;;
    --slice) SLICE_K=${2%%/*}; SLICE_N=${2##*/}; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$OUT" "$OUT/tmp" || exit 1
FACTS="$OUT/facts.$SLICE_K.triples"
DONE="$OUT/done.$SLICE_K.txt"
: >> "$FACTS"; : >> "$DONE"

# 索引（12 MB，一次）：包名 + orig tar 路径
IDX="$OUT/Sources.gz"
if [ ! -s "$IDX" ]; then
    echo "== 取源码索引 =="
    curl -k -s -o "$IDX" "$MIRROR/dists/bookworm/$SUITE_DIR/source/Sources.gz" || { echo "FAIL: 索引下不来"; exit 1; }
fi
# Debian pool 布局：pool/<section>/<首字母或 lib+首字母>/<包名>/
zcat "$IDX" | awk -v sec="$SUITE_DIR" -v maxmb="$MAXMB" -v sk="$SLICE_K" -v sn="$SLICE_N" '
    /^Package:/ { pkg = $2 }
    /^Files:/ { inf = 1; next }
    inf && /\.orig\.tar\./ {
        n = split($3, a, "/"); f = a[n]; sz = $2 + 0
        inf = 0
        # 只留"像代码"的包：跳过数据/文档/字体/图标/主题/声音/样例
        if (pkg ~ /(-data$|-docs?$|-fonts?$|-icons?$|-themes?$|-sounds?$|-wallpapers?$|-assets$|-examples?$|-tests?$|-common$)/) next
        if (sz > maxmb * 1048576) next
        c = substr(pkg, 1, 1); if (substr(pkg,1,3) == "lib") c = "lib" substr(pkg, 4, 1)
        line = pkg "\tpool/" sec "/" c "/" pkg "/" f
        if (sn > 1) { k = NR % sn; if (k != sk) next }
        print line
        next
    }
    inf && /^[^ ]/ { inf = 0 }
' > "$OUT/index.txt"
NIDX=$(wc -l < "$OUT/index.txt")
echo "== 索引里 $(wc -l < "$OUT/index.txt") 个源码包；这次最多做 $MAXPKG 个 =="

did=0; tried=0; lines=0; bytes=0; t0=$(date +%s)
while IFS='	' read -r pkg path; do
    [ -n "${pkg:-}" ] || continue
    grep -q "^$pkg	" "$DONE" 2>/dev/null && continue
    [ "$did" -ge "$MAXPKG" ] && break
    tried=$((tried + 1))
    url="$MIRROR/$path"
    tb="$OUT/tmp/$(basename "$path")"
    curl -k -s -o "$tb" "$url" || { rm -f "$tb"; continue; }
    sz=$(stat -c %s "$tb" 2>/dev/null || echo 0)
    [ "$sz" -gt 1000 ] || { rm -f "$tb"; continue; }
    src="$OUT/tmp/src_$pkg"; rm -rf "$src"; mkdir -p "$src"
    tar -xf "$tb" -C "$src" 2>/dev/null || { rm -rf "$src" "$tb"; continue; }
    : > "$OUT/tmp/f.triples"
    INC="$src" sh "$ROOT/tools/headers2triples.sh" "$OUT/tmp/h.triples" 100000000 > /dev/null 2>&1
    [ -s "$OUT/tmp/h.triples" ] && cat "$OUT/tmp/h.triples" >> "$OUT/tmp/f.triples"
    sh "$ROOT/tools/c2triples.sh" "$OUT/tmp/c.triples" --ngate "$src" > /dev/null 2>&1
    [ -s "$OUT/tmp/c.triples" ] && cat "$OUT/tmp/c.triples" >> "$OUT/tmp/f.triples"
    n=$(wc -l < "$OUT/tmp/f.triples" 2>/dev/null || echo 0)
    cat "$OUT/tmp/f.triples" >> "$FACTS"
    printf '%s\t%s\t%s\n' "$pkg" "$n" "$sz" >> "$DONE"
    lines=$((lines + n)); bytes=$((bytes + sz))
    rm -rf "$src" "$tb" "$OUT"/tmp/*.triples
    did=$((did + 1))
    # 磁盘护栏（每次包级检查，4 路并行最多超出一个包的量）
    used=$(du -sm "$OUT" 2>/dev/null | cut -f1)
    if [ "${used:-0}" -ge $((CAPGB * 1024)) ]; then
        echo "  ⚠ 事实文本已到 ${used} MB ≥ ${CAPGB} GB ⇒ 停下（先转 v2 二进制再续跑）"
        break
    fi
    el=$(( $(date +%s) - t0 ))
    [ "$el" -gt 0 ] || el=1
    printf '  [%d] %-24s %6.1f MB 源码 ⇒ %7d 行；累计 %d 行 / %.0f 行/秒 / 下载 %.1f MB\n' \
        "$did" "$pkg" "$(awk -v b="$sz" 'BEGIN{printf "%.1f", b/1048576}')" "$n" "$lines" \
        "$(awk -v a="$lines" -v t="$el" 'BEGIN{printf "%.0f", a/t}')" \
        "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')"
done < "$OUT/index.txt"

echo
printf '== 本批：%d 个包 / 下载 %.1f MB / 事实 %d 行 / %.0f 行每秒 ==\n' \
    "$did" "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')" "$lines" \
    "$(awk -v a="$lines" -v t="$(( $(date +%s) - t0 > 0 ? $(date +%s) - t0 : 1 ))" 'BEGIN{printf "%.0f", a/t}')"
printf '   %s（%d 行）；账本 %s（%d 个包）\n' "$FACTS" "$(wc -l < "$FACTS")" "$DONE" "$(wc -l < "$DONE")"
