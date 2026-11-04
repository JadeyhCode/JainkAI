#!/bin/sh
# tools/rebuild_index.sh — 预生成"已剔除做过包"的分片索引（§6.744）
#
# 为什么不用脚本内过滤：实测脚本内那条 awk 会把 676 行的片压成 0 行（原因未明），
# 而这里的手工流水线验证可用（ubucorpus 10 473 行 / debtrix 13 964 行）。
#
# 用法：sh tools/rebuild_index.sh <语料目录> <套件> <分片总数>
set -u
D=${1:?}; SEC=${2:?}; SN=${3:-33}
[ -s "$D/Sources.gz" ] || { echo "FAIL: 没有 $D/Sources.gz"; exit 1; }
gzip -t "$D/Sources.gz" 2>/dev/null || { echo "FAIL: $D/Sources.gz 损坏"; exit 1; }
rm -f "$D"/index.[0-9]*.txt
k=1
while [ "$k" -le "$SN" ]; do
    zcat "$D/Sources.gz" | awk -v sec="$SEC" -v sk="$k" -v sn="$SN" '
    /^Package:/ { pkg = $2 }
    /^Files:/ { inf = 1; next }
    inf && /\.orig\.tar\./ { n = split($3, a, "/"); f = a[n]; inf = 0
        c = substr(pkg,1,1); if (substr(pkg,1,3)=="lib") c="lib" substr(pkg,4,1)
        prio = 1; if (pkg ~ /^lib/ || pkg ~ /-dev$/) prio = 0
        if (NR % sn != sk) next
        print prio "\t" pkg "\tpool/" sec "/" c "/" pkg "/" f; next }
    inf && /^[^ ]/ { inf = 0 }' \
      | LC_ALL=C sort -t"	" -k1,1n -s | cut -f2- \
      | awk -F'\t' 'NR==FNR { d[$1]=1; next } !($1 in d)' "$D/done.all.txt" - > "$D/index.$k.txt.new" \
      && mv "$D/index.$k.txt.new" "$D/index.$k.txt"   # ⚠ 原子替换：否则 worker 会看到"正在写的空文件"
    k=$((k + 1))
done
echo "== $D：索引 $(( SN )) 片 / 合计 $(cat "$D"/index.*.txt 2>/dev/null | wc -l) 行 =="
