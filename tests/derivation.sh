#!/bin/sh
# tests/derivation.sh — 推导层（链是一等公民，文字只是它的投影）
#
# 看护四件事：
#   ① 链是**结构化**的：按步摊开，每步标明类型 + 验证标记 + 用了哪条规则
#   ② 验证器真的在验：直接事实、规则合法性、结论可重推，都要独立复查
#   ③ **有缺口就不许下断言** —— 这是整层最要紧的规矩
#   ④ 语言是投影：同一份链换 4 种说法，**结论不变**
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }

run() {
    { printf '::load %s\n::char off\n' "$MODEL"
      # 钉住两个类比开关：这一套测的是**默认档**语义（反向测量时不能被 env 掀掉）
      printf '::rel analogy off\n::rel analogy solver off\n'
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'
    } | "$BIN" 2>&1
}

echo "① 链是结构化的（按步摊开、带规则名和验证标记）"
out=$(run "猫 有 毛 吗" "::der")
echo "$out" | sed -n '/推导\]/,/这么说/p' | sed 's/^你 > /     /'
printf '%s\n' "$out" | grep -q '规则 ✓' && chk "规则步带验证标记 ✓" 1 || chk "规则步带验证标记 ✓" 0
printf '%s\n' "$out" | grep -q '（继承）' && chk "标出了用的是哪条规则（继承）" 1 || chk "标出了用的是哪条规则" 0
printf '%s\n' "$out" | grep -q '缺口=0' && chk "缺口=0（链是完整的）" 1 || chk "缺口=0" 0
printf '%s\n' "$out" | grep -q '事实 ✓.*动物 -\[有\]-> 毛' \
    && chk "事实步被独立复查过" 1 || chk "事实步被独立复查过" 0

echo "② 传递链也标出规则，且符号化给的是两环前提"
out=$(run "::style 3" "猫 属于 生物 吗" "::der")
printf '%s\n' "$out" | grep -q '猫 属于 生物  ⇐  猫 属于 动物  ∧  动物 属于 生物' \
    && chk "符号化写出「结论 ⇐ 前提 ∧ 前提」" 1 \
    || chk "符号化写出「结论 ⇐ 前提 ∧ 前提」" 0
printf '%s\n' "$out" | grep -q '（传递）' && chk "传递链标了「传递」" 1 || chk "传递链标了「传递」" 0

echo "③ 缺口 → 不下断言"
out=$(run "::der 猫 有 翅膀")
printf '%s\n' "$out" | grep -q '缺口=0' \
    && chk "「猫 有 翅膀」有明确结论（知识库里有否定事实，不算缺口）" 1 \
    || chk "「猫 有 翅膀」有明确结论" 0
# 探针要挑**真的没有知识**的：`猫 会 唱歌` 现在被「缺前提」规则接住了
# （唱歌 需要 灵活的声带 + 猫 没有 灵活的声带 ⟹ 不会，见 tests/prerequisite.sh），
# 它不再是「推不出来」的例子。这里换成知识库里确实没有的「跳舞」。
out=$(run "::der 猫 会 跳舞")
echo "$out" | sed -n '/推导\]/,/这么说/p' | sed 's/^你 > /     /'
printf '%s\n' "$out" | grep -q '缺口=1' \
    && chk "推不出来时链上**留下缺口**（缺口=1）" 1 \
    || chk "推不出来时链上留下缺口" 0
printf '%s\n' "$out" | grep -q '推不出来' \
    && chk "有缺口时只报「推不出来」，不下断言" 1 \
    || chk "有缺口时只报「推不出来」" 0

echo "④ 语言是投影：同一份链，4 种说法，结论不变"
# 先把风格设成符号化，问一次（这次是**直接回答**），然后依次换风格让它把同一份链重说四遍
out=$(run "::style 3" "猫 属于 生物 吗" "::style 0" "::style 1" "::style 2" "::style 3")
echo "$out" | grep -E 'AI >|现在这么说' | sed 's/^你 > /     /'
# 直接回答：符号化风格 —— 结论仍是「对」
printf '%s\n' "$out" | grep -q 'AI > 对 ： 猫 属于 生物  ⇐  猫 属于 动物  ∧  动物 属于 生物' \
    && chk "符号化风格直接回答：结论仍是「对」，且给出两环前提" 1 \
    || chk "符号化风格直接回答：结论仍是「对」" 0
# 同一份链换四种说法，真值不变
printf '%s\n' "$out" | grep -q '现在这么说：对$' \
    && chk "风格 0（只给结论）→ 「对」" 1 || chk "风格 0 → 「对」" 0
printf '%s\n' "$out" | grep -q '现在这么说：对 ， 猫 -\[属于·传递\]-> 动物' \
    && chk "风格 1（结论+链）→ 结论仍是「对」" 1 || chk "风格 1 → 结论仍是「对」" 0
printf '%s\n' "$out" | grep -q '现在这么说：猫 -\[属于·传递\]' \
    && chk "风格 2（只给链）不给结论，但链一致" 1 || chk "风格 2 只给链" 0
printf '%s\n' "$out" | grep -q '现在这么说：对 ： 猫 属于 生物  ⇐' \
    && chk "风格 3（符号化）→ 结论仍是「对」" 1 || chk "风格 3 → 结论仍是「对」" 0

echo "⑤ 非逻辑路径也留来源，不许包装成推导结论"
out=$(run "你好呀" "::der")
printf '%s\n' "$out" | grep -q '无逻辑结论' \
    && chk "闲聊那轮标成「无逻辑结论（只有来源）」" 1 \
    || chk "闲聊那轮标成「无逻辑结论」" 0

echo
[ "$fail" -eq 0 ] && echo "PASS derivation（链结构化 / 验证器在验 / 缺口不断言 / 语言是投影）" \
                  || { echo "FAIL derivation"; exit 1; }
