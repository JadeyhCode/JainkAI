#!/bin/sh
# tests/datacheck.sh — **数据行有没有让"学得到"变成"学不到"**（§6.313）
#
# 做法：固定一组自然说法（tests/datacorpus.txt），`AIP_NODATA=0/1` 各跑一遍**逐句对照**；
# 如果无数据时答复里出现「记下/知道/改过来」而**有数据时没有**，说明某条数据行
# 把这条**带内容**的说法抢走了（§6.312 的 `教我` 就是这样被发现的）。
# 每句一个进程（互不污染）。这不是回归套件（不进 suites.txt）：它出的是**清单**。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
CORPUS=${2:-tests/datacorpus.txt}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }

one() {   # $1=句子 $2=1 表示关数据
    if [ "${2:-0}" = 1 ]; then
        printf '%s\n::quit\n' "$1" | AIP_NODATA=1 timeout 20 "$BIN" --model "$MODEL" 2>&1
    else
        printf '%s\n::quit\n' "$1" | timeout 20 "$BIN" --model "$MODEL" 2>&1
    fi | sed -n 's/.*AI > //p' | head -1
}

n=0; flag=0
while IFS= read -r q; do
    [ -n "$q" ] || continue
    case "$q" in \#*) continue ;; esac
    n=$((n+1))
    on=$(one "$q" 0)
    off=$(one "$q" 1)
    case "$off" in
    *记下*|*知道*|*改\ 过来*)
        case "$on" in
        *记下*|*知道*|*改\ 过来*) : ;;
        *) flag=$((flag+1)); printf '  ⚠「%s」\n     有数据: %s\n     无数据: %s\n' "$q" "$on" "$off" ;;
        esac ;;
    esac
done < "$CORPUS"
echo "数据行误伤检查：$n 句，可疑 $flag 句"
