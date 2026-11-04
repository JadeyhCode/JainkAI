#!/bin/sh
# tools/jsfacts.sh — 从 JS/TS 源码抽声明类事实（从 src_npm.sh 里抽出来的独立版本）§6.745
# 用法：sh tools/jsfacts.sh <源码根> <包名> <输出文件>
set -u
SRC=${1:?}; PKG=${2:?}; OUT=${3:?}
find "$SRC" -type f \( -name '*.js' -o -name '*.mjs' -o -name '*.cjs' -o -name '*.ts' -o -name '*.tsx' -o -name '*.jsx' \) \
    -size -2M -print 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
    rel=${f#"$SRC"/}
    awk -v pkg="$PKG" -v file="$PKG/$rel" '
    {
        line = $0
        if (match(line, /(^|[^A-Za-z0-9_$.])function[ \t]+[A-Za-z_$][A-Za-z0-9_$]*/)) {
            s = substr(line, RSTART, RLENGTH); sub(/.*function[ \t]+/, "", s)
            print "函数:" s "\t定义于\t文件:" file
        }
        if (match(line, /(^|[^A-Za-z0-9_$.])class[ \t]+[A-Za-z_$][A-Za-z0-9_$]*/)) {
            s = substr(line, RSTART, RLENGTH); sub(/.*class[ \t]+/, "", s)
            print "类:" s "\t定义于\t文件:" file
        }
        if (match(line, /(^|[^A-Za-z0-9_$.])interface[ \t]+[A-Za-z_$][A-Za-z0-9_$]*/)) {
            s = substr(line, RSTART, RLENGTH); sub(/.*interface[ \t]+/, "", s)
            print "接口:" s "\t定义于\t文件:" file
        }
        if (match(line, /(const|let|var)[ \t]+[A-Za-z_$][A-Za-z0-9_$]*[ \t]*=[ \t]*(async[ \t]+)?(function|\()/)) {
            s = substr(line, RSTART, RLENGTH); sub(/^(const|let|var)[ \t]+/, "", s); sub(/[ \t]*=.*$/, "", s)
            print "函数:" s "\t定义于\t文件:" file
        }
        if (match(line, /(from[ \t]*|require[ \t]*\()[ \t]*[\047"][^\047"]+[\047"]/)) {
            s = substr(line, RSTART, RLENGTH); sub(/^[^ \t]*[ \t]*\(?[ \t]*/, "", s); gsub(/[\047"]/, "", s)
            if (s != "") print "文件:" file "\t导入\t模块:" s
        }
    }' "$f" >> "$OUT"
done
