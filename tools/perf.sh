#!/bin/sh
# tools/perf.sh — 性能与速度的完整画像（§6.755）
#
# 七项：① 载入 ② 图查询 ::rel ③ 思维 ::think ④ 端到端一问一答 ⑤ 吞吐 ⑥ RSS ⑦ 灌图/训练吞吐
# 方法：延迟一律用**差分法**（N 次 − 0 次，扣掉进程启动的固定开销），否则 23 ms 的启动会盖住 µs 级信号。
#
# 用法：sh tools/perf.sh [模型，默认 model.bin] [N，默认 2000]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
M=${1:-$ROOT/model.bin}
N=${2:-2000}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
ms() { date +%s%N | cut -c1-13; }

# ① 载入（三次取最小）
best=999999
for i in 1 2 3; do t0=$(ms); printf '::quit\n' | "$ROOT/build_aip" --model "$M" >/dev/null 2>&1; t1=$(ms); d=$((t1-t0)); [ "$d" -lt "$best" ] && best=$d; done
load=$best

# 空跑基线（只启动+退出）
printf '::quit\n' > "$T/e.in"
base=999999
for i in 1 2 3; do
    e0=$(ms); "$ROOT/build_aip" --model "$M" < "$T/e.in" >/dev/null 2>&1; e1=$(ms)
    d=$((e1-e0)); [ "$d" -lt "$base" ] && base=$d
done

diff_us() {  # $1=命令 $2=次数（三次取最小，避免磁盘/调度抖动把差分打成负数）
    yes "$1" | head -n "$2" > "$T/q.in"; echo '::quit' >> "$T/q.in"
    best=999999
    for i in 1 2 3; do
        t0=$(ms); "$ROOT/build_aip" --model "$M" < "$T/q.in" >/dev/null 2>&1; t1=$(ms)
        d=$((t1-t0)); [ "$d" -lt "$best" ] && best=$d
    done
    awk -v a="$best" -v b="$base" -v n="$2" 'BEGIN{ v=(a-b)*1000/n; if (v < 0) v = 0; printf "%.1f", v }'
}

rel_us=$(diff_us '::rel 猫' "$N")
think_us=$(diff_us '::think 猫' "$((N / 4))")
turn_ms=$(awk -v us=$(diff_us '猫是什么' "$((N / 4))") 'BEGIN{printf "%.1f", us/1000}')
qps=$(awk -v us="$rel_us" 'BEGIN{printf "%.0f", (us > 0) ? 1000000/us : 0}')
tps=$(awk -v ms="$turn_ms" 'BEGIN{printf "%.0f", (ms > 0) ? 1000/ms : 0}')
rss=$({ /usr/bin/time -f '%M' "$ROOT/build_aip" --model "$M" < "$T/e.in" >/dev/null; } 2>&1 | tail -1)
edges=$(printf '::stats\n::quit\n' | "$ROOT/build_aip" --model "$M" 2>/dev/null | grep -a '符号节点' | head -1 | sed 's/.*边=//')

# ⑦ 灌图吞吐（2000 条三元组）
: > "$T/ing.triples"
i=0; while [ $i -lt 2000 ]; do printf '测试节点%d\t属于\t测试类\n' "$i" >> "$T/ing.triples"; i=$((i+1)); done
t0=$(ms); printf '::ingest %s\n::quit\n' "$T/ing.triples" | "$ROOT/build_aip" --model "$M" >/dev/null 2>&1; t1=$(ms)
ing=$(awk -v d=$((t1-t0)) 'BEGIN{printf "%.0f", 2000000/d}')

printf '模型 %-22s 边 %-8s\n' "$(basename "$M")" "${edges:-?}"
printf '  载入 %4s ms | ::rel %s µs | ::think %s µs | 一问一答 %s ms | 查询 %s 条/秒 | 问答 %s 条/秒 | RSS %s MB | 灌图 %s 条/秒\n' \
    "$load" "$rel_us" "$think_us" "$turn_ms" "$qps" "$tps" "$(awk -v k="$rss" 'BEGIN{printf "%.0f", k/1024}')" "$ing"
