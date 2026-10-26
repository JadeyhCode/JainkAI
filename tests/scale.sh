#!/bin/sh
# tests/scale.sh — **规模门槛**（阶段二 WP0；§6.632）
#
# 干什么：用 `tools/mkbig.c` 造**合成**图（不灌语料、不进仓库），量四件事：
#   ① 载入耗时；② 常驻内存（RSS）；③ 单条查询耗时（`::rel` / `::contradict` / `::revcheck`）；
#   ④ 存盘耗时。
#
# 为什么这么量：踩过"把载入时间算进查询时间"的坑（第一次量 M 档：每条命令 11 s，
# 其实 11.2 s 全是**载入**，查询本身 ≈0 ms）。所以脚本先量一次"只加载 + ::quit"当基线，
# 再用"同一条命令跑 5 次"减去基线，除以 5 得到**单次**成本。
#
# 用法：sh tests/scale.sh [S|M|L] [词数] [事实数]
#   默认 S = 1200 词 / 4.1 万事实；M = 5 万 / 200 万；L = 50 万 / 2000 万（L 很吃内存，慎跑）
#   环境变量 AIP_KEEP=1 保留生成的图（默认删）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TIER=${1:-S}
case "$TIER" in
    S) NW=${2:-1200};   NF=${3:-41000} ;;
    M) NW=${2:-50000};  NF=${3:-2000000} ;;
    L) NW=${2:-500000}; NF=${3:-20000000} ;;
    *) echo "用法: sh tests/scale.sh [S|M|L] [词数] [事实数]"; exit 2 ;;
esac
WORK="$ROOT/_t/scale"
mkdir -p "$WORK"
cd "$WORK"

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -o mkbig "$ROOT/tools/mkbig.c" -lm || { echo "FAIL: mkbig 编不过"; exit 1; }
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror || exit 1

BIG="$WORK/big_$TIER.bin"
echo "== 造图（$TIER：$NW 词 / $NF 事实） =="
./mkbig "$BIG" "$NW" "$NF" 7 || { echo "FAIL: 造图失败"; exit 1; }

# 一条会话跑完：$1=输出文件；从 stdin 读命令序列；返回毫秒 + RSS
runsess() {
    _out=$1
    _t0=$(date +%s%N)
    if command -v /usr/bin/time >/dev/null 2>&1; then
        /usr/bin/time -f "AIP_RSS_KB=%M" ./aip --model "$BIG" > "$_out" 2>"$_out.time"
        _rc=$?
        _rss=$(sed -n 's/.*AIP_RSS_KB=\([0-9]*\).*/\1/p' "$_out.time" | tail -1)
    else
        ./aip --model "$BIG" > "$_out" 2>&1
        _rc=$?
        _rss="?"
    fi
    _t1=$(date +%s%N)
    echo "$(( (_t1 - _t0) / 1000000 )) $_rc ${_rss:-?}"
}

echo "== 基线（只加载 + ::quit） =="
printf '::notepad off\n::quit\n' > base.txt
set -- $(runsess base.out < base.txt)
BASE_MS=$1; BASE_RC=$2; BASE_RSS=$3
[ "$BASE_RC" -eq 0 ] || { echo "FAIL: 载入失败（rc=$BASE_RC）"; exit 1; }
echo "  载入 ${BASE_MS} ms / RSS ${BASE_RSS} KB"

# 单条命令：跑 5 次，减去基线，除以 5
bench() {
    _name=$1; _cmd=$2
    { printf '::notepad off\n'; for _i in 1 2 3 4 5; do printf '%s\n' "$_cmd"; done; printf '::quit\n'; } > one.txt
    set -- $(runsess one.out < one.txt)
    _tot=$1; _rc=$2
    _per=$(( (_tot - BASE_MS) / 5 ))
    [ "$_per" -lt 0 ] && _per=0
    printf '| %-14s | %6s ms | rc=%s |\n' "$_name" "$_per" "$_rc"
}

echo "== 单条查询/维护（5 次平均，已减载入） =="
echo "| 操作 | 单次耗时 | 备注 |"
echo "|---|---|---|"
bench "::rel"        '::rel w000001'
bench "::contradict" '::contradict' 2>/dev/null || bench "::contradict" '::contradict'
bench "::revcheck"   '::revcheck'
bench "::save"       "::save $WORK/roundtrip.bin"

echo
echo "注：S/M/L 不是越大越好；门槛只盯两个数 —— 载入时间与单条查询时间"
echo "    （它们决定"这个规模还能不能交互"），RSS 是第三个数（机器扛不扛得住）。"

[ -n "${AIP_KEEP:-}" ] || rm -f "$BIG" roundtrip.bin
echo "PASS scale（$TIER：载入 ${BASE_MS} ms / RSS ${BASE_RSS} KB）"
