#!/bin/sh
# tests/bucketabl.sh — **按桶消融**：每个数据族各贡献多少分（§6.333）
#
# `AIP_NODATA=<桶名>` 只关掉那一桶；跑全部度量，与"全开"的差就是该族贡献。
# 同时打印该族的**行数**，给出"每修好一项要几行"——过拟合的硬指标。
# 不是回归套件（不进 suites.txt），跑一遍约几分钟。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
# `SETS` 可用环境变量覆盖（快速自测用，比如只跑一组）：
#     SETS=tests/holdout15.txt sh tests/bucketabl.sh
SETS=${SETS:-"tests/holdout.txt tests/holdout2.txt tests/holdout3.txt tests/holdout4.txt tests/holdout5.txt \
tests/holdout6.txt tests/holdout7.txt tests/holdout8.txt tests/holdout9.txt tests/holdout10.txt \
tests/holdout11.txt tests/holdout12.txt tests/holdout13.txt tests/holdout14.txt tests/holdout15.txt tests/holdout16.txt \
tests/conv.txt tests/conv2.txt tests/conv3.txt tests/conv4.txt"}
tot() {   # $1=额外的环境变量赋值（可为空）
    p=0; t=0
    for f in $SETS; do
        case "$f" in
            *conv*.txt) run=tests/conv.sh ;;
            *) run=tests/holdout.sh ;;
        esac
        line=$(env $1 sh "$run" "$MODEL" "$f" 2>/dev/null | head -1)
        a=$(printf '%s' "$line" | sed -n 's/.*：\([0-9]*\) 通过.*/\1/p')
        b=$(printf '%s' "$line" | sed -n 's/.*通过 \/ \([0-9]*\) [条轮]/\1/p')
        p=$((p+${a:-0})); t=$((t+${b:-0}))
    done
    echo "$p $t"
}
base=$(tot ""); bp=${base% *}; bt=${base#* }
printf '%-8s %10s %10s %6s %8s\n' "桶" "通过/总数" "本族贡献" "行数" "行/项"
# §6.380 桶名**从数据里发现**（以前写死 7 个：漏了 tail 和 §6.375 新加的 fill）
BUCKETS=${BUCKETS:-$(grep -oE '^\[[a-z]+\]' patterns.txt | tr -d '[]' | sort -u)}
for k in $BUCKETS; do
    ab=$(tot "AIP_NODATA=$k"); ap=${ab% *}
    rows=$(grep -c "^\[$k\]" patterns.txt || true)
    gain=$((bp - ap))
    if [ "$gain" -gt 0 ]; then ratio=$(awk -v r="$rows" -v g="$gain" 'BEGIN{printf "%.2f", r/g}'); else ratio="-"; fi
    printf '%-8s %10s %10s %6s %8s\n' "$k" "$ap/$bt" "$gain" "$rows" "$ratio"
done
echo "全开基线：$bp/$bt"
