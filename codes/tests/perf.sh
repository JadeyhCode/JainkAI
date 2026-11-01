#!/bin/sh
# tests/perf.sh — **性能量尺**（阶段三 M0；§6.682）
#
# 为什么单独一支：`tests/scale.sh` 是"造大图 + 报数"的探路脚本，可它的计时是**单次抽样**
# （5 次平均），既没有分布也没有机器指纹 —— 拿它做"优化前 / 优化后"的判据，很容易把噪声当收益。
# 这一支只干一件事：**在同一台机器上可重复地量五个数**，并把机器指纹一起写下来。
#
# 五个数（阶段三的门槛就盯它们）：
#   ① 载入（load + ::quit）  ② 常驻内存 RSS  ③ 单条查询（::rel）
#   ④ 全表校验（::revcheck） ⑤ 存盘（::save）
# 每个数跑 R 组、每组 N 次，报 **min / 中位 / max**（不用平均值：一次卡顿就能把均值毁掉）。
#
# 用法：sh tests/perf.sh [S|M] [组数R] [每组次数N]
#   默认 S 档 / R=3 / N=50（S 档进整包，~10 s）；M 档手动跑（造图 + 载入各 10 s 上下）。
#
# ⚠ 口径（血泪，见 tests/scale.sh 头注释）：**先把"只加载 + ::quit"量出来当基线**，
#   再把命令块的总时间减掉基线、除以次数 —— 否则 10 s 的载入会被算进每条查询里。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TIER=${1:-S}
R=${2:-3}
N=${3:-50}
case "$TIER" in
    S)   NW=1200;   NF=41000;    GEN=mkbig;    ;;
    M)   NW=50000;  NF=2000000;  GEN=mkbig;    ;;
    K10) NW=200000; NF=10000000; GEN=mkcorpus; ;;   # 阶段三 M1：幂律 + 长尾（10^7 边）
    L)   NW=500000; NF=20000000; GEN=mkcorpus; ;;
    *) echo "用法: sh tests/perf.sh [S|M|K10|L] [组数] [次数]"; exit 2 ;;
esac
# 便宜的多跑几次（把噪声摊薄），贵的少跑；K10/L 光载入就要几十秒，只能少跑
case "$TIER" in
    S|M)   N_REL=50; N_CON=50; N_REV=2; N_SAV=2 ;;
    # K10/L 的枢纽词出度上万 ⇒ 一次 ::rel 就打几万行，次数必须压到个位数，
    # 否则量到的是**磁盘写**不是查询（§6.685 实测：100 次把 4 秒的会话拖成 5 分钟）
    K10|L) N_REL=2;  N_CON=3;  N_REV=1; N_SAV=1 ;;
esac
WORK="$ROOT/_t/perf"
mkdir -p "$WORK"
cd "$WORK"

CC=${CC:-gcc}
case "$GEN" in
    mkbig)    $CC -std=c11 -O2 -Wall -Wextra -o mkbig "$ROOT/tools/mkbig.c" -lm || { echo "FAIL: mkbig 编不过"; exit 1; } ;;
    mkcorpus) $CC -std=c11 -O2 -Wall -Wextra -o mkcorpus "$ROOT/tools/mkcorpus.c" -lm || { echo "FAIL: mkcorpus 编不过"; exit 1; } ;;
esac
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror || exit 1

BIG="$WORK/big_$TIER.bin"
if [ ! -f "$BIG" ]; then
    if [ "$GEN" = "mkcorpus" ]; then
        # 出度上限：§6.683 时是 2000（写入路径 O(出度)，Σdeg²/2 撑不住）；
        # §6.685 加了边存在性布隆之后放到 20000（更接近真语料的头，造价 27.7 s / 10^7 边）
        ./mkcorpus "$BIG" "$NW" "$NF" zipf tail 7 20000 || { echo "FAIL: 造图失败"; exit 1; }
    else
        ./mkbig "$BIG" "$NW" "$NF" 7 > /dev/null || { echo "FAIL: 造图失败"; exit 1; }
    fi
fi

MAXS=${MAXS:-120}          # 单次会话硬超时：超过就判该指标 TIMEOUT（基准脚本不许挂住）
[ "$TIER" = "K10" ] || [ "$TIER" = "L" ] && MAXS=${MAXS_OVERRIDE:-300}
# 跑一段会话：$1=命令文件 $2=输出前缀；回显 "<毫秒> <RSS_KB> <rc>"
runsess() {
    _in=$1; _pfx=$2
    _t0=$(date +%s%N)
    if command -v /usr/bin/time > /dev/null 2>&1; then
        timeout "$MAXS" /usr/bin/time -f '%M' -o "$_pfx.rss" ./aip --model "$BIG" < "$_in" > "$_pfx.out" 2>&1
        _rc=$?
    else
        timeout "$MAXS" ./aip --model "$BIG" < "$_in" > "$_pfx.out" 2>&1
        _rc=$?
        echo 0 > "$_pfx.rss"
    fi
    _t1=$(date +%s%N)
    _rss=$(cat "$_pfx.rss" 2>/dev/null || echo 0)
    echo "$(( (_t1 - _t0) / 1000000 )) ${_rss:-0} $_rc"
}

stat3() {  # 从一行空白分隔的数里报 min / 中位 / max
    printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[1], a[int((NR+1)/2)], a[NR]}'
}

echo "== 机器指纹 =="
printf '  机器: %s / %s 核 / 内存 %s\n' "$(uname -m)" "$(nproc 2>/dev/null || echo '?')" \
    "$(free -g 2>/dev/null | awk 'NR==2{print $2" GB"}' || echo '?')"
printf '  编译器: %s\n' "$($CC --version 2>/dev/null | head -1)"
printf '  档位: %s（%s 词 / %s 事实）\n' "$TIER" "$NW" "$NF"
printf '  采样: 每指标 %s 组；差分法（跑 n 与 2n 两个会话相减，载入抖动自动抵消）\n' "$R"

echo "== ① 载入（load + ::quit） =="
printf '::quit\n' > q.in
LOADS=""
LRSS=0
i=1
while [ "$i" -le "$R" ]; do
    o=$(runsess q.in "load.$i")
    ms=$(printf '%s\n' "$o" | cut -d' ' -f1)
    LRSS=$(printf '%s\n' "$o" | cut -d' ' -f2)
    _lrc=$(printf '%s\n' "$o" | cut -d' ' -f3)
    [ "${_lrc:-0}" -eq 124 ] && { echo "  [FAIL] 载入超过 ${MAXS}s（TIMEOUT）"; exit 1; }
    LOADS="$LOADS $ms"
    i=$((i + 1))
done
# shellcheck disable=SC2086
set -- $LOADS
BASELINE=$(stat3 $LOADS | cut -d' ' -f2)
LMIN=$(stat3 $LOADS | cut -d' ' -f1)
LMAX=$(stat3 $LOADS | cut -d' ' -f3)
printf '  载入 min/中位/max = %s / %s / %s ms；RSS = %s KB\n\n' "$LMIN" "$BASELINE" "$LMAX" "$LRSS"

# ⚠ **语义看护（§6.682 踩过）**：第一版把模型当成位置参数传（`./aip big.bin`），
#   结果它把 .bin 当**语料**读，10 s 的载入变成 110 ms、RSS 3 MB —— 数字全"变好"了，
#   其实是空图。所以这里必须证明"图真的在里面"：`::rel` 得能查出一条边。
printf '::rel w000001\n::quit\n' > sanity.in
runsess sanity.in "sanity" > /dev/null
if grep -qa 'w000001' sanity.out && grep -qaE '\->|属于' sanity.out; then
    printf '  语义看护：::rel w000001 查到了边（图真的载入了）\n\n'
else
    printf '  [FAIL] 语义看护：::rel w000001 查不到边 —— 图没载入（量出来的数没有意义）\n'
    printf '         证据：%s\n' "$(head -3 sanity.out | tr '\n' ' ')"
    exit 1
fi

# ---- 差分法（§6.682 第二版量尺）----
# 为什么不用"块时间 − 基线"：基线是**另一个进程**量出来的，M 档它自己就有 ±1.5 s 的抖动，
# 除以 2~5 次就变成 ±300~750 ms 的"单次成本" —— 抖动比要测的数还大。
# 差分法：同一个指标跑 T(n) 和 T(2n) 两个会话，**相减**，基线（载入）在同一次相减里抵消，
# 剩下的差 ÷ n 就是单次成本；分辨率由噪声 ÷ n 决定（n 越大越准，代价是时间）。
measure() {  # $1=标签（也是命令） $2=每组小会话的次数 n（大会话跑 2n）
    _cmd=$1
    _n=${2:-10}
    _vals=""
    _j=1
    while [ "$_j" -le "$R" ]; do
        _f1="blk1.in"; : > "$_f1"
        _f2="blk2.in"; : > "$_f2"
        _k=1
        while [ "$_k" -le "$_n" ]; do printf '%s\n' "$_cmd" >> "$_f1"; _k=$((_k + 1)); done
        printf '::quit\n' >> "$_f1"
        _k=1
        while [ "$_k" -le $((_n * 2)) ]; do printf '%s\n' "$_cmd" >> "$_f2"; _k=$((_k + 1)); done
        printf '::quit\n' >> "$_f2"
        _o1=$(runsess "$_f1" "b1.$_j"); _t1=$(printf '%s\n' "$_o1" | cut -d' ' -f1)
        _rc1=$(printf '%s\n' "$_o1" | cut -d' ' -f3)
        _o2=$(runsess "$_f2" "b2.$_j"); _t2=$(printf '%s\n' "$_o2" | cut -d' ' -f1)
        _rc2=$(printf '%s\n' "$_o2" | cut -d' ' -f3)
        if [ "${_rc1:-0}" -eq 124 ] || [ "${_rc2:-0}" -eq 124 ]; then
            printf '| %-46s | %5s | %5s | %5s | %3s |\n' "$_cmd" "-" ">${MAXS}s" "-" "超时"
            return 0
        fi
        _per=$(( (_t2 - _t1) / _n ))
        [ "$_per" -lt 0 ] && _per=0
        _vals="$_vals $_per"
        _j=$((_j + 1))
    done
    _s=$(stat3 $_vals)
    _mn=$(printf '%s' "$_s" | cut -d' ' -f1)
    _md=$(printf '%s' "$_s" | cut -d' ' -f2)
    _mx=$(printf '%s' "$_s" | cut -d' ' -f3)
    if [ "${_md:-0}" -gt 0 ]; then _jk=$(( (_mx - _mn) * 100 / _md )); else _jk=0; fi
    printf '| %-46s | %5s | %5s | %5s | %3s%% |\n' "$_cmd" "$_mn" "$_md" "$_mx" "$_jk"
}

echo "== ② 单条成本（已减载入；抖动 = (max-min)/中位） =="
printf '| %-46s | %5s | %5s | %5s | %4s |\n' "操作" "min" "中位" "max" "抖动"
printf '|---|---|---|---|---|\n'
# 便宜的路径多跑几次（把 ±1 s 的基线噪声摊薄），贵的路径少跑（它本身就有 2 s 量级）
measure "::rel w000001" "$N_REL"
measure "::contradict" "$N_CON"
measure "::revcheck" "$N_REV"
measure "::save $WORK/save_tmp.bin" "$N_SAV"

echo
{
    echo "tier=$TIER words=$NW facts=$NF groups=$R per=$N"
    echo "machine=$(uname -m) cores=$(nproc 2>/dev/null || echo '?')"
    echo "cc=$($CC --version 2>/dev/null | head -1)"
    echo "load_ms min=$LMIN med=$BASELINE max=$LMAX"
    echo "rss_kb=$LRSS"
    echo "date=$(date -Iseconds)"
} > "$WORK/last.txt"
echo "  → 指纹与上表已写入 $WORK/last.txt"
echo "PASS perf（$TIER 档：载入中位 ${BASELINE} ms / RSS ${LRSS} KB）"
