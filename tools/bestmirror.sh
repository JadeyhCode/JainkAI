#!/bin/sh
# tools/bestmirror.sh — 挑一个**当前能用**的镜像（同 distro 依次探测，返回第一个 200/206 的）§6.737
#
# 为什么要它：清华/中科大/阿里/北外在我们跑量后陆续返回 403（限流），硬编码一个镜像 =
# 一挂就整条管道停摆。这里按 `tools/mirrors.conf` 的顺序探测，并把结果缓存到
# `$CACHE`（默认 /mnt/models/.bestmirror.<distro>），下次优先用它。
#
# 用法：sh tools/bestmirror.sh <distro> <dist> <suite> [缓存文件]
#   输出：可用镜像基址（一行）；都不行则输出空并返回 1
set -u
DISTRO=${1:?}; DIST=${2:?}; SUITE=${3:?}
CACHE=${4:-/mnt/models/.bestmirror.$DISTRO}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

probe() {  # $1=base
    code=$(timeout 25 curl -k -s -o /dev/null -w '%{http_code}' --connect-timeout 8 --max-time 20 \
        -r 0-2000 "$1/dists/$DIST/$SUITE/source/Sources.gz" 2>/dev/null)
    [ "$code" = "200" ] || [ "$code" = "206" ]
}

# 先试上次那个
if [ -s "$CACHE" ]; then
    prev=$(head -1 "$CACHE")
    if probe "$prev"; then printf '%s\n' "$prev"; exit 0; fi
fi
for base in $(grep "^$DISTRO|" "$ROOT/tools/mirrors.conf" | cut -d'|' -f2); do
    if probe "$base"; then printf '%s\n' "$base" > "$CACHE"; printf '%s\n' "$base"; exit 0; fi
done
printf '' ; exit 1
