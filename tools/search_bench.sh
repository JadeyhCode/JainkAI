#!/bin/sh
# tools/search_bench.sh — **搜索能力**评测（§6.756）
#
# 五条搜索路径，分别测"找得到吗"和"多快"：
#   A 图搜索      ::rel <词>                     —— 已知事实的召回 + 延迟
#   B 片段搜索    ::code frag <关键词>            —— 代码片段召回
#   C 单片搜索    ::parts corpus rel <名字>       —— 含解压/挂载
#   D 跨片搜索    ::parts corpus find <名字> <目录> —— 分片数伸缩
#   E 本地论文    ::paper search <词>             —— papers/ 目录检索
#
# 用法：sh tools/search_bench.sh [分片目录]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SHARD_DIR=${1:-/mnt/models/debtrix/shards}
ms() { date +%s%N | cut -c1-13; }
ask() { printf '%s\n::quit\n' "$1" | "$ROOT/build_aip" --model "$2" 2>&1; }

echo "== A 图搜索（model.bin，41 198 边）=="
hit=0; tot=0
for q in 猫 动物 学习 朋友 太阳; do
    tot=$((tot + 1))
    n=$(printf '::rel %s\n::quit\n' "$q" | "$ROOT/build_aip" --model "$ROOT/model.bin" 2>/dev/null | grep -ac -- '--\[' || true)
    [ "${n:-0}" -gt 0 ] && hit=$((hit + 1))
done
t0=$(ms); for i in 1 2 3 4 5; do printf '::rel 猫\n::quit\n' | "$ROOT/build_aip" --model "$ROOT/model.bin" >/dev/null 2>&1; done; t1=$(ms)
echo "  召回 $hit/$tot 个词有边；含进程启动 $(awk -v d=$((t1-t0)) 'BEGIN{printf "%.0f", d/5}') ms/次"

echo "== B 片段搜索（code_model.bin，418 749 边）=="
hit=0; tot=0
for k in printf malloc strlen fopen; do
    tot=$((tot + 1))
    n=$(printf '::code frag %s\n::quit\n' "$k" | "$ROOT/build_aip" --model "$ROOT/code/code_model.bin" 2>/dev/null | grep -ac '片段:' || true)
    [ "${n:-0}" -gt 0 ] && hit=$((hit + 1))
    printf '  %-8s ⇒ %s 条\n' "$k" "${n:-0}"
done
echo "  召回 $hit/$tot"

echo "== C 单片搜索（含解压+挂载）=="
S=$(ls "$SHARD_DIR"/*.gz 2>/dev/null | head -1)
if [ -n "${S:-}" ]; then
    t0=$(ms); for i in 1 2 3; do printf '::parts corpus %s\n::parts corpus rel 函数:printf\n::parts corpus close\n::quit\n' "$S" | "$ROOT/build_aip" --model "$ROOT/model.bin" >/dev/null 2>&1; done; t1=$(ms)
    echo "  单片（$(du -h "$S" | cut -f1)）：$(awk -v d=$((t1-t0)) 'BEGIN{printf "%.0f", d/3}') ms/次（含 gzip 解压）"
else
    echo "  跳过（没有分片）"
fi

echo "== D 跨片搜索（伸缩）=="
for n in 8 32 128; do
    [ -d "$SHARD_DIR" ] || break
    d="$ROOT/_t/searchsub"; rm -rf "$d"; mkdir -p "$d"
    ls "$SHARD_DIR"/*.gz 2>/dev/null | head -n "$n" | while IFS= read -r f; do ln -sf "$f" "$d/" 2>/dev/null; done
    cnt=$(ls "$d" 2>/dev/null | wc -l)
    [ "$cnt" -gt 0 ] || continue
    t0=$(ms); out=$(printf '::parts corpus find 函数:printf %s\n::quit\n' "$d" | "$ROOT/build_aip" --model "$ROOT/model.bin" 2>&1); t1=$(ms)
    echo "  $cnt 片：$(awk -v d=$((t1-t0)) 'BEGIN{printf "%.1f", d/1000}') 秒 —— $(echo "$out" | grep -a '扫了' | tail -1 | sed 's/.*扫了 //')"
done

echo "== E 本地论文搜索 =="
if [ -d "$ROOT/papers" ] && [ -n "$(ls "$ROOT/papers" 2>/dev/null | head -1)" ]; then
    t0=$(ms); out=$(printf '::paper search 网络\n::quit\n' | "$ROOT/build_aip" --model "$ROOT/model.bin" 2>&1); t1=$(ms)
    echo "  $(echo "$out" | grep -ai '找到\|命中\|条' | head -1 | cut -c1-60)（$(awk -v d=$((t1-t0)) 'BEGIN{printf "%.0f", d}') ms）"
else
    echo "  跳过（papers/ 空）"
fi
