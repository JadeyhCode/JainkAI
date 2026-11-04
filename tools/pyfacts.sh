#!/bin/sh
# tools/pyfacts.sh — 从 Python 源码抽声明类事实（从 src_pypi.sh 里抽出来的独立版本）§6.745
# 用法：sh tools/pyfacts.sh <源码根> <包名> <输出文件>
set -u
SRC=${1:?}; PKG=${2:?}; OUT=${3:?}
find "$SRC" -name '*.py' -size -2M -print 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
    rel=${f#"$SRC"/}
    awk -v file="$PKG/$rel" '
    BEGIN { cls = "" }
    {
        line = $0
        if (line ~ /^[ \t]*class[ \t]+[A-Za-z_][A-Za-z0-9_]*/) {
            sub(/^[ \t]*class[ \t]+/, "", line); sub(/[^A-Za-z0-9_].*$/, "", line)
            if (line != "") { print "类:" line "\t定义于\t文件:" file; cls = line }
            next
        }
        if (line ~ /^[ \t]*def[ \t]+[A-Za-z_][A-Za-z0-9_]*/) {
            indent = line; sub(/[^ \t].*$/, "", indent)
            sub(/^[ \t]*def[ \t]+/, "", line); sub(/[^A-Za-z0-9_].*$/, "", line)
            if (line == "") next
            if (cls != "" && length(indent) > 0) print "方法:" cls "." line "\t定义于\t文件:" file
            else { print "函数:" line "\t定义于\t文件:" file; if (length(indent) == 0) cls = "" }
            next
        }
        if (line ~ /^[ \t]*from[ \t]+[A-Za-z0-9_.]+[ \t]+import/) {
            sub(/^[ \t]*from[ \t]+/, "", line); sub(/[ \t].*$/, "", line)
            print "文件:" file "\t导入\t模块:" line; next
        }
        if (line ~ /^[ \t]*import[ \t]+[A-Za-z0-9_.]+/) {
            sub(/^[ \t]*import[ \t]+/, "", line); sub(/[,;# \t].*$/, "", line)
            print "文件:" file "\t导入\t模块:" line
        }
    }' "$f" >> "$OUT"
done
