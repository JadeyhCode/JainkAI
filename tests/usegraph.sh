#!/bin/sh
# tests/usegraph.sh — **扩完的图到底有没有用**（阶段四：把"有用"变成能量的事；§6.713）
#
# 判据不是"边多不多"，而是**问它、它答得对不对**。三类图各出一组题：
#   ① 真知识（Wikidata v2 分片）：Q31 是不是主权国家、Q31 在哪个国家、瞎编的实体必须说"没有"；
#   ② 代码知识图（`code/code_model.bin`）：printf 的原型/头、任务→骨架、片段能查到；
#   ③ 7B 合成语料分片（`.c7b`）：挂上能查邻居、读回字节远小于文件。
# 最后打一张表：**答对 / 答错 / 没答**（"没答"单独算，不许拿"没答"充"答对"）。
#
# 用法：sh tests/usegraph.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/usegraph"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }

ok_n=0; bad_n=0; miss_n=0; total=0
ask() { printf '  %-58s ' "$1"; total=$((total + 1)); }
hit()   { ok_n=$((ok_n + 1));   printf '[答对] %s\n' "${1:-}"; }
wrong() { bad_n=$((bad_n + 1)); printf '[答错] %s\n' "${1:-}"; }
miss()  { miss_n=$((miss_n + 1)); printf '[没答] %s\n' "${1:-}"; }

echo "== ① 真知识（Wikidata v2 分片，片内属性表）=="
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o wd2c7b "$ROOT/tools/wd2c7b.c" -lm 2> wd2c7b.err \
    || bad_n=$((bad_n + 1))
./wd2c7b wd.c7b2 < "$ROOT/tests/fixtures/wd_sample.nt" > wd2.log 2>&1
q_wd() {   # $1=问题 $2=实体 $3=期望片段
    ask "$1"
    printf '::parts corpus wd.c7b2\n::parts corpus rel %s\n::quit\n' "$2" | ./build_aip --model m.bin > wd_q.txt 2>&1
    if grep -qa -- "$3" wd_q.txt; then hit "$3"
    elif grep -qa '没有它\|没有事实' wd_q.txt; then miss "图里没有"
    else wrong "$(grep -a '\[语料\]' wd_q.txt | tail -1 | cut -c1-60)"
    fi
}
q_wd "Q31 是不是主权国家（P31=Q3624078）？" Q31 'Q31 --\[P31\]--> Q3624078'
q_wd "Q31 在哪个国家（P17=Q55）？" Q31 'Q31 --\[P17\]--> Q55'
q_wd "Q31 的职业（P106=Q1069370）？" Q31 'Q31 --\[P106\]--> Q1069370'
q_wd "瞎编的实体 Q999999999 必须说没有" Q999999999 'Q999999999：这个分片里没有它'

echo "== ② 代码知识图（406 125 边的默认模型）=="
MODEL="$ROOT/code/code_model.bin"
if [ ! -s "$MODEL" ]; then
    [ -s "$ROOT/code/knowledge/capi.triples" ] || sh "$ROOT/tools/headers2triples.sh" "$ROOT/code/knowledge/capi.triples" 100000 > /dev/null 2>&1
    sh "$ROOT/code/build_code_model.sh" > build.log 2>&1
fi
if [ -s "$MODEL" ]; then
    ask "printf 的原型和头（C API 知识）"
    printf '::rel 函数:printf\n::quit\n' | ./build_aip --model "$MODEL" > c1.txt 2>&1
    if grep -qa '函数:printf --\[头\]--> stdio.h' c1.txt && grep -qa '函数:printf --\[原型\]-->' c1.txt; then hit "stdio.h + 原型"
    else wrong "$(grep -a '函数:printf' c1.txt | head -1 | cut -c1-60)"; fi

    ask "任务:素数 → 骨架（合成器检索源）"
    printf '::rel 任务:素数\n::quit\n' | ./build_aip --model "$MODEL" > c2.txt 2>&1
    if grep -qa '任务:素数 --\[骨架\]--> 骨架:素数' c2.txt; then hit "骨架:素数"
    else wrong "$(grep -a '任务:素数' c2.txt | head -1 | cut -c1-60)"; fi

    ask "片段库非空（::code frag）"
    printf '::code frag\n::quit\n' | ./build_aip --model "$MODEL" > c3.txt 2>&1
    if grep -qa '图里共 [1-9][0-9]* 个片段' c3.txt; then hit "$(grep -a '图里共' c3.txt | tail -1 | sed 's/.*图里共 //' | cut -c1-40)"
    else miss "$(grep -a '\[片段\]' c3.txt | tail -1 | cut -c1-60)"; fi

    # ⚠ 延迟必须用**差分法**，而且查询次数要够多：5 次时载入抖动（±50 ms）会盖住信号，
    #   实测 100 次查询和 0 次的总时间差在噪声里（926 vs 1006 ms）⇒ 用 200 次、门限 2 ms/条。
    ask "单条查询延迟（差分法：200 次 − 0 次，要求 ≤ 2 ms/条）"
    printf '::quit\n' > q0.in
    : > q200.in
    i=0
    while [ "$i" -lt 200 ]; do printf '::rel 函数:printf\n' >> q200.in; i=$((i + 1)); done
    printf '::quit\n' >> q200.in
    a0=$(date +%s%N); ./build_aip --model "$MODEL" < q0.in > /dev/null 2>&1; a1=$(date +%s%N)
    b0=$(date +%s%N); ./build_aip --model "$MODEL" < q200.in > /dev/null 2>&1; b1=$(date +%s%N)
    base=$(( (a1 - a0) / 1000000 ))
    many=$(( (b1 - b0) / 1000000 ))
    per=$(( (many - base) * 1000 / 200 ))
    [ "$per" -lt 0 ] && per=0
    if [ "$per" -le 2000 ]; then hit "${per} µs/条（200 次 − 载入 ${base} ms）"
    else wrong "${per} µs/条（载入 ${base} ms / 200 次 ${many} ms）"; fi
    rm -f q0.in q200.in
else
    ask "代码知识图（缺 $MODEL）"; miss "先跑 code/build_code_model.sh"
fi

echo "== ③ 7B 合成语料分片（.c7b v1）=="
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o mkcorpus7b "$ROOT/tools/mkcorpus7b.c" -lm 2> mk.err \
    || bad_n=$((bad_n + 1))
./mkcorpus7b big.c7b 20000 400000 1 7 2000 > /dev/null 2>&1
ask "挂一片 400 000 边的分片并查一个词"
printf '::parts corpus big.c7b\n::parts corpus rel w000042\n::quit\n' | ./build_aip --model m.bin > b1.txt 2>&1
if grep -qa -- 'w000042 --\[' b1.txt; then hit "$(grep -ac -- 'w000042 --\[' b1.txt) 条邻居"
else wrong "$(grep -a '\[语料\]' b1.txt | tail -1 | cut -c1-60)"; fi

ask "读回字节数远小于文件（局部加载）"
readb=$(printf '::parts corpus big.c7b\n::parts corpus rel w000042\n::parts corpus stats\n::quit\n' | ./build_aip --model m.bin 2>&1 | grep -a '共 [0-9]* 字节' | tail -1 | sed 's/.*共 \([0-9]*\) 字节.*/\1/')
fsz=$(wc -c < big.c7b)
if [ -n "${readb:-}" ] && [ "$readb" -lt $((fsz / 20)) ]; then hit "读 ${readb} / 文件 ${fsz} 字节"
elif [ -n "${readb:-}" ]; then wrong "读了 ${readb} / ${fsz}（太多）"
else miss ""; fi

echo "== ④ 真实代码语料（Debian 分片，28 M 量级；有就测，没有就 SKIP）=="
REAL=${REAL_SHARD:-$(ls /mnt/models/codecorpus/shards/*.gz 2>/dev/null | head -1)}
if [ -n "${REAL:-}" ] && [ -s "$REAL" ]; then
    ask "函数:printf 的原型/头（真实 Debian 源码）"
    printf '::parts corpus %s\n::parts corpus rel 函数:printf\n::parts corpus stats\n::quit\n' "$REAL" | ./build_aip --model m.bin > r1.txt 2>&1
    if grep -qa -- '函数:printf --\[头\]-->' r1.txt && grep -qa -- '函数:printf --\[原型\]-->' r1.txt; then
        hit "头 + 原型（$(grep -ac -- '函数:printf --\[' r1.txt) 条）"
    elif grep -qa '挂上\|v2 真 KG' r1.txt; then miss "分片里没有 函数:printf"
    else wrong "$(grep -a '\[语料\]' r1.txt | tail -1 | cut -c1-60)"; fi

    ask "宏:HAVE_CONFIG_H 的声明出处"
    printf '::parts corpus %s\n::parts corpus rel 宏:HAVE_CONFIG_H\n::quit\n' "$REAL" | ./build_aip --model m.bin > r2.txt 2>&1
    if grep -qa -- '宏:HAVE_CONFIG_H --\[声明\]-->' r2.txt; then hit "$(grep -a -- '宏:HAVE_CONFIG_H --' r2.txt | head -1 | sed 's/.*--> //' | cut -c1-40)"
    else miss "这条分片里没有"; fi

    ask "局部加载：一次查询只读少量字节"
    readb=$(printf '::parts corpus %s\n::parts corpus rel 函数:printf\n::parts corpus stats\n::quit\n' "$REAL" | ./build_aip --model m.bin 2>&1 | grep -a '共 [0-9]* 字节' | tail -1 | sed 's/.*共 \([0-9]*\) 字节.*/\1/')
    if [ -n "${readb:-}" ]; then hit "读 ${readb} 字节"; else miss ""; fi
else
    ask "真实代码语料（缺分片）"; miss "SKIP（/mnt/models/codecorpus/shards 为空）"
fi
echo "== ⑤ 第二套真语料（Ubuntu universe 分片）=="
# ⚠ 分片是**按块滚动生成**的，最新那一片不一定含 printf（实测：`tail -1` 拿到一片没有的 ⇒ 假红）
# ⇒ 最多试 6 片，任一片里有就算对；全都没有才记"没答"。
# 分片按块滚动生成 ⇒ 最新几片不一定含某个函数。这里**多个候选名字 × 多片**，
# 任一组查到就算过；全都没有就 SKIP（不假红——抽样不中不等于引擎坏了）。
hit2=0; tried2=0
for cand in ${REAL_SHARD2:-$(ls /mnt/models/ubucorpus/shards/*.gz 2>/dev/null | tail -8)}; do
    [ -s "$cand" ] || continue
    tried2=$((tried2 + 1))
    for nm in 函数:printf 函数:malloc 函数:main 宏:HAVE_CONFIG_H; do
        printf '::parts corpus %s\n::parts corpus rel %s\n::parts corpus close\n::quit\n' "$cand" "$nm" | ./build_aip --model m.bin > u1.txt 2>&1
        if grep -qa -- "$nm --\\[" u1.txt; then
            ask "Ubuntu 分片：查得到一个真实函数/宏（$nm）"
            hit "第 $tried2 片，$(grep -ac -- "$nm --\\[" u1.txt) 条"
            hit2=1; break
        fi
    done
    [ "$hit2" -eq 1 ] && break
done
if [ "$hit2" -eq 0 ]; then
    ask "Ubuntu 分片：查得到一个真实函数/宏"
    if [ "$tried2" -eq 0 ]; then miss "SKIP（没有分片）"; else miss "SKIP（抽样的 $tried2 片里没有候选名字）"; fi
fi

echo "== ⑥ 跨分片查名字（::parts corpus find）：7B 语料可用的关键能力 =="
FIND_DIR=/mnt/models/localcorpus/shards
if [ -d "$FIND_DIR" ] && [ -n "$(ls "$FIND_DIR"/*.gz 2>/dev/null)" ]; then
    ask "在 2 片里找 函数:printf / 类:*"
    out=$(printf '::parts corpus find 函数:printf %s\n::parts corpus find 类:App %s\n::quit\n' "$FIND_DIR" "$FIND_DIR" | ./build_aip --model m.bin 2>&1)
    n_scan=$(echo "$out" | grep -ac '扫了 [0-9]* 片' || true)
    if [ "${n_scan:-0}" -ge 2 ]; then hit "$n_scan 次扫描都有结果行"
    else wrong "没有扫描结果行"; fi
else
    ask "跨分片查（缺分片目录）"; miss "SKIP"
fi

echo
printf '有用性：**答对 %d / 共 %d**（答错 %d / 没答 %d）\n' "$ok_n" "$total" "$bad_n" "$miss_n"
[ "$bad_n" -eq 0 ] && [ "$miss_n" -eq 0 ] && printf 'PASS usegraph（三类图都答对了：真知识 / 代码知识 / 7B 语料）\n' || printf 'FAIL usegraph（答错 %d / 没答 %d）\n' "$bad_n" "$miss_n"
[ "$bad_n" -eq 0 ] && [ "$miss_n" -eq 0 ]
