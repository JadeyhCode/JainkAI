#!/bin/sh
# tools/man2triples.sh — 从**本机 man 手册**抽 C 函数知识（阶段四扩图：第二个知识来源；§6.709）
#
# 为什么要有：`tools/headers2triples.sh` 只从头文件抽「有这个函数、原型、在哪个头」，
# 抽不到**用途**（一句话语义）。man 手册的 `NAME` 段天生就是一句话用途，
# 而且它是**独立来源** —— 同一个函数两个来源都出现，就可以做交叉核对（§6.709）。
#
# 输出三元组（TAB 分隔，`:ingest` 直接吃）：
#   函数:<名字>  用途   <NAME 段那一句话>        （英文，就是手册原话）
#   函数:<名字>  原型   <SYNOPSIS 里的原型>       （第二个来源，可与头文件交叉）
#   函数:<名字>  头     <SYNOPSIS 里的 #include>  （第二来源）
#   函数:<名字>  手册   man3/<页名>               （出处，回溯用）
#
# 用法：sh tools/man2triples.sh [输出文件] [手册目录…]
#   默认输出 <repo>/code/knowledge/man.triples，默认扫 /usr/share/man/man3 与 man2。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/code/knowledge/man.triples}
shift 2>/dev/null || true
DIRS=${*:-"/usr/share/man/man3 /usr/share/man/man2"}
TMP="$OUT.tmp"
mkdir -p "$(dirname "$OUT")"
: > "$TMP"

for d in $DIRS; do
    [ -d "$d" ] || { echo "  跳过（没有这个目录）：$d"; continue; }
    sec=${d##*/}
    for f in "$d"/*.gz; do
        [ -f "$f" ] || continue
        page=${f##*/}
        page=${page%.gz}
        zcat "$f" 2>/dev/null | awk -v page="$page" -v sec="$sec" '
        function striproff(s,   q) {
            # 去掉 roff 请求与引号/字体转义：`.BI "int printf(const char *" format ", ...);"`
            # ⚠ roff 里 `.BI a b c` 的参数是**直接相连**的（不插空格）：
            #   `.BI "int printf(const char *" format ", ...);"` 渲染成 `int printf(const char *format, ...);`
            #   所以先把"引号挨着空格"的那一对空格删掉，再去引号 —— 否则原型里会多出
            #   `const char * format , ...` 这种空格（不影响编译，但看起来像坏知识）。
            sub(/^\.[A-Za-z][A-Za-z]?[ \t]+/, "", s)
            gsub(/\\f[BIRP]/, "", s)
            gsub(/\\-/, "-", s)
            gsub(/"[ \t]+/, "", s)
            gsub(/[ \t]+"/, "", s)
            gsub(/"/, "", s)
            gsub(/[ \t]+$/, "", s)
            return s
        }
        function flushname(   i, n, a, nm, use2) {
            if (nameline == "") return
            i = index(nameline, " \\- ")
            if (i > 0) { names = substr(nameline, 1, i - 1); use = substr(nameline, i + 4) }
            else { names = nameline; use = "" }
            nameline = ""
            gsub(/[ \t]+$/, "", use)
            if (use == "") return
            if (length(use) > 80) use2 = substr(use, 1, 80); else use2 = use
            n = split(names, a, /,/)
            for (i = 1; i <= n; i++) {
                nm = a[i]
                gsub(/^[ \t]+|[ \t]+$/, "", nm)
                if (nm !~ /^[A-Za-z_][A-Za-z0-9_]*$/) continue
                print "函数:" nm "\t用途\t" use2
                print "函数:" nm "\t手册\t" sec "/" page
            }
        }
        function funname(p,   q, head, n, parts) {
            q = index(p, "(")
            if (q <= 1) return ""
            head = substr(p, 1, q - 1)
            sub(/[ \t]+$/, "", head)
            n = split(head, parts, /[ \t]/)
            if (parts[n] !~ /^[A-Za-z_][A-Za-z0-9_]*$/) return ""
            return parts[n]
        }
        BEGIN { st = 0; hdr = ""; nameline = "" }
        # NAME 段：`printf, fprintf, ... \- formatted output conversion`
        # ⚠ NAME **会折行**（printf.3 第 36/37 行：名字列一行、`\- 用途` 在下一行）
        #   ⇒ 先攒起来，见到 `\-` 再吐；不然一半的页抽不到"用途"（第一版就是这样丢了 printf）。
        /^\.SH[ \t]+NAME/ { st = 1; nameline = ""; next }
        st == 1 && /^\.SH/ { flushname(); st = 0 }
        st == 1 {
            line = $0
            if (line ~ /^\./) next
            if (nameline == "") nameline = line; else nameline = nameline " " line
            if (index(nameline, " \\- ") > 0) flushname()
            next
        }
        # SYNOPSIS：头文件 + 原型
        /^\.SH[ \t]+SYNOPSIS/ { st = 2; next }
        st == 2 && /^\.SH/ { st = 0 }
        st == 2 {
            line = $0
            if (line ~ /#include/) {
                if (match(line, /<[^>]+>/))
                    hdr = substr(line, RSTART + 1, RLENGTH - 2)
                next
            }
            if (line ~ /^\.nf/ || line ~ /^\.fi/ || line ~ /^\.PP/ || line ~ /^\.br/) next
            if (line ~ /^\.(IP|TP|RS|RE|sp|SH|SS|nf)/) next
            proto = line
            if (line ~ /^\.[A-Za-z]/) proto = striproff(line)
            gsub(/[ \t]+$/, "", proto)
            if (proto !~ /;$/) next
            if (proto !~ /\(/) next
            nm = funname(proto)
            if (nm == "") next
            if (length(proto) > 120) proto = substr(proto, 1, 120)
            print "函数:" nm "\t原型\t" proto
            if (hdr != "") print "函数:" nm "\t头\t" hdr
            print "函数:" nm "\t手册\t" sec "/" page
            np++
        }
        ' >> "$TMP"
    done
done

# 去重（同一函数的多个手册页/多个别名很常见）
LC_ALL=C sort -u "$TMP" -o "$TMP"
mv "$TMP" "$OUT"
printf 'man 知识：%s 行（函数 %s 个 / 有用途 %s / 有原型 %s / 有头 %s）\n' \
    "$(wc -l < "$OUT")" \
    "$(cut -f1 "$OUT" | sort -u | wc -l)" \
    "$(grep -c '	用途	' "$OUT" || true)" \
    "$(grep -c '	原型	' "$OUT" || true)" \
    "$(grep -c '	头	' "$OUT" || true)"
