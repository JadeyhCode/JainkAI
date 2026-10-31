#!/bin/sh
# tests/e2e_paper.sh — **阶段二长链验收**（§6.644）：
#   读论文（离线）→ 抽断言 → 全部待证 → 用户确认一条 → 进阶到事实
#   → 教一条规则 → ::prove 证明（给每一步）→ 出处可查
# 只走仓库里已有的命令；不联网、不碰外部世界。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/e2e_paper"
rm -rf "$WORK"; mkdir -p "$WORK/papers"
cd "$WORK"
cp "$ROOT/model.bin" m.bin
cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" . 2>/dev/null || true
cat > papers/p.txt <<'PAPER'
企鹅 属于 鸟类。
麒麟 属于 神兽。
PAPER
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

run() {
    printf '::notepad off\n::char off\n%b\n::quit\n' "$1" | \
        timeout 120 "$ROOT/build_aip" --model m.bin > out.txt 2>&1 || true
}

echo "== ① 读论文：抽断言，全进待证（事实层还没有） =="
run '::paper read papers/p.txt\n::claim\n::save m.bin\n'
grep -qaE '抽到 [0-9]+ 条，记为待证 [0-9]+ 条' out.txt \
    && ok "::paper read 抽到断言并记为待证" \
    || bad "抽断言" "$(grep -a '论文' out.txt | head -1)"
grep -qaF '（来源：论文，可信度 50/100）' out.txt \
    && ok "待证来源/可信度都写清了" \
    || bad "来源" "$(grep -a '待证' out.txt | sed -n 2p)"

echo "== ② 用户确认一条 ⇒ 这时才是事实 =="
run '::claim accept 1\n企鹅 属于 鸟类 吗\n::save m.bin\n'
grep -qaF '按你说的算数' out.txt \
    && ok "::claim accept 升格了一条" \
    || bad "确认" "$(grep -a '待证' out.txt | head -1)"
grep -qaE '对 ， 企鹅 -\[属于\]-> 鸟类' out.txt \
    && ok "升格之后问答**真答对**（事实层里有这条）" \
    || bad "答对" "$(grep -a 'AI >' out.txt | tail -1)"

echo "== ③ 教规则 + 证明（给每一步） =="
run '::pred make 父 甲 乙\n::pred make 父 乙 丙\n::rule add 祖先($x,$y) :- 父($x,$y)\n::rule add 祖先($x,$z) :- 父($x,$y), 祖先($y,$z)\n::prove 祖先(甲,丙)\n'
grep -qaF '祖先(甲,丙) ⇒ **成立**' out.txt \
    && ok "::prove 祖先(甲,丙) ⇒ 成立（两跳）" \
    || bad "证明" "$(grep -a '证明' out.txt | head -1)"
grep -qaF '选取事实' out.txt && grep -qaF '规则 [规则:R' out.txt \
    && ok "证明里给出**每一步**（选取事实 + 规则）" \
    || bad "步骤" "$(grep -a -A2 '证明' out.txt | head -2 | tr '\n' ' ')"
grep -qaF '祖先(丙,甲) ⇒ **推不出来**' out.txt || true

echo "== ④ 出处：证明过的事实能查 =="
run '::src 甲 属于 乙\n'
if grep -qaE '来源|图里|推' out.txt; then
    ok "::src 能答出处（面板有输出）"
else
    bad "出处" "$(grep -a 'AI >' out.txt | head -1)"
fi

echo
printf '长链验收：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS e2e_paper（论文→待证→确认→规则→证明→出处）\n' || printf 'FAIL e2e_paper\n'
[ "$fail" -eq 0 ]
