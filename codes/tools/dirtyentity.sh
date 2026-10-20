#!/bin/sh
# tools/dirtyentity.sh — **脏实体 + 内层事实** 检查（§6.435；§6.437 加内层事实）
#
# 背景：`我说猫属于动物` 曾学出实体「我说猫」，`据说猫属于动物` 学出「据说猫」。
# 只查"没进图"还不够：如果传话被完全丢弃，脏实体也没有，但用户的话白说了。
# 所以每个用例查两件事：
#   ① 可疑名字**没进图**（`::rel X` ⇒ 不在图里）；
#   ② 内层事实**真的学到了**（问一句，答复里出现关键词）。
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

# 教一句 <TAB> 查的可疑名字 <TAB> 之后问的问题 <TAB> 答复里该有的关键词
cat > "$tmp/cases.txt" <<'CASES'
我说猫属于动物	我说猫	猫属于什么	动物
你说考拉属于动物	你说考拉	考拉属于什么	动物
据说猫属于动物	据说猫	猫属于什么	动物
听说狗属于动物	听说狗	狗属于什么	动物
我认为猫属于动物	我认为猫	猫属于什么	动物
你觉得猫属于动物	你觉得猫	猫属于什么	动物
人家说猫属于动物	人家说猫	猫属于什么	动物
据我所知猫属于动物	据我所知猫	猫属于什么	动物
我们听说狗属于动物	我们听说狗	狗属于什么	动物
你觉得鲸鱼是鱼吗	你觉得鲸鱼	鲸鱼属于什么	哺乳
据说他属于动物	据说他	他属于什么	没 记 过
CASES

bad_name=0; bad_fact=0; total=0
while IFS="$(printf '\t')" read -r teach suspect ask want; do
    [ -n "$teach" ] || continue
    total=$((total + 1))
    out=$( { printf '::load %s\n' "$MODEL"
             printf '%s\n' "$teach"
             printf '::rel %s\n' "$suspect"
             printf '%s\n' "$ask"
             printf '::quit\n'; } | "$BIN" 2>&1 )
    name_ok=0; fact_ok=0
    case "$out" in *"[$suspect] 不在图里"*) name_ok=1 ;; esac
    ans=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | tail -1)
    case "$ans" in *"$want"*) fact_ok=1 ;; esac
    [ "$name_ok" -eq 1 ] || { printf '  [脏名] %-12s 进了图（教：%s）\n' "$suspect" "$teach"; bad_name=$((bad_name + 1)); }
    [ "$fact_ok" -eq 1 ] || { printf '  [没学到] %-10s 问「%s」⇒ %s\n' "$suspect" "$ask" "$ans"; bad_fact=$((bad_fact + 1)); }
    [ "$name_ok" -eq 1 ] && [ "$fact_ok" -eq 1 ] && printf '  [OK]   %-12s 没进图，且内层事实学到了\n' "$suspect"
done < "$tmp/cases.txt"

echo "脏实体：$bad_name / $total；内层事实没学到：$bad_fact / $total"
[ "$bad_name" -eq 0 ] && [ "$bad_fact" -eq 0 ] \
    && echo "PASS dirtyentity（传话/传闻没被当成实体，内层事实也学到了）"
exit 0
