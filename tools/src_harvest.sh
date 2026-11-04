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
DIST=${DIST:-bookworm}      # 发行版代号：Debian bookworm / Ubuntu noble、jammy…
MIRROR="https://mirrors.tuna.tsinghua.edu.cn/debian"
SLICE_K=0
SLICE_N=1
MAXMB=${MAXMB:-20}          # 单个 orig 包上限（MB）：C 库/工具一般 1–10 MB；再大多半是数据/美术资源
                            # （实测：60 MB 上限时撞上 apertium 语言包/Java 包，速度从 11.8k 掉到 1.7k 行/秒）
CAPGB=${CAPGB:-40}          # 事实**文本**上限（GB）：超了停下（等 facts2shards 转成分片再续）
ROTATE_SEC=${ROTATE_SEC:-420}  # 不管大小，距上次轮转超过这么多秒也切块（防"文件涨不到阈值"）
ROTATE_MB=${ROTATE_MB:-20}  # 单个 facts 文件到这么大就**轮转**成"已完成块"，
                            # ⚠ 这个值要跟着**并发数**调：20 路时每个文件只涨 ~435 行/秒，
                            #   400 MB（700 万行）要 4.5 小时才轮转 ⇒ 转换器整段时间空转（实测积压 29 M 行）
                            #   40 MB ≈ 70 万行 ⇒ 约 27 分钟一块
                            # 交给 facts2shards 去转 —— 转换器只碰**没人写**的文件，不会和抓取抢
                            # ⚠ 不设护栏的话：7e9 × ~54 B = 378 GB 文本，本机只剩 274 GB（会被写爆）
shift 2 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
    --dist) DIST=$2; shift 2 ;;
    --suite) SUITE_DIR=$2; shift 2 ;;
    --mirror) MIRROR=$2; shift 2 ;;
    --slice) SLICE_K=${2%%/*}; SLICE_N=${2##*/}; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$OUT" "$OUT/tmp" || exit 1
FACTS="$OUT/facts.$SLICE_K.triples"
DONE="$OUT/done.$SLICE_K.txt"
DONE_ALL="$OUT/done.all.txt"   # 全局去重表：换分片数 N 重跑时不会重复处理同一个包
: >> "$FACTS"; : >> "$DONE"; : >> "$DONE_ALL"

# 索引（12 MB，一次）：包名 + orig tar 路径
IDX="$OUT/Sources.gz"
# ⚠ 索引文件（14 MB）只在缺失或**超过 1 小时**时重建：36 个续跑器频繁重启 worker 时，
#   每次都 zcat 14 MB + awk 过滤会把 IO/CPU 吃光（实测：85 个 worker 只产出 971 边/秒）。
IDX_STALE=0
if [ -s "$IDX" ]; then
    now_t=$(date +%s); idx_t=$(stat -c %Y "$IDX" 2>/dev/null || echo 0)
    [ $((now_t - idx_t)) -gt 86400 ] && IDX_STALE=1
fi
# ⚠ 索引文件可能是**半截**的：下载被打断（curl 超时/峰段冻结）会留下截断的 .gz，
#   而脚本只在"缺失"时才重下 ⇒ 永远用着坏索引、worker 一个包都找不到（实测：ubucorpus 的
#   Sources.gz 截断，索引建出来是 0 行，日志一片"本批：0 个包"）。
if [ -s "$IDX" ] && ! gzip -t "$IDX" 2>/dev/null; then
    echo "  ⚠ 索引文件损坏（截断）⇒ 重下"
    rm -f "$IDX"
fi
if [ ! -s "$IDX" ] || [ "$IDX_STALE" -eq 1 ]; then
    echo "== 取源码索引 =="
    # ⚠ 索引 14–24 MB，慢速下 240 s 下不完 ⇒ 被截断（实测 ubucorpus 连续两次坏索引，
        #   导致索引建出 0 行、worker 一个包都找不到）。索引单独给 1800 s + gzip -t 校验。
    curl -k -s --connect-timeout 20 --max-time 1800 -o "$IDX" "$MIRROR/dists/$DIST/$SUITE_DIR/source/Sources.gz" \
        || { echo "FAIL: 索引下不来"; exit 1; }
    gzip -t "$IDX" 2>/dev/null || { echo "FAIL: 索引下载不完整（gzip -t 失败）"; rm -f "$IDX"; exit 1; }
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
        # 语言包/文档/数据包：抽不出 C 事实，白花下载时间（实测踩过）
        # 语言包/文档/数据包：抽不出 C 事实，纯烧带宽（实测 Ubuntu universe 里一屏幕的 "⇒ 0 行"）
        if (pkg ~ /^(apertium-|texlive-|tex-|fonts-|libjs-|libreoffice-|gcc-|linux-|nvidia-|python3?-|libghc-|libocaml-|libmono-|libjava|libcommons-|libperl|perl|bioperl|liblingua-|node-|nodejs|ruby|ruby-|php-|libphp|golang-|golang|rust-|rustc|librust|r-cran-|r-bioc-|swi-prolog|raku-|ocaml|mono-|jekyll|libapt-pkg-doc)/) next
        # 后缀型（Ubuntu universe 里占比极大）：libfoo-perl / libbar-java / python3-xxx 之类
        if (pkg ~ /-(perl|java|python|python3|ruby|ocaml|ghc|php|julia|lua|tcl|mono|js|node|font|fonts|data|doc|docs|dbg|dbgsym|theme|icon|icons|sound|sounds|wallpaper|wallpapers|example|examples|test|tests|common|all|kde|gnome)$/) next
        if (pkg ~ /(-doc$|-docs$|-dbg$|-dbgsym$|-dev-doc$)/) next
        if (sz > maxmb * 1048576) next
        c = substr(pkg, 1, 1); if (substr(pkg,1,3) == "lib") c = "lib" substr(pkg, 4, 1)
        # **按预期密度排序**：lib* / *-dev 包带大量头文件（实测 noble main 18 384 行/MB vs universe 7 516）
        # 排在最前面 ⇒ 同样带宽下先吃到"密"的包。0 = 高密度，1 = 其它。
        prio = 1
        if (pkg ~ /^lib/ || pkg ~ /-dev$/) prio = 0
        line = prio "\t" pkg "\tpool/" sec "/" c "/" pkg "/" f
        if (sn > 1) { k = NR % sn; if (k != sk) next }
        print line
        next
    }
    inf && /^[^ ]/ { inf = 0 }
' | LC_ALL=C sort -t"	" -k1,1n -s | cut -f2- > "$OUT/index.$SLICE_K.txt.new"
    mv "$OUT/index.$SLICE_K.txt.new" "$OUT/index.$SLICE_K.txt"   # 原子替换，别让别的进程读到半截
# （已做过的包改由**预生成的索引**剔除：脚本内过滤实测会把 676 行的片压成 0 行，原因未明；
#   `tools/rebuild_index.sh` 手工流水线验证可用。）
NIDX=$(wc -l < "$OUT/index.$SLICE_K.txt")
echo "== 索引里 $(wc -l < "$OUT/index.$SLICE_K.txt") 个源码包（分片 $SLICE_K/$SLICE_N）；这次最多做 $MAXPKG 个 =="

did=0; tried=0; lines=0; bytes=0; t0=$(date +%s)
while IFS='	' read -r pkg path; do
    [ -n "${pkg:-}" ] || continue
    grep -q "^$pkg	" "$DONE_ALL" 2>/dev/null && continue
    [ "$did" -ge "$MAXPKG" ] && break
    tried=$((tried + 1))
    url="$MIRROR/$path"
    tb="$OUT/tmp/$(basename "$path")"
    curl -k -s --connect-timeout 20 --max-time 900 -o "$tb" "$url" || { rm -f "$tb"; continue; }
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
    printf '%s\t%s\n' "$pkg" "$n" >> "$DONE_ALL"
    lines=$((lines + n)); bytes=$((bytes + sz))
    rm -rf "$src" "$tb" "$OUT"/tmp/*.triples
    did=$((did + 1))
    # 文件轮转：到 ROTATE_MB **或** 距上次轮转超过 ROTATE_SEC 就切成"已完成块"
    # （只按大小会出事：worker 多、包小时每个文件涨得慢，块迟迟不生成 ⇒ 转换器空转，实测卡过 4 次）
    last=$(cat "$OUT/.rt.$SLICE_K" 2>/dev/null || echo 0)
    now=$(date +%s)
    fsz=$(stat -c %s "$FACTS" 2>/dev/null || echo 0)
    if [ "$fsz" -lt $((ROTATE_MB * 1048576)) ] && [ $((now - ${last:-0})) -lt "${ROTATE_SEC:-600}" ]; then
        fsz=0
    fi
    if [ "$fsz" -ge $((ROTATE_MB * 1048576)) ]; then
        # ⚠ 序号必须**持久化**：用"已存在文件个数"当序号，转换完删掉后会**重用同一个名字**
        #   ⇒ 新块撞上旧块的 .done 标记，转换器永远跳过它（实测：两个 400 MB 块卡在那儿）
        # ⚠ 名字里必须带**时间戳**：只靠序号时，脚本改过/计数文件丢过之后序号会重用，
        #   新块撞上旧块的 `.done` 标记 ⇒ 转换器永远跳过它（实测发生过两次，卡了 3.3 GB 文本）
        ctr="$OUT/.rot.$SLICE_K"
        seq=$(cat "$ctr" 2>/dev/null || echo 0)
        seq=$((seq + 1))
        printf '%d\n' "$seq" > "$ctr"
        mv "$FACTS" "$OUT/facts.$SLICE_K.$(date +%s).$(printf '%04d' "$seq").triples"
        date +%s > "$OUT/.rt.$SLICE_K"
        echo "  ↻ 轮转：facts.$SLICE_K.$seq.triples（$((fsz / 1048576)) MB）"
    fi
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
done < "$OUT/index.$SLICE_K.txt"

echo
printf '== 本批：%d 个包 / 下载 %.1f MB / 事实 %d 行 / %.0f 行每秒 ==\n' \
    "$did" "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')" "$lines" \
    "$(awk -v a="$lines" -v t="$(( $(date +%s) - t0 > 0 ? $(date +%s) - t0 : 1 ))" 'BEGIN{printf "%.0f", a/t}')"
printf '   %s（%d 行）；账本 %s（%d 个包）\n' "$FACTS" "$(wc -l < "$FACTS")" "$DONE" "$(wc -l < "$DONE")"
