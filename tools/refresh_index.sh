#!/bin/sh
# tools/refresh_index.sh — 定期重建"待做索引"（§6.744）
#
# 为什么：`index.<k>.txt` 是**建成那一刻**的待做快照；worker 做完的包只会进 `done.all.txt`，
# 不会从索引里消失 ⇒ 跑一阵后 worker 全是"本批：0 个包"（实测）。
# 这里每 10 分钟重建一次三个语料的索引。
#
# 用法：sh tools/refresh_index.sh [间隔秒，默认 600]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INT=${1:-600}
while :; do
    sh "$ROOT/tools/rebuild_index.sh" /mnt/models/ubucorpus universe 33 > /dev/null 2>&1
    sh "$ROOT/tools/rebuild_index.sh" /mnt/models/debtrix main 33 > /dev/null 2>&1
    sh "$ROOT/tools/rebuild_index.sh" /mnt/models/ubumain main 33 > /dev/null 2>&1
    printf '[%s] 索引已刷新：ubu %s / trix %s / main %s\n' "$(date '+%H:%M:%S')" \
        "$(cat /mnt/models/ubucorpus/index.*.txt 2>/dev/null | wc -l)" \
        "$(cat /mnt/models/debtrix/index.*.txt 2>/dev/null | wc -l)" \
        "$(cat /mnt/models/ubumain/index.*.txt 2>/dev/null | wc -l)" >> /mnt/models/refresh_index.log
    sleep "$INT"
done
