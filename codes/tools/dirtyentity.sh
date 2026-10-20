#!/bin/sh
# tools/dirtyentity.sh — **脏实体检查**（§6.435）
#
# 背景：`我说猫属于动物` 曾学出实体「我说猫」，`据说猫属于动物` 学出「据说猫」。这类节点是
# "把传话/传闻当成了东西"。这个脚本把常见说法各教一句，再 `::rel` 查"脏名字"是否进了图。
#
# 用法：sh tools/dirtyentity.sh [模型]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

# 每个用例：教一句 → 查一个"不该存在的名字"
cat > "$tmp/cases.txt" <<'CASES'
我说猫属于动物	我说猫
你说考拉属于动物	你说考拉
据说猫属于动物	据说猫
听说狗属于动物	听说狗
我认为猫属于动物	我认为猫
你觉得猫属于动物	你觉得猫
人家说猫属于动物	人家说猫
CASES

bad=0; total=0
while IFS="$(printf '\t')" read -r teach suspect; do
    [ -n "$teach" ] || continue
    total=$((total + 1))
    out=$( { printf '::load %s\n' "$MODEL"
             printf '%s\n' "$teach"
             printf '::rel %s\n' "$suspect"
             printf '::quit\n'; } | "$BIN" 2>&1 )
    case "$out" in
    *"[$suspect] 不在图里"*)
        printf '  [OK]   %-12s 没进图（教的是：%s）\n' "$suspect" "$teach" ;;
    *)
        printf '  [脏]   %-12s 进了图（教的是：%s）\n' "$suspect" "$teach"
        bad=$((bad + 1)) ;;
    esac
done < "$tmp/cases.txt"

echo "脏实体：$bad / $total"
[ "$bad" -eq 0 ] && echo "PASS dirtyentity（传话/传闻没有被当成实体）"
exit 0
