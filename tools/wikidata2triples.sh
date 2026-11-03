#!/bin/sh
# tools/wikidata2triples.sh — 把 **Wikidata 的 NT dump** 转成 `::ingest` 吃的三元组（阶段四扩图：从公开语料薅真知识）
#
# 为什么要它：本机能挖的真知识只有 ~1.27×10⁶ 条边（§6.709/§6.710 把本机头文件/man/.c 都挖过了），
# 离"7B 真知识"差三个数量级 ⇒ 必须吃外部语料。Wikidata 的 `truthy` dump（实体-实体事实，
# 不含引用/限定符）是最便宜的一档：每行一条 `<Q..> <P..> <Q..> .`，转成三元组就能进图。
#
# 只收**实体对实体**（Q-P-Q）：字面量（数字/日期/字符串）会把节点数炸开，
# 而且本引擎的图是"词/概念"的图，不收值节点。丢掉了多少条，脚本会报出来（不静默）。
#
# 关系名：少数高频属性映射成中文关系（好查、好推理），其余保留成 `wd:Pxxx`（动态关系）。
# 想改映射就改下面这张表（**数据在脚本里，不在 C 里**）。
#
# 用法：sh tools/wikidata2triples.sh <dump.nt.bz2> <输出.triples> [最多行数]
#   例：sh tools/wikidata2triples.sh /mnt/models/wikidata/wd_truthy_head.nt.bz2 /tmp/wd.triples 200000
#   bz2 被截断也能用（`bzip2 -dc` 吐完能解的部分就停）—— 样本就是靠这个先跑通的。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IN=${1:?用法: sh tools/wikidata2triples.sh <dump.nt.bz2> <输出.triples> [最多行数]}
OUT=${2:?缺输出文件}
LIMIT=${3:-0}

case "$IN" in
*.bz2) CAT="bzip2 -dc" ;;
*.gz)  CAT="gzip -dc" ;;
*)     CAT="cat" ;;
esac

if [ "$LIMIT" -gt 0 ]; then
    $CAT "$IN" 2>/dev/null | head -n "$LIMIT" > "$OUT.nt"
else
    $CAT "$IN" 2>/dev/null > "$OUT.nt"
fi
NLINE=$(wc -l < "$OUT.nt")

awk -F'\t' '
BEGIN {
    # 高频属性 → 中文关系（其余保留 wd:Pxxx，动态关系照样能查）
    R["P31"]  = "属于";      # instance of
    R["P279"] = "属于";      # subclass of
    R["P17"]  = "在";        # country
    R["P361"] = "部分";      # part of
    R["P527"] = "包含";      # has part
    R["P1343"]= "记载于";    # described by source
    R["P106"] = "职业";      # occupation
    R["P39"]  = "职位";      # position held
    kept = 0; lit = 0; bad = 0
}
{
    line = $0
    # 期望形状：<subj> <pred> <obj> .
    n = split(line, parts, " ")
    if (n < 4) { bad++; next }
    subj = parts[1]; pred = parts[2]; obj = parts[3]
    if (substr(subj, 1, 1) != "<" || substr(obj, 1, 1) != "<") { lit++; next }
    s = substr(subj, 2, length(subj) - 2)
    p = substr(pred, 2, length(pred) - 2)
    o = substr(obj, 2, length(obj) - 2)
    # 只认 wikidata 实体 URI
    if (s !~ /\/entity\/Q[0-9]+$/) { bad++; next }
    if (p !~ /\/prop\/direct\/P[0-9]+$/) { bad++; next }
    if (o !~ /\/entity\/Q[0-9]+$/) { lit++; next }
    sub(/.*\/entity\//, "", s)
    sub(/.*\/prop\/direct\//, "", p)
    sub(/.*\/entity\//, "", o)
    rel = (p in R) ? R[p] : ("wd:" p)
    print "实体:" s "\t" rel "\t实体:" o
    kept++
}
END {
    printf "  NT 行 %d ⇒ 实体-实体事实 **%d**（字面量/值跳过 %d；形状不认识 %d）\n", NR, kept, lit, bad > "/dev/stderr"
}' "$OUT.nt" > "$OUT"
rm -f "$OUT.nt"
printf '  %s：%s 行三元组（%s）\n' "$OUT" "$(wc -l < "$OUT")" "$(grep -c '实体:' "$OUT" || true)"
[ "$NLINE" -gt 0 ] || { echo "FAIL: 一行 NT 都没读到（$IN 是空的/没解开？）"; exit 1; }
