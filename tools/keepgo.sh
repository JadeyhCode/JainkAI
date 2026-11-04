#!/bin/sh
# tools/keepgo.sh — 抓取"续跑器"：worker 干完自己那片会正常退出，这里负责**换分片数重启**（§6.735）
#
# 背景：`src_harvest.sh` 的 `--slice k/N` 是固定映射（NR % N == k），一个 worker 把自己那片
# 处理完就退出（实测 32 个 worker 全部留了"本批"收尾日志后死掉，速率从 14 887 掉到 1 447 边/秒）。
# 这里每轮把 N 变一下（N = N*2-1 后再回落到基数），映射随之改变 ⇒ worker 能继续拿到没做过的包；
# 全局 `done.all.txt` 保证不重复处理。
#
# 用法：sh tools/keepgo.sh <语料目录> <worker 序号> <基数 N> <镜像> <发行版> <套件>
set -u
OUT=${1:?}; K=${2:?}; N0=${3:?}; MIRROR=${4:?}; DIST=${5:?}; SUITE=${6:?}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
N=$N0
while :; do
    LOG="$OUT/log.$K.txt"
    # ⚠ N 固定：换 N 会重建索引（14 MB zcat+awk），36 个续跑器频繁换 N 会把 IO/CPU 吃光
    #   （实测 85 worker 只产出 971 边/秒）。去重靠全局 done.all.txt，不靠换 N 覆盖。
    :
    # 每一轮先探一次镜像：被限流（403）就自动换下一个可用的（§6.737）
    case "$MIRROR" in
    *huaweicloud*) DISTRO=debian ;; *nju*|*163*|*aliyun*) DISTRO=debian ;;
    *) DISTRO=debian ;;
    esac
    case "$DIST" in noble|jammy) DISTRO=ubuntu ;; esac
    M=$(sh "$ROOT/tools/bestmirror.sh" "$DISTRO" "$DIST" "$SUITE" 2>/dev/null)
    [ -n "${M:-}" ] || M=$MIRROR
    MIRROR=$M
    if sh "$ROOT/tools/src_harvest.sh" "$OUT" 5000 --slice "$K/$N" \
        --mirror "$MIRROR" --dist "$DIST" --suite "$SUITE" >> "$LOG" 2>&1; then
        :
    fi
    sleep 5
done
