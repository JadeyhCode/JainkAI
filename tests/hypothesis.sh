#!/bin/sh
# tests/hypothesis.sh — 反事实推理（假设世界）的回归
#
# 「如果 猫 会 飞，那 猫 会 游泳 吗」需要一个**假设世界**。
# 看护五件事，第 ④ 条是重中之重：
#   ① 设假设 → 在假设下推理，链上那一步标「假设」，答复写明「在假设下」
#   ② 假设**不覆盖**无关的已知事实（猫 不会游泳 依然成立）
#   ③ 假设能沿规则外推（如果 猫 属于 鸟 ⟹ 猫 会 飞，靠「继承」）
#   ④ **假设绝不进图**：撤回后回到原样；存盘再读也带不出来
#   ⑤ 与已知事实冲突时如实说明，不假装没看见
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }

TMP=./_hyp_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }
run() {
    { printf '::load %s\n::rel analogy off\n::notepad off\n::rel analogy off\n' "$MODEL"
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'
    } | "$BIN" 2>&1
}

echo "① 在假设世界里推理"
out=$(run "如果 猫 会 飞" "猫 会 飞 吗" "::der")
printf '%s\n' "$out" | grep -E 'AI >|假设 [✓✗]|→ 这么说' | sed 's/^你 > /     /'
printf '%s\n' "$out" | grep -q '在 假设 下 ' && chk "答复写明「在 假设 下」" 1 \
                                          || chk "答复写明「在 假设 下」" 0
printf '%s\n' "$out" | grep -q '假设 ✓' && chk "链上那一步标成「假设」并验证过" 1 \
                                        || chk "链上那一步标成「假设」并验证过" 0

echo "② 假设不覆盖无关的已知事实"
out=$(run "如果 猫 会 飞" "猫 会 游泳 吗" "猫 属于 植物 吗")
r2=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | sed -n 2p)
r3=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | sed -n 3p)
echo "     猫 会 游泳 吗   → $r2"
echo "     猫 属于 植物 吗 → $r3"
case "$r2" in *"不 对"*"猫 -[不 会]-> 游泳"*) chk "「猫 不会游泳」仍然成立" 1 ;;
             *) chk "「猫 不会游泳」仍然成立（实得「$r2」）" 0 ;; esac
case "$r3" in *"不 对"*) chk "「猫 不属于植物」仍然成立" 1 ;;
             *) chk "「猫 不属于植物」仍然成立（实得「$r3」）" 0 ;; esac

echo "③ 假设沿规则外推（猫 属于 鸟 ⟹ 猫 会 飞）"
out=$(run "如果 猫 属于 鸟" "猫 会 飞 吗" "::der")
printf '%s\n' "$out" | grep -E 'AI >|假设 [✓✗]|事实 [✓✗]|缺口' | sed 's/^你 > /     /'
printf '%s\n' "$out" | grep -q '缺口=0' && chk "推导链完整（缺口=0）" 1 || chk "推导链完整" 0
printf '%s\n' "$out" | grep -q '被继承/传递规则用上' \
    && chk "假设那一步被「继承」用上（这就是外推）" 1 \
    || chk "假设那一步被「继承」用上" 0
printf '%s\n' "$out" | grep -q '鸟 -\[会\]-> 飞' \
    && chk "依据是图里的真事实「鸟 会 飞」" 1 || chk "依据是图里的真事实" 0

echo "④ **假设绝不进图**（撤回 / 存盘都带不出来）"
out=$(run "如果 猫 会 飞" "猫 会 飞 吗" "算了" "猫 会 飞 吗")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | tail -1)
echo "     撤回后 → $r"
case "$r" in *"不 对"*"猫 -[不 会]-> 飞"*) chk "撤回后回到原本的知识" 1 ;;
             *) chk "撤回后回到原本的知识（实得「$r」）" 0 ;; esac
{ printf '::load %s\n::rel analogy off\n如果 猫 会 飞\n::save %s/m.bin\n::load %s/m.bin\n::rel analogy off\n猫 会 飞 吗\n::quit\n' \
      "$MODEL" "$TMP" "$TMP"; } | "$BIN" 2>&1 | sed -n 's/.*AI > //p' > "$TMP/persist.txt"
last=$(tail -1 "$TMP/persist.txt")
echo "     存盘重载后 → $last"
case "$last" in *"不 对"*) chk "存盘重载后假设不存在（说明它从没写进图）" 1 ;;
              *) chk "存盘重载后假设不存在（实得「$last」）" 0 ;; esac
# 再确认一次：文件里确实没有「猫 属于/会 飞」这类被污染的关系
out=$(run "::rel 猫")
printf '%s\n' "$out" | grep -q -- '--\[不 会\]--> 飞' \
    && chk "知识库里 猫 与 飞 的关系仍是「不 会」" 1 \
    || chk "知识库里 猫 与 飞 的关系仍是「不 会」" 0

echo "⑤ 与已知事实冲突要如实说"
out=$(run "如果 猫 会 飞")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | head -1)
echo "     $r"
case "$r" in *"不过 我 知道 实际 上 不是 这样"*) chk "冲突时说明了「实际上不是这样」" 1 ;;
             *) chk "冲突时说明了「实际上不是这样」（实得「$r」）" 0 ;; esac
out=$(run "如果 猫 属于 宠物")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | head -1)
case "$r" in *"不过"*) chk "不冲突时不该乱报冲突" 0 ;;
             *) chk "不冲突时不乱报冲突（$r）" 1 ;; esac

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS hypothesis（假设世界：可推 / 不越界 / 能外推 / 不进图 / 冲突如实说）" \
                  || { echo "FAIL hypothesis"; exit 1; }
