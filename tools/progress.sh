#!/bin/sh
# tools/progress.sh — 「7B 真知识」进度看板：各源条数 / 合计 / 速率 / 到 7e9 的 ETA（§6.724）
#
# 数据都在 /mnt/models 下的四条源目录里（Debian 分片 + PyPI/gems/npm 文本）。
# 速率用**两次调用之间的差值**算（基线藏在 `.progress_base`），所以第一次调用只出总数。
#
# 用法：sh tools/progress.sh [--json]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
M=${MODELS:-/mnt/models}
BASE="$M/.progress_base"

# Debian 分片里的边数（读 v2 头里的 n_edges，u64 @ 偏移 16）
shard_edges() {   # $1 = 语料目录
    [ -d "$1/shards" ] || { echo 0; return; }
    for f in "$1"/shards/*.gz; do
        [ -f "$f" ] || continue
        gunzip -c "$f" 2>/dev/null | head -c 64 | od -An -tu8 -j 16 -N 8 | tr -d ' '
    done | awk '{s += $1} END {print s + 0}'
}
text_lines() { [ -d "$1" ] || { echo 0; return; }; cat "$1"/facts*.triples 2>/dev/null | wc -l; }
pkg_count() { [ -d "$1" ] || { echo 0; return; }; cat "$1"/done*.txt 2>/dev/null | wc -l; }

d_edges=$(shard_edges "$M/codecorpus")
u_edges=$(shard_edges "$M/ubucorpus")
pypi=$(text_lines "$M/pycorpus")
gems=$(text_lines "$M/gemcorpus")
npm=$(text_lines "$M/npmcorpus")
debian_pend=$(cat "$M"/codecorpus/facts.*.triples 2>/dev/null | wc -l)
# ⚠ **待转文本行不算进总账**：行和边不是同一量纲（一次转换会让"合计"倒退，速率变负）。
#   积压单独报，总账只算**已经成边**的（分片）与其它源的文本条数。
total=$((d_edges + u_edges + pypi + gems + npm))
target=7000000000
now=$(date +%s)

echo "== 7B 真知识进度（$(date '+%F %T')）=="
printf '  Debian  分片 %6s 片 / %12s 条边（文本待转 %s 行）\n' \
    "$(ls "$M"/codecorpus/shards/*.gz 2>/dev/null | wc -l)" "$d_edges" "$debian_pend"
printf '  Ubuntu  分片 %6s 片 / %12s 条边（文本待转 %s 行）\n' \
    "$(ls "$M"/ubucorpus/shards/*.gz 2>/dev/null | wc -l)" "$u_edges" \
    "$(cat "$M"/ubucorpus/facts.*.triples 2>/dev/null | wc -l)"
printf '  PyPI    %6s 包 / %12s 条\n' "$(pkg_count "$M/pycorpus")" "$pypi"
printf '  gems    %6s 个 / %12s 条\n' "$(pkg_count "$M/gemcorpus")" "$gems"
printf '  npm     %6s 包 / %12s 条\n' "$(pkg_count "$M/npmcorpus")" "$npm"
printf '  ─────────────────────────────────────────────\n'
printf '  合计 %12s 条 = %.2f%%（目标 %s）\n' "$total" \
    "$(awk -v a="$total" -v b="$target" 'BEGIN{printf "%.2f", 100*a/b}')" "$target"

if [ -s "$BASE" ]; then
    read -r bt bv < "$BASE" 2>/dev/null || { bt=0; bv=0; }
    # 窗口太短时速率会被"文本转分片"的抖动放大（一次转换：待转行 -7M、分片边 +3M）
    if [ "${bt:-0}" -gt 0 ] && [ $((now - bt)) -ge 300 ]; then
        dt=$((now - bt)); dv=$((total - bv))
        rate=$(awk -v d="$dv" -v t="$dt" 'BEGIN{printf "%.1f", (t>0? d/t*60 : 0)}')
        printf '  速率 %s 条/分钟（近 %s 分钟 +%s 条）\n' "$rate" "$((dt / 60))" "$dv"
        awk -v r="$rate" -v a="$total" -v b="$target" 'BEGIN{
            left = b - a;
            # ⚠ 别用 %d：7e9 超出 int32 ⇒ 会打出 2147483647（awk 内部是 double，格式化才溢出）
            if (r > 0) printf "  ETA  %.1f 天（还需 %.0f 条）\n", left / r / 1440, left;
            else printf "  ETA  —（速率为 0，抓取可能停了）\n";
        }'
    fi
fi
printf '%s %s\n' "$now" "$total" > "$BASE"

if [ "${1:-}" = "--json" ]; then
    printf '{"debian":%s,"pyproc":%s,"gems":%s,"npm":%s,"total":%s,"target":%s}\n' \
        "$d_edges" "$pypi" "$gems" "$npm" "$total" "$target"
fi
