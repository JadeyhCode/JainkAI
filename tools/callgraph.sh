#!/bin/sh
# tools/callgraph.sh — 从 C 源码抽**调用关系**事实（§6.739）
#
# 为什么加：现在管道卡在带宽（3.77 MB/s），要更快只能"同样一 MB 出更多事实"。
# `调用` 是真实断言（谁调用了谁），每个 .c 文件能出几十到几百条。
#
# 事实格式（和 `::ingest` 一致）：
#   文件:<pkg/相对路径>   调用   函数:<被调函数名>
#
# 过滤：C 关键字、`sizeof`、`defined`、纯大写宏也保留（宏调用是真实的），但长度 <3 的丢掉。
#
# 用法：sh tools/callgraph.sh <源码根> <包名> <输出文件>
set -u
ROOT=${1:?}; PKG=${2:?}; OUT=${3:?}
find "$ROOT" -type f -name '*.c' -size -4M -print 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
    rel=${f#"$ROOT"/}
    awk -v file="$PKG/$rel" '
    BEGIN {
        split("if for while switch return sizeof defined case goto else do break continue " \
              "static const unsigned signed long short int char float double void struct union enum " \
              "typedef extern register volatile inline __attribute__ __inline__ __asm__ asm", kw, " ")
        for (i in kw) K[kw[i]] = 1
    }
    {
        line = $0
        # 去注释行与字符串里的内容（粗但够用：行首 // 或 * 直接跳过）
        if (line ~ /^[ \t]*(\/\/|\*|\/\*)/) next
        while (match(line, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
            s = substr(line, RSTART, RLENGTH)
            line = substr(line, RSTART + RLENGTH)
            sub(/[ \t]*\($/, "", s)
            if (s in K) continue
            if (length(s) < 3) continue
            if (s ~ /^[A-Z_][A-Z0-9_]+$/) { }        # 宏调用也留
            if (!(s in seen)) { seen[s] = 1; print "文件:" file "\t调用\t函数:" s }
        }
    }' "$f" >> "$OUT"
done
