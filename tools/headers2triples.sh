#!/bin/sh
# tools/headers2triples.sh — 从系统头文件抽 **C API 知识**（阶段四 M1；§6.699）
#
# 为什么抽头文件：这是本机上**真实存在**的 C 代码知识（函数名 / 原型 / 属于哪个头），
# 不是造出来的噪声。合成器要"写代码"，第一件事就是知道有哪些函数、怎么声明、要 include 什么。
#
# 输出**三元组**（TAB 分隔，和 `::ingest` 的格式一致）：
#   函数:<名字>  头   <头文件>
#   函数:<名字>  原型 <一行原型>
#   头:<头文件>  声明 <名字>
#
# 用法：sh tools/headers2triples.sh [输出文件] [原型上限]
#   默认输出 <repo>/code/knowledge/capi.triples，上限 60000 条原型。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/code/knowledge/capi.triples}
MAX=${2:-60000}
INC=${INC:-/usr/include}

[ -d "$INC" ] || { echo "FAIL: 没有 $INC"; exit 1; }
mkdir -p "$(dirname "$OUT")"
TMP="$OUT.tmp"
: > "$TMP"

# 只收"像函数声明"的行：以标识符或类型开头、含 '(' 和 ')'、以 ';' 结尾；
# 去掉宏、类型定义、结构体成员（缩进过的）、K&R 老写法（没括号里的类型）。
find "$INC" -name '*.h' -print 2>/dev/null | LC_ALL=C sort | while IFS= read -r h; do
    rel=${h#"$INC"/}
    awk -v hdr="$rel" '
        /^[A-Za-z_][A-Za-z0-9_ \t\*]*\(/ && /\)/ && /;[ \t]*$/ {
            line = $0
            # 函数名：第一个左括号前面最后一个标识符
            p = index(line, "(")
            if (p <= 1) next
            head = substr(line, 1, p - 1)
            # ⚠ 必须先去尾空格再 split：`extern int printf (` 的 head 以空格结尾，
            # split 会多出一个**空字段**，name 变成空串 ⇒ 整个函数被丢掉
            # （实测 printf/strlen 这些全丢了，只收到 8 816 个偏门函数）
            sub(/[ \t]+$/, "", head)
            n = split(head, parts, /[ \t]+/)
            name = parts[n]
            if (name !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            gsub(/[ \t]+/, " ", line)
            sub(/^ /, "", line)
            print "函数:" name "\t头\t" hdr
            print "函数:" name "\t原型\t" line
            print "头:" hdr "\t声明\t" name
            # 返回类型（`extern int printf (...)` 里 `int`）与参数个数 —— 写代码时要用
            rt = parts[1]
            if (rt != "extern" && rt != "static" && rt != "void" && rt != "inline")
                print "函数:" name "\t返回\t" rt
            else if (parts[2] != "" && parts[2] != "int" && parts[2] != "char")
                print "函数:" name "\t返回\t" parts[2]
            np = 0
            body = substr(line, p)
            if (body !~ /\([ \t]*void[ \t]*\)/) {
                np = 1
                for (q = 2; q <= length(body); q++) if (substr(body, q, 1) == ",") np++
            } else np = 0
            print "函数:" name "\t参数\t" np
        }
        # 枚举成员（缩进 + 全大写/驼峰常量 + `=` 或 `,`）—— 头文件里大量真实常量
        /^[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*(=[ \t]*[^,]*)?[,]?[ \t]*$/ && !/;/ && !/^[ \t]*#/ {
            line = $0
            sub(/^[ \t]+/, "", line)
            sub(/[ \t]*,?[ \t]*$/, "", line)
            eq = index(line, "=")
            if (eq > 0) { name = substr(line, 1, eq - 1); val = substr(line, eq + 1) }
            else { name = line; val = "" }
            gsub(/[ \t]+$/, "", name)
            if (name !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            if (toupper(name) != name && name !~ /^[a-z]/) next
            if (length(val) > 40) val = substr(val, 1, 40)
            print "枚举值:" name "\t定义\t" val
            print "枚举值:" name "\t声明\t" hdr
        }
        # 结构体字段（缩进 + 类型... + 名字 + 可选数组 + ;）
        /^[ \t][ \t]+[A-Za-z_][A-Za-z0-9_ \t\*]*[ \t\*]+[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?[ \t]*;[ \t]*$/ {
            line = $0
            sub(/^[ \t]+/, "", line)
            sub(/[ \t]*;[ \t]*$/, "", line)
            if (line ~ /\(/) next
            n = split(line, parts, /[ \t]+/)
            fname = parts[n]
            sub(/\[.*\]/, "", fname)
            if (fname !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            ftype = parts[1]
            if (length(ftype) > 24) ftype = substr(ftype, 1, 24)
            print "字段:" fname "\t类型\t" ftype
            print "字段:" fname "\t声明\t" hdr
        }
        # 宏：`#define NAME 体...`（体太长就截断 —— 知识要的是"有这个宏、大致干什么"）
        /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*/ {
            line = $0
            sub(/^[ \t]*#[ \t]*define[ \t]+/, "", line)
            p = index(line, " ")
            if (p > 0) { name = substr(line, 1, p - 1); body = substr(line, p + 1) }
            else { name = line; body = "" }
            if (name !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            if (length(body) > 60) body = substr(body, 1, 60)
            print "宏:" name "\t定义\t" body
            print "宏:" name "\t声明\t" hdr
        }
        # 类型：`typedef ... NAME;`
        /^[ \t]*typedef[ \t]/ && /;[ \t]*$/ {
            line = $0
            p = index(line, ";")
            if (p > 0) line = substr(line, 1, p - 1)
            n = split(line, parts, /[ \t]+/)
            name = parts[n]
            if (name !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            if (length(line) > 60) line = substr(line, 1, 60)
            print "类型:" name "\t定义\t" line
            print "类型:" name "\t声明\t" hdr
        }
        # 结构体/联合/枚举的**名字**（定义体不管，只记"有这么个类型、在哪个头"）
        /^[ \t]*struct[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\{/ || /^[ \t]*union[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\{/ || /^[ \t]*enum[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\{/ {
            line = $0
            sub(/^[ \t]*/, "", line)
            split(line, parts, /[ \t]+/)
            kind = parts[1]; name = parts[2]
            if (name !~ /^[A-Za-z_][A-Za-z0-9_]*$/) next
            print kind ":" name "\t声明\t" hdr
        }' "$h" >> "$TMP"
done

# 去重（同一函数在多个头里声明很常见）
LC_ALL=C sort -u "$TMP" -o "$TMP"
N=$(wc -l < "$TMP")
if [ "$N" -gt "$((MAX * 3))" ]; then
    # 截断要**成套**截（否则只有"头"没有"原型"，知识是残的）
    head -n "$((MAX * 3))" "$TMP" > "$TMP.cut" && mv "$TMP.cut" "$TMP"
    echo "  （截断到 $((MAX * 3)) 行 = 约 $MAX 个函数）"
fi
mv "$TMP" "$OUT"
printf 'C API 知识：%s 行（函数 %s 个 / 头 %s 个）\n' \
    "$(wc -l < "$OUT")" \
    "$(grep -c '	原型	' "$OUT" || true)" \
    "$(grep -c '^头:' "$OUT" | awk '{print int($0/1)}' || true)"
