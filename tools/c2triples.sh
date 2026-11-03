#!/bin/sh
# tools/c2triples.sh — 从本机 **.c 源码**抽「函数片段」知识（阶段四扩图：会写代码的那条腿；§6.710）
#
# 为什么这条腿最值钱：`headers2triples.sh` 只抽到"有这个函数、原型"（**声明**），
# 而合成器真正要拼的是**函数体**（片段）。现在图里只有 3 个手写片段
# （`is_prime` / `bubble` / `count_primes_to`），所以"会写代码"的天花板就是这 3 个。
#
# 输出三元组（TAB 分隔，`:ingest` 直接吃；函数体是**一条记录跨多个物理行**，行尾 `\` 续行）：
#   片段:<文件>_<函数>  签名   <一行签名>
#   片段:<文件>_<函数>  来自   <源文件路径>
#   片段:<文件>_<函数>  行数   <n>
#   片段:<文件>_<函数>  需要   <源文件里的 #include 行>（最多 8 条；合成时要一起带上）
#   片段:<文件>_<函数>  独立   是|否（**只用常用头**就能编过 = 是）
#   片段:<文件>_<函数>  代码   <签名 + 函数体>
#
# **两道门**（图里的知识必须是能用的，不是"看起来像代码"）：
#   B 门（必需）：片段 + 它源文件里的 `#include` + 七个常用头 ⇒ `gcc -std=c11 -fsyntax-only` 过；
#   A 门（加分）：同上但**只带常用头**（不借源文件的 include）⇒ 过了才标 `独立 是`。
# 连 B 门都过不了的片段**不进图**（缺类型/缺宏，拼到别的程序里也编不过）。
# `main` 不收：模板（`模板:命令行整数`）负责 main，片段只提供辅助函数。
#
# 用法：sh tools/c2triples.sh <输出文件> [根目录…] [--ngate]
#   默认根：/usr/share/doc /usr/lib/python3/dist-packages /usr/src /home/jadeth/llama-cpp
#   `--ngate` 只抽不编（量产量用；**正常建知识不要用**）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/code/knowledge/c2.triples}
shift 2>/dev/null || true
NGATE=0
DIRS=""
for a in "$@"; do
    case "$a" in
    --ngate) NGATE=1 ;;
    *) DIRS="$DIRS $a" ;;
    esac
done
[ -n "$DIRS" ] || DIRS="/usr/share/doc /usr/lib/python3/dist-packages /usr/src /home/jadeth/llama-cpp"

# ⚠ 工作目录必须**每次唯一**：两个实例共用 `_t/c2` 会互相清掉（实测：一个后台抽全量、
#   一个跑夹具测试 ⇒ `awk: cannot open idx_g.txt` / `chunk_am 不存在`，看起来像脚本坏了）。
WORK=$(mktemp -d "$ROOT/_t/c2.XXXXXX") || exit 1
trap '[ "${C2_KEEP:-0}" = 1 ] || rm -rf "$WORK"' EXIT INT TERM
RAW="$WORK/raw.triples"
: > "$RAW"

# 常用头：只用来过门（不算进知识）。片段要是靠这些就能编过，才叫"独立"。
cat > "$WORK/preamble.h" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
EOF

# ---------- 1) 抽：顶层函数（`{` 与签名同行，或签名一行、`{` 下一行） ----------
nf_file=0
for d in $DIRS; do
    [ -d "$d" ] || { echo "  跳过（没有这个目录）：$d"; continue; }
    # 只排除**仓库顶层**的 .c（main.c / train*.c 是本项目自己）；测试夹具在 tests/ 下，要能扫到
    # find 的 `-path` 里 `*` **能跨 `/`**：`$ROOT/*.c` 会把 tests/fixtures/*.c 也排掉（实测）。
    # 所以"排除仓库自己的 .c"放到 while 循环里按 dirname 判。
    find "$d" -name '*.c' -print 2>/dev/null | LC_ALL=C sort > "$WORK/files.txt"
    while IFS= read -r f; do
        [ "$(dirname "$f")" = "$ROOT" ] && continue            # 仓库顶层的 main.c / train*.c 不收
        [ "${f#$ROOT/_t/}" != "$f" ] && continue                 # 自己的临时目录也不收
        nf_file=$((nf_file + 1))
        base=${f##*/}
        base=${base%.c}
        grep -E '^[ \t]*#[ \t]*include' "$f" 2>/dev/null | head -8 > "$WORK/inc.txt" || true
        awk -v src="$f" -v base="$base" -v incfile="$WORK/inc.txt" '
        function fnname(s,   q, head, n, parts, i) {
            q = index(s, "(")
            if (q <= 1) return ""
            head = substr(s, 1, q - 1)
            sub(/[ \t]+$/, "", head)
            n = split(head, parts, /[ \t\*]/)
            for (i = n; i >= 1; i--) if (parts[i] ~ /^[A-Za-z_][A-Za-z0-9_]*$/) return parts[i]
            return ""
        }
        function emit(   nm, id, i, k, clean) {
            if (!collecting) return
            collecting = 0
            nm = fnname(sig)
            if (nm == "" || nm == "main") return
            if (nline < 3) return
            clean = base
            gsub(/[^A-Za-z0-9_]/, "_", clean)
            id = "片段:" clean "_" nm
            print id "\t签名\t" sig
            print id "\t来自\t" src
            print id "\t行数\t" nline
            for (k = 1; k <= ninc; k++) print id "\t需要\t" inc[k]
            # ⚠ 续行是"**一条记录跨多个物理行**"：只有第一行带 `片段:…<TAB>代码<TAB>` 前缀，
            #   后面的行必须**裸写**（`::ingest` 的续行是把下一行整行接上，带前缀就一起进客体了）。
            print id "\t代码\t" (nline > 1 ? buf[1] "\\" : buf[1])
            for (i = 2; i <= nline; i++) print (i < nline ? buf[i] "\\" : buf[i])
        }
        BEGIN {
            while ((getline l < incfile) > 0) inc[++ninc] = l
            close(incfile)
            collecting = 0; pending = 0; nline = 0
        }
        {
            line = $0
            if (!collecting) {
                if (line ~ /^[A-Za-z_].*\(.*\)[ \t]*\{[ \t]*$/ && line !~ /;/ && line !~ /^#/) {
                    sig = line; sub(/[ \t]*\{[ \t]*$/, "", sig)
                    collecting = 1; nline = 0; buf[++nline] = line
                    next
                }
                if (line ~ /^[A-Za-z_].*\)[ \t]*$/ && line !~ /;/ && line !~ /^#/) {
                    pendingsig = line; pending = 1; next
                }
                if (pending && line ~ /^\{[ \t]*$/) {
                    sig = pendingsig; pending = 0
                    collecting = 1; nline = 0
                    buf[++nline] = sig; buf[++nline] = line
                    next
                }
                pending = 0
                next
            }
            buf[++nline] = line
            if (line ~ /^\}[ \t]*$/) emit()
        }
        END { if (collecting) emit() }' "$f" >> "$RAW"
    done < "$WORK/files.txt"
done

NF=$(grep -c '	签名	' "$RAW" || true)
NF=${NF:-0}
echo "  抽到片段 $NF 个（扫了 $nf_file 个 .c）"
if [ "$NF" -eq 0 ]; then
    echo "FAIL: 一个片段都没抽到（根目录对吗？）"
    exit 1
fi

# ---------- 2) 两道门 + 组装 ----------
if [ "$NGATE" = 0 ]; then
    # 2a) 按块拆成每片段一个 .c：g_<i>.c = 常用头 + 源文件 include + 函数体；h_<i>.c = 常用头 + 函数体
    # 每个片段有没有 `需要`（源文件 include）：没有的才用"常用头"兜底，
    # ⚠ 有 include 时**不能再塞常用头**：alsa 的 `asoundlib.h` 和 `stdint.h` 一起进就
    #   `redefinition of 'struct timespec'`（实测 92 个片段里 91 个因此被误杀）。
    awk -F'\t' '/	签名	/ { i++ } /	需要	/ { has[i] = 1 } END { for (k = 1; k <= i; k++) print k "\t" (has[k] ? 1 : 0) }' \
        "$RAW" > "$WORK/hasinc.txt"
    awk -v pre="$WORK/preamble.h" -v w="$WORK" -v hf="$WORK/hasinc.txt" '
    BEGIN {
        while ((getline h < pre) > 0) pre_[++np] = h
        close(pre)
        while ((getline l < hf) > 0) { split(l, a, "\t"); has[a[1] + 0] = a[2] + 0 }
        close(hf)
        i = 0
    }
    /	签名	/ {
        i++
        for (k = 1; k <= np; k++) print pre_[k] > (w "/h_" i ".c")      # A 门：只用常用头
        if (!has[i]) for (k = 1; k <= np; k++) print pre_[k] > (w "/g_" i ".c")
        next
    }
    /	需要	/ { split($0, a, "\t"); print a[3] >> (w "/g_" i ".c"); next }
    /	来自	/ { split($0, a, "\t"); print a[3] > (w "/dir_" i ".txt"); next }
    {
        if ($0 ~ /^片段:/) {
            split($0, a, "\t")
            if (a[2] != "代码") next
            b = a[3]; sub(/\\$/, "", b)
        } else { b = $0; sub(/\\$/, "", b) }          # 裸续行 = 上一行代码的后续
        print b >> (w "/g_" i ".c")
        print b >> (w "/h_" i ".c")
    }' "$RAW"

    # 2b) 批量编译（150 个一批），整批失败再逐个定位
    gate_batch() {   # $1=文件前缀 g|h  $2=失败清单
        # ⚠ 先把参数抄成自己的变量：下面要循环取"这一批的文件名"，
        #   若用 `set --` 会**把 $1/$2 覆盖掉**（第一版就是这样把失败清单写丢的）
        _pre=$1
        _fail=$2
        rm -f "$_fail" "$_fail.tmp"
        split -l 150 "$WORK/idx_${_pre}.txt" "$WORK/chunk_"
        for c in "$WORK"/chunk_*; do
            [ -f "$c" ] || continue
            _files=$(sed "s|^|$WORK/${_pre}_|; s|\$|.c|" "$c" | tr '\n' ' ')
            _incs=""
            for k in $(cat "$c"); do
                [ -f "$WORK/dir_$k.txt" ] && _incs="$_incs -I$(dirname "$(cat "$WORK/dir_$k.txt")")"
            done
            if ! gcc -std=c11 -Wall -Werror=implicit-function-declaration -fsyntax-only $_incs $_files >/dev/null 2>&1; then
                for k in $(cat "$c"); do
                    _inc1=""
                    [ -f "$WORK/dir_$k.txt" ] && _inc1="-I$(dirname "$(cat "$WORK/dir_$k.txt")")"
                    gcc -std=c11 -Wall -Werror=implicit-function-declaration -fsyntax-only $_inc1 "$WORK/${_pre}_$k.c" >/dev/null 2>&1 || echo "$k" >> "$_fail.tmp"
                done
            fi
        done
        if [ -f "$_fail.tmp" ]; then mv "$_fail.tmp" "$_fail"; else : > "$_fail"; fi
        rm -f "$WORK"/chunk_*
    }
    seq 1 "$NF" > "$WORK/idx_g.txt"
    gate_batch g "$WORK/failB.txt"
    NF_BAD=$(grep -c . "$WORK/failB.txt" || true)
    NF_BAD=${NF_BAD:-0}
    # A 门：只查过了 B 门的那些（省一半编译）
    awk -v fb="$WORK/failB.txt" 'BEGIN { while ((getline l < fb) > 0) bad[l + 0] = 1 } { if (!($1 in bad)) print $1 }' "$WORK/idx_g.txt" > "$WORK/idx_h.txt"
    gate_batch h "$WORK/failA.txt"
    NF_STAND=$(grep -c . "$WORK/failA.txt" || true)
    NF_STAND=${NF_STAND:-0}

    # 2c) 组装：跳过 B 门没过的块；同名片段加 `_2/_3` 尾缀；补一条 `独立 是|否`
    awk -v failB="$WORK/failB.txt" -v failA="$WORK/failA.txt" '
    BEGIN {
        while ((getline l < failB) > 0) badB[l + 0] = 1
        while ((getline l < failA) > 0) badA[l + 0] = 1
        i = 0; keep = 1; cur = ""
    }
    # ⚠ 一个片段是**多行块**（签名/来自/行数/需要×k/代码…）：尾缀只在**签名那一行**决定，
    #   然后整块都用它。第一版对每一行都加尾缀 ⇒ 代码边挂到 `_4/_5/…` 这些不存在的片段上。
    /	签名	/ {
        i++
        keep = !(i in badB)
        if (!keep) next
        split($0, a, "\t"); id = a[1]
        if (id in seen) { seen[id]++; cur = id "_" seen[id] } else { seen[id] = 1; cur = id }
        sub(/^片段:[^\t]*/, cur, $0)
        print
        print cur "\t独立\t" ((i in badA) ? "否" : "是")
        next
    }
    keep { sub(/^片段:[^\t]*/, cur, $0); print }' "$RAW" > "$OUT"
    echo "  两道门：B（带源文件 include）编过 $((NF - NF_BAD)) / $NF；A（只用常用头）编过 $((NF - NF_BAD - NF_STAND))"
else
    cp "$RAW" "$OUT"
fi

printf 'C 片段知识：%s 行（片段 %s 个 / 来源 %s 个 .c / 独立 %s 个）\n' \
    "$(wc -l < "$OUT")" \
    "$(grep -c '	签名	' "$OUT" || true)" \
    "$(grep -a '	来自	' "$OUT" | cut -f3 | sort -u | wc -l)" \
    "$(grep -c '	独立	是' "$OUT" || true)"
