#!/bin/sh
# tools/src_npm.sh — 从 **npmmirror**（1.7 MB/s）流式抽「JS/TS 代码真知识」（§6.722）
#
# 为什么是 npm：这是唯一**够 7B 体量**的源（npm 注册表 **4 564 295 个包**，名单用
# `tools/npm_names.sh` 从 replicate.npmjs.com 分页拉；包本身从快的 npmmirror 下）。
#
# 每个包：`registry.npmmirror.com/<name>` 元数据 → 取 `dist-tags.latest` → 拼
# `registry.npmmirror.com/<name>/-/<basename>-<ver>.tgz` → 解压 → 抽事实 → 记账 → 删源码。
#
# 事实（全部来自源码，不做推断）：
#   函数:<name>    定义于 文件:<pkg/相对路径>   （function 声明 / 箭头函数赋值 / 方法）
#   类:<name>      定义于 文件:<pkg/相对路径>
#   接口:<name>    定义于 文件:<pkg/相对路径>   （TS interface）
#   文件:<path>    导入   模块:<module>         （import / require）
#
# 断点续跑 + 轮转（持久计数）+ 40 GB 护栏，与另外三条源同构。
#
# 用法：sh tools/src_npm.sh <输出目录> [最多包数] [--slice k/N] [--names 文件名]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?用法: sh tools/src_npm.sh <输出目录> [最多包数] [--slice k/N]}
MAXPKG=${2:-100}
SLICE_K=0
SLICE_N=1
REG=${REG:-https://registry.npmmirror.com}
ROTATE_MB=${ROTATE_MB:-400}
NAMES=${NAMES:-$OUT/npm_names.txt}
MIN_KB=${MIN_KB:-100}       # 只看 unpackedSize ≥ 这个值的包（**垃圾包占大多数**：实测前 1345 个包只出 6 947 条）
MIN_FILES=${MIN_FILES:-5}   # 文件数太少的一律跳过（一个包里连 5 个源文件都没有，抽不出什么）
shift 2 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
    --slice) SLICE_K=${2%%/*}; SLICE_N=${2##*/}; shift 2 ;;
    --names) NAMES=$2; shift 2 ;;
    *) shift ;;
    esac
done
mkdir -p "$OUT" "$OUT/ntmp" || exit 1
FACTS="$OUT/facts.n$SLICE_K.triples"
DONE="$OUT/done.n$SLICE_K.txt"
: >> "$FACTS"; : >> "$DONE"
[ -s "$NAMES" ] || { echo "FAIL: 没有包名清单 $NAMES（先跑 tools/npm_names.sh）"; exit 1; }

# 只抽**声明类事实**（定义/导入/接口）
jsfacts() {   # $1=源码根 $2=包名 $3=输出
    find "$1" -type f \( -name '*.js' -o -name '*.mjs' -o -name '*.cjs' -o -name '*.ts' -o -name '*.tsx' -o -name '*.jsx' \) \
        -size -2M -print 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
        rel=${f#"$1"/}
        awk -v pkg="$2" -v file="$2/$rel" '
        {
            line = $0
            # function name(...)
            if (match(line, /(^|[^A-Za-z0-9_$.])function[ \t]+[A-Za-z_$][A-Za-z0-9_$]*/)) {
                s = substr(line, RSTART, RLENGTH)
                sub(/.*function[ \t]+/, "", s)
                print "函数:" s "\t定义于\t文件:" file
            }
            # class Name
            if (match(line, /(^|[^A-Za-z0-9_$.])class[ \t]+[A-Za-z_$][A-Za-z0-9_$]*/)) {
                s = substr(line, RSTART, RLENGTH); sub(/.*class[ \t]+/, "", s)
                print "类:" s "\t定义于\t文件:" file
            }
            # TS interface Name
            if (match(line, /(^|[^A-Za-z0-9_$.])interface[ \t]+[A-Za-z_$][A-Za-z0-9_$]*/)) {
                s = substr(line, RSTART, RLENGTH); sub(/.*interface[ \t]+/, "", s)
                print "接口:" s "\t定义于\t文件:" file
            }
            # exports.foo = function(...) / module.exports.foo = ... / foo: function(...)
            if (match(line, /(exports|module\.exports|this)\.[A-Za-z_$][A-Za-z0-9_$]*[ \t]*=[ \t]*(async[ \t]+)?function/)) {
                s = substr(line, RSTART, RLENGTH)
                sub(/^(exports|module\.exports|this)\./, "", s); sub(/[ \t]*=.*$/, "", s)
                print "函数:" s "\t定义于\t文件:" file
            }
            # 方法简写：`  name(args) {`（行首缩进 + 标识符 + 括号 + 花括号）
            if (match(line, /^[ \t]+(async[ \t]+)?[A-Za-z_$][A-Za-z0-9_$]*[ \t]*\([^)]*\)[ \t]*\{/)) {
                s = substr(line, RSTART, RLENGTH)
                sub(/^[ \t]*(async[ \t]+)?/, "", s); sub(/[ \t(].*$/, "", s)
                if (s != "if" && s != "for" && s != "while" && s != "switch" && s != "catch" && s != "function")
                    print "方法:" s "\t定义于\t文件:" file
            }
            # const name = ( ... ) => | function
            if (match(line, /(const|let|var)[ \t]+[A-Za-z_$][A-Za-z0-9_$]*[ \t]*=[ \t]*(async[ \t]+)?(function|\(|[A-Za-z_$][A-Za-z0-9_$]*[ \t]*=>)/)) {
                s = substr(line, RSTART, RLENGTH)
                sub(/^(const|let|var)[ \t]+/, "", s); sub(/[ \t]*=.*$/, "", s)
                print "函数:" s "\t定义于\t文件:" file
            }
            # import ... from "mod"  /  require("mod")
            if (line ~ /^[ \t]*(import|export)[ \t]/ || line ~ /require[ \t]*\(/) {
                if (match(line, /(from[ \t]*|require[ \t]*\()[ \t]*[\047"][^\047"]+[\047"]/)) {
                    s = substr(line, RSTART, RLENGTH)
                    sub(/^[^ \t]*[ \t]*\(?[ \t]*/, "", s)
                    gsub(/[\047"]/, "", s)
                    if (s != "") print "文件:" file "\t导入\t模块:" s
                }
            }
        }' "$f" >> "$3"
    done
}

did=0; lines=0; bytes=0; t0=$(date +%s)
n=0
while IFS= read -r name; do
    n=$((n + 1))
    [ -n "${name:-}" ] || continue
    if [ "$SLICE_N" -gt 1 ]; then k=$((n % SLICE_N)); [ "$k" -ne "$SLICE_K" ] && continue; fi
    grep -q "^$name	" "$DONE" 2>/dev/null && continue
    [ "$did" -ge "$MAXPKG" ] && break
    # ⚠ 用 `/<name>/latest`（**只含最新版**的清单，3–29 KB / ~0.5 s），
    #   不要用 `/<name>`（整包历史：typescript 那个 **15 MB / 19 s** —— 这才是"4.5 s/包"的真凶）
    meta="$OUT/ntmp/m.json"
    curl -k -sL --connect-timeout 15 --max-time 240 -o "$meta" "$REG/$name/latest" || { rm -f "$meta"; continue; }
    ver=$(sed -n 's/.*"version":"\([^"]*\)".*/\1/p' "$meta" | head -1)
    tburl=$(sed -n 's/.*"tarball":"\([^"]*\)".*/\1/p' "$meta" | head -1)
    usize=$(sed -n 's/.*"unpackedSize":[ ]*\([0-9]*\).*/\1/p' "$meta" | head -1)
    fcount=$(sed -n 's/.*"fileCount":[ ]*\([0-9]*\).*/\1/p' "$meta" | head -1)
    rm -f "$meta"
    # 小包/垃圾包：只记账不下载（元数据已经付过钱了，别再花带宽和解压时间）
    if [ -n "${usize:-}" ] && [ "$usize" -lt $((MIN_KB * 1024)) ]; then
        printf '%s\t%s\tsmall\n' "$name" "${ver:-}" >> "$DONE"; continue
    fi
    if [ -n "${fcount:-}" ] && [ "$fcount" -lt "$MIN_FILES" ]; then
        printf '%s\t%s\tfewfiles\n' "$name" "${ver:-}" >> "$DONE"; continue
    fi
    [ -n "${ver:-}" ] || { printf '%s\t\n' "$name" >> "$DONE"; continue; }
    base=${name##*/}
    [ -n "${tburl:-}" ] || tburl="$REG/$name/-/$base-$ver.tgz"
    tb="$OUT/ntmp/p.tgz"
    # ⚠ npmmirror 的 tgz 是 **302 跳转**（不带 -L 只会拿到 80 字节的跳转页 ⇒ 看着像"包坏了"）
    curl -k -sL --connect-timeout 15 --max-time 240 -o "$tb" "$tburl" || { rm -f "$tb"; continue; }
    sz=$(stat -c %s "$tb" 2>/dev/null || echo 0)
    if [ "$sz" -lt 500 ] || [ "$(head -c 2 "$tb" | od -An -tx1 | tr -d ' \n')" != "1f8b" ]; then
        rm -f "$tb"; continue;                       # 不是 tgz：不记 done
    fi
    src="$OUT/ntmp/src"; rm -rf "$src"; mkdir -p "$src"
    tar -xzf "$tb" -C "$src" 2>/dev/null || { rm -rf "$src" "$tb"; continue; }
    : > "$OUT/ntmp/f.triples"
    jsfacts "$src" "$name" "$OUT/ntmp/f.triples"
    cnt=$(wc -l < "$OUT/ntmp/f.triples" 2>/dev/null || echo 0)
    cat "$OUT/ntmp/f.triples" >> "$FACTS"
    printf '%s\t%s\n' "$name" "$ver" >> "$DONE"
    lines=$((lines + cnt)); bytes=$((bytes + sz))
    rm -rf "$src" "$tb" "$OUT/ntmp/f.triples"
    did=$((did + 1))
    el=$(( $(date +%s) - t0 )); [ "$el" -gt 0 ] || el=1
    [ $((did % 25)) -eq 0 ] && printf '  [%d] %-32s %6.0f KB ⇒ %6d 条；累计 %d 条 / %.0f 条/秒\n' \
        "$did" "$name" "$(awk -v b="$sz" 'BEGIN{printf "%.0f", b/1024}')" "$cnt" "$lines" \
        "$(awk -v a="$lines" -v t="$el" 'BEGIN{printf "%.0f", a/t}')"
    fsz=$(stat -c %s "$FACTS" 2>/dev/null || echo 0)
    if [ "$fsz" -ge $((ROTATE_MB * 1048576)) ]; then
        ctr="$OUT/.rotr.$SLICE_K"
        seq=$(cat "$ctr" 2>/dev/null || echo 0); seq=$((seq + 1)); printf '%d\n' "$seq" > "$ctr"
        mv "$FACTS" "$OUT/facts.n$SLICE_K.$(printf '%04d' "$seq").triples"
        echo "  ↻ 轮转：facts.n$SLICE_K.$seq.triples（$((fsz / 1048576)) MB）"
    fi
    used=$(du -sm "$OUT" 2>/dev/null | cut -f1)
    [ "${used:-0}" -ge 40000 ] && { echo "  ⚠ 文本到 40 GB ⇒ 停（先跑 facts2shards）"; break; }
done < "$NAMES"

echo
printf '== 本批：%d 个 npm 包 / 下载 %.1f MB / 事实 %d 条 ==\n' "$did" \
    "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}')" "$lines"
