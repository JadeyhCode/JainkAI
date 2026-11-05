#!/bin/sh
# tools/coverage_sample.sh — 抽样评估"这个语料能回答多少真实问题"§6.748
#
# 做法：拿一批**真实 C 符号**（libc 常见函数 + 宏 + 类型），在若干**随机分片**里查，
# 统计命中率（分片里是否有该符号的事实）。这不是精确的召回率，而是"随手一问答不答得上"的体感指标。
#
# 用法：sh tools/coverage_sample.sh [每语料取几片，默认 4]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
N=${1:-4}
SYMS="函数:printf 函数:malloc 函数:free 函数:memcpy 函数:strlen 函数:fopen 函数:fclose \
函数:pthread_create 函数:socket 函数:bind 函数:listen 函数:accept 函数:open 函数:read 函数:write \
宏:HAVE_CONFIG_H 宏:NULL 类型:size_t"
total=0; hit=0
for corpus in codecorpus ubucorpus ubumain debtrix bigsrc localcorpus; do
    d=/mnt/models/$corpus/shards
    [ -d "$d" ] || continue
    list=$(ls "$d"/*.gz 2>/dev/null | awk -v n="$N" 'BEGIN{srand(42)} {a[NR]=$0} END{for(i=1;i<=n && i<=NR;i++){k=int(rand()*NR)+1; print a[k]}}')
    [ -n "$list" ] || continue
    c_hit=0; c_tot=0; c_shards=0
    for s in $list; do
        c_shards=$((c_shards + 1))
        for sym in $SYMS; do
            c_tot=$((c_tot + 1))
            if printf '::parts corpus %s\n::parts corpus rel %s\n::parts corpus close\n::quit\n' "$s" "$sym" \
                | "$ROOT/build_aip" --model "$ROOT/model.bin" 2>/dev/null | grep -qa -- "$sym --\\["; then
                c_hit=$((c_hit + 1))
            fi
        done
    done
    total=$((total + c_tot)); hit=$((hit + c_hit))
    printf '  %-12s 抽 %d 片 / %d 问 ⇒ 命中 %d（%.0f%%）\n' "$corpus" "$c_shards" "$c_tot" "$c_hit" \
        "$(awk -v a=$c_hit -v b=$c_tot 'BEGIN{printf "%.0f", (b>0? 100*a/b:0)}')"
done
printf '合计：%d 问 / 命中 %d（%.0f%%）\n' "$total" "$hit" "$(awk -v a=$hit -v b=$total 'BEGIN{printf "%.0f", (b>0? 100*a/b:0)}')"
