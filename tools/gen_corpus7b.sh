#!/bin/sh
# tools/gen_corpus7b.sh — **7B 边合成语料**的分片生成 + 校验（阶段四扩图；§6.711）
#
# 为什么要新格式：0.5B 那批每片是**完整模型**（59.2 B/边）⇒ 7B 要 414 GB，本机 `/mnt/models`
# 只剩 327 GB。`.c7b` 只存边（**8 B/边**）⇒ 7B ≈ **58 GB**，装得下。
#
# 生成什么：`<输出目录>/shard_NNN.c7b`，每片 2000 万边、同一套 40 万词表（`w000001`…）、
# 不同种子（片号进种子）⇒ 350 片 = 7 000 000 000 边。
# **照实说**：这不是"一张 7B 边的单图"（那需要 392 GB 内存），而是 350 片可独立 attach 的语料
# （与 0.5B 同口径：按片查询；跨片单图 = 路线 B，未做）。
#
# 断点续跑：已存在且 `--probe` 校验通过的分片**跳过**；清单每次重写（写全量、不是追加）。
# 校验：每片内部"索引合计 == 头里的边数"且"文件长度 == 头 + 索引 + 边×8"；
#       最后汇总"总边数 == 目标"，并抽查若干分片的查询。
#
# 用法：sh tools/gen_corpus7b.sh [总边数] [每片边数] [词数] [输出目录] [种子]
#       sh tools/gen_corpus7b.sh --verify-only [输出目录]
#   默认：7 000 000 000 / 20 000 000 / 400 000 / /mnt/models/aip-corpus-7B / 7
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ "${1:-}" = "--verify-only" ]; then
    OUT=${2:-/mnt/models/aip-corpus-7B}
    VERIFY_ONLY=1
    TOTAL=0; PER=0; NW=0; SEED=0
    # 目标从清单里读回来（否则汇总里会打"目标 0"，像没设目标 —— 实测踩过）
    [ -f "$OUT/manifest.txt" ] && TOTAL=$(sed -n 's/^total_target //p' "$OUT/manifest.txt" | head -1)
else
    VERIFY_ONLY=0
    TOTAL=${1:-7000000000}
    PER=${2:-20000000}
    NW=${3:-400000}
    OUT=${4:-/mnt/models/aip-corpus-7B}
    SEED=${5:-7}
fi
CAP=${C7B_CAP:-2000}

mkdir -p "$OUT" || { echo "FAIL: 建不了输出目录 $OUT"; exit 1; }
BIN="$ROOT/_t/c7b/mkcorpus7b"
mkdir -p "$ROOT/_t/c7b"
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o "$BIN" "$ROOT/tools/mkcorpus7b.c" -lm \
    || { echo "FAIL: mkcorpus7b 编不过"; exit 1; }

MAN="$OUT/manifest.txt"

# 计划里第 i 片（1 起）应有的边数（最后一片可能不满）
plan_edges() {
    _i=$1
    _left=$((TOTAL - (_i - 1) * PER))
    if [ "$_left" -ge "$PER" ]; then echo "$PER"; else echo "$_left"; fi
}

if [ "$VERIFY_ONLY" = 0 ]; then
    NSHARD=$(( (TOTAL + PER - 1) / PER ))
    echo "== 7B 语料：$TOTAL 边 / 每片 $PER / $NW 词 / $NSHARD 片 / 输出 $OUT =="
    echo "   （每边 8 字节 ⇒ 预计 $((TOTAL * 8 / 1000000000)) GB + 索引；出度上限 $CAP）"
    i=1
    t_start=$(date +%s)
    while [ "$i" -le "$NSHARD" ]; do
        f=$(printf "%s/shard_%03d.c7b" "$OUT" "$i")
        want=$(plan_edges "$i")
        if [ -s "$f" ] && "$BIN" --probe "$f" > /dev/null 2>&1; then
            :   # 已完成且校验过 ⇒ 跳过（断点续跑）
        else
            rm -f "$f"
            if ! "$BIN" "$f" "$NW" "$want" "$i" "$SEED" "$CAP" 2>>"$OUT/build.log"; then
                echo "FAIL: 第 $i 片生成失败（见 $OUT/build.log）"
                exit 1
            fi
            if ! "$BIN" --probe "$f" > /dev/null 2>&1; then
                echo "FAIL: 第 $i 片生成后校验不过"
                exit 1
            fi
        fi
        i=$((i + 1))
        # 进度：每 10 片报一次（含 ETA）
        if [ $((i % 10)) -eq 0 ] || [ "$i" -gt "$NSHARD" ]; then
            now=$(date +%s)
            el=$((now - t_start))
            done_edges=$(( (i - 1) * PER ))
            [ "$done_edges" -gt "$TOTAL" ] && done_edges=$TOTAL
            if [ "$done_edges" -gt 0 ] && [ "$el" -gt 0 ]; then
                eta=$(( (TOTAL - done_edges) / (done_edges / el + 1) ))
            else
                eta=0
            fi
            printf '  [%d/%d] 已完成 %d 边（%.1f%%）；已用 %d s，预计还要 %d s\n' \
                "$((i - 1))" "$NSHARD" "$done_edges" \
                "$(awk -v a="$done_edges" -v b="$TOTAL" 'BEGIN{printf "%.1f", 100.0*a/b}')" "$el" "$eta"
        fi
    done
fi

# ---------- 汇总 + 清单（每次重写全量） ----------
: > "$MAN"
printf '# 7B 边合成语料清单（tools/gen_corpus7b.sh 生成；§6.711）\n' >> "$MAN"
printf '# 格式：C7B v1（每边 8 字节；索引每段 16 字节）\n' >> "$MAN"
printf 'format c7b-v1\n' >> "$MAN"
printf 'per_shard %s\n' "$PER" >> "$MAN"
printf 'words %s\n' "$NW" >> "$MAN"
printf 'seed %s\n' "$SEED" >> "$MAN"
printf 'cap %s\n' "$CAP" >> "$MAN"
printf 'total_target %s\n' "$TOTAL" >> "$MAN"

nf=0; sum=0; bytes=0
for f in "$OUT"/shard_*.c7b; do
    [ -f "$f" ] || continue
    if out_=$("$BIN" --probe "$f" 2>/dev/null); then ok=1; else ok=0; fi
    e=$(printf '%s\n' "$out_" | sed -n 's/.* 边=\([0-9]*\)（.*/\1/p')
    b=$(printf '%s\n' "$out_" | sed -n 's/.*文件 \([0-9]*\) 字节.*/\1/p')
    # 完整 = 索引合计与头一致 + 文件长度一致（probe 的退出码就是这两条）
    if [ "$ok" = 0 ]; then
        echo "  ⚠ $(basename "$f")：清单里记下来了，但这一片**没通过校验**（见 --probe 输出）"
    fi
    printf 'shard %s %s %s\n' "$(basename "$f")" "${e:-0}" "${b:-0}" >> "$MAN"
    nf=$((nf + 1))
    sum=$((sum + ${e:-0}))
    bytes=$((bytes + ${b:-0}))
done

printf 'shards %d\n' "$nf" >> "$MAN"
printf 'total_edges %d\n' "$sum" >> "$MAN"
printf 'total_bytes %d\n' "$bytes" >> "$MAN"
if [ "$VERIFY_ONLY" = 0 ]; then
    if [ "$sum" -eq "$TOTAL" ]; then
        printf 'status COMPLETE\n' >> "$MAN"
    else
        printf 'status PARTIAL\n' >> "$MAN"
    fi
fi

echo
echo "== 汇总 =="
printf '  分片 %d 个；边合计 **%d**（目标 %s）；占用 %.1f GB\n' \
    "$nf" "$sum" "${TOTAL:-?}" \
    "$(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1000000000}')"
if [ -s "$MAN" ]; then
    grep -a '^status' "$MAN" | sed 's/^/  /'
fi
[ "$VERIFY_ONLY" = 1 ] && [ "$sum" -gt 0 ] && exit 0
[ "$nf" -gt 0 ] && [ "$sum" -eq "$TOTAL" ] && exit 0 || exit 1
