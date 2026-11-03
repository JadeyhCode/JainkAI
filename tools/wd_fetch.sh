#!/bin/sh
# tools/wd_fetch.sh — **可续传、并行分段**地抓公开 KG dump（§6.712）
#
# 为什么这么写：本机链路实测 **单连接 13 KB/s**、8 路并行 **70 KB/s**（16 路不再涨）。
# 所以"能不能薅到 7B"不是脚本问题，是**这条链路的上限**问题；脚本能做的只有两件事：
#   ① 把 70 KB/s 用满（8 路并行、分块、断点续传、失败重试）；
#   ② 每块**落地 + 记账**，随时能停、能续、能核对（不信"看起来下完了"）。
#
# 用法：sh tools/wd_fetch.sh <输出文件> <URL> <总字节数> [块大小 MB] [并发数]
#   块落在 `<输出文件>.chunks/<起始偏移>.part`；全部齐了才拼成 <输出文件>。重跑=续传。
set -u
OUT=${1:?用法: sh tools/wd_fetch.sh <输出文件> <URL> <总字节数> [块MB] [并发]}
URL=${2:?缺 URL}
TOTAL=${3:?缺总字节数}
CHUNK_MB=${4:-32}
JOBS=${5:-8}
CHUNK=$((CHUNK_MB * 1024 * 1024))
DIR="$OUT.chunks"
mkdir -p "$DIR" || exit 1

# 计划：按 CHUNK 切块（最后一块可能不满）
plan="$DIR/plan.txt"
if [ ! -s "$plan" ]; then
    i=0
    : > "$plan"
    while [ $((i * CHUNK)) -lt "$TOTAL" ]; do
        s=$((i * CHUNK))
        e=$((s + CHUNK - 1))
        [ "$e" -ge "$TOTAL" ] && e=$((TOTAL - 1))
        printf '%s %s %s\n' "$s" "$e" "$((e - s + 1))" >> "$plan"
        i=$((i + 1))
    done
fi
N=$(wc -l < "$plan")
echo "== $OUT：$TOTAL 字节 = $N 块 × ${CHUNK_MB} MB，$JOBS 路并行 =="

done_count() {   # 已完整落地的块数
    _n=0
    while read -r s e want; do
        f="$DIR/$s.part"
        [ -f "$f" ] || continue
        have=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$have" = "$want" ] && _n=$((_n + 1))
    done < "$plan"
    echo "$_n"
}

# 一个块的下载（被 xargs 并行调用；参数：起始 结束 应得字节）
# ⚠ 别用 `jobs -rp` 做作业池：dash 里它是 bash 扩展（实测一个块都没起）——用 xargs -P。
run_round() {
    # 只把"还不完整"的块交给 xargs（每行三个参数）
    : > "$DIR/todo.txt"
    while read -r s e want; do
        f="$DIR/$s.part"
        have=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$have" = "$want" ] && continue
        printf '%s %s %s\n' "$s" "$e" "$want" >> "$DIR/todo.txt"
    done < "$plan"
    [ -s "$DIR/todo.txt" ] || return 0
    # xargs 里不能直接调 shell 函数 ⇒ 把逻辑写成一段 -c 脚本
    xargs -a "$DIR/todo.txt" -n 3 -P "$JOBS" sh -c '
        DIR="$1"; URL="$2"; shift 2
        s=$1; e=$2; want=$3
        f="$DIR/$s.part"
        have=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$have" = "$want" ] && exit 0
        from=$((s + have))
        [ "$from" -gt "$e" ] && exit 0
        curl -k -s --max-time 1800 -r "$from-$e" "$URL" >> "$f" 2>/dev/null
        have=$(stat -c %s "$f" 2>/dev/null || echo 0)
        [ "$have" -ge "$want" ] && exit 0
        exit 1
    ' _ "$DIR" "$URL" 2>/dev/null || true
}

round=0
while :; do
    round=$((round + 1))
    run_round
    have=$(done_count)
    printf '  [第 %d 轮] 完整块 %d / %d（%.1f%%）\n' "$round" "$have" "$N" \
        "$(awk -v a="$have" -v b="$N" 'BEGIN{printf "%.1f", 100.0*a/b}')"
    [ "$have" -eq "$N" ] && break
    if [ "$round" -ge 40 ]; then
        echo "FAIL: 40 轮还差 $((N - have)) 块（链路太差？留着下次续传）"
        exit 1
    fi
done

echo "== 拼文件 =="
: > "$OUT.tmp"
while read -r s e want; do
    cat "$DIR/$s.part" >> "$OUT.tmp" || { echo "FAIL: 拼 $s 失败"; exit 1; }
done < "$plan"
SZ=$(stat -c %s "$OUT.tmp")
if [ "$SZ" != "$TOTAL" ]; then
    echo "FAIL: 拼完 $SZ 字节 ≠ 期望 $TOTAL"
    exit 1
fi
mv "$OUT.tmp" "$OUT"
echo "OK  $OUT（$SZ 字节）"
