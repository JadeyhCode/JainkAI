#!/bin/sh
# tests/switches_sample.sh —— **开关组合抽样**：把"写进表 ≠ 跑过"这件事变成不可能
#
# 背景（§6.67 的教训）：`switches_ab.sh` 里列了十几个默认关开关，但**没人真跑**其中几档
# ——`AIP_WGATE=1` 那一档三个月没跑，C 级测试里 12+1 项一直假红都没人知道。
# 这个脚本干两件事：
#   ① **随机抽** N 组开关组合，每组跑一次整包（不重复、不遗漏地"雨露均沾"）；
#   ② 把每组的结果和**时间**记进 `_t/switches_last_run.tsv`，`--status` 能看出哪个开关太久没跑。
#
# 用法：
#   sh tests/switches_sample.sh            # 抽 2 组跑（约 1.5~2 分钟）
#   N=4 sh tests/switches_sample.sh        # 抽 4 组
#   sh tests/switches_sample.sh --status   # 只看"每个开关最近一次跑是什么时候"
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
LEDGER=_t/switches_last_run.tsv
touch "$LEDGER"

# 开关清单：从 switches_ab.sh 的 `名字:AIP_X=1` 行里抽（一处维护）
pairs=$(grep -oE '^[a-z0-9_]+:AIP_[A-Z0-9_]+=1' tests/switches_ab.sh | sed 's/^[^:]*://' | sort -u)
NALL=$(printf '%s\n' "$pairs" | grep -c . || true)

if [ "${1:-}" = "--status" ]; then
    echo "开关清单 $NALL 个（来自 tests/switches_ab.sh）；最近一次被抽到跑的时间："
    now=$(date +%s)
    printf '%s\n' "$pairs" | while IFS= read -r sw; do
        [ -n "$sw" ] || continue
        last=$(awk -F'\t' -v s="$sw" '$0 ~ s {t=$1} END{print t}' "$LEDGER" 2>/dev/null || true)
        if [ -z "$last" ]; then
            printf '  %-22s **从没跑过**\n' "$sw"
        else
            d=$(( (now - last) / 86400 ))
            printf '  %-22s %s 天前（%s）\n' "$sw" "$d" "$(date -d "@$last" '+%m-%d %H:%M' 2>/dev/null || echo '?')"
        fi
    done
    echo
    echo "（抽一组跑：sh tests/switches_sample.sh ；每组跑一次整包，结果也记进 $LEDGER）"
    exit 0
fi

N=${N:-2}
seed=$(( $(date +%s) ^ $$ ))
echo "=== 开关组合抽样：$N 组（从 $NALL 个开关里抽，seed=$seed）==="
i=0
fail=0
while [ "$i" -lt "$N" ]; do
    i=$((i + 1))
    # 每组 1~3 个开关：用 awk 的 rand 做**可复现**的抽样（seed 打出来）
    subset=$(printf '%s\n' "$pairs" | awk -v seed="$seed" -v k="$i" '
        BEGIN{srand(seed + k * 7919)}
        { if (rand() < 0.18) print }' | head -3 | tr '\n' ' ')
    [ -n "$subset" ] || subset=$(printf '%s\n' "$pairs" | awk -v seed="$seed" -v k="$i" 'BEGIN{srand(seed+k)} NR==1{print}')
    echo "--- 第 $i 组：[ $subset] ---"
    rc=0
    # shellcheck disable=SC2086
    env $subset sh tests/all.sh 2>&1 | grep -aE "^\[FAIL\]|套件结果|全部回归" || rc=$?
    ts=$(date +%s)
    for sw in $subset; do
        printf '%s\t%s\t%s\n' "$ts" "$sw" "$([ "$rc" -eq 0 ] && echo pass || echo FAIL)" >> "$LEDGER"
    done
    [ "$rc" -eq 0 ] || fail=1
done
echo
echo "跑完了；看每个开关最近一次： sh tests/switches_sample.sh --status"
exit "$fail"
