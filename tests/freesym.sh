#!/bin/sh
# tests/freesym.sh — **自由图符号**（§6.770）：可命名、只互连+临时挂知识、可回收、可稀疏激活
#
# 判据四条：
#   ① 造/连/anchor 都成功，show 能看到
#   ② **不污染知识**：anchor 到 猫 之后，`::rel 猫` 里看不到这个自由符号
#   ③ 可回收：drop 之后 list 里没了
#   ④ 稀疏激活：activate 只把点名的那个放进焦点环（不动知识边）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/freesym"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
cd "$ROOT"
cp model.bin "$W/m.bin"

echo "== ① 造 / 互连 / 临时挂知识 =="
out=$(printf '::freesym new 草稿1\n::freesym new 草稿2\n::freesym link 草稿1 草稿2\n::freesym anchor 草稿2 猫\n::freesym show 草稿1\n::freesym show 草稿2\n::freesym list\n::quit\n' | "$ROOT/build_aip" --model "$W/m.bin" 2>&1)
printf '%s' "$out" | grep -qa '草稿1 互连 草稿2' && ok "自由符号互连（想象边）" || bad "互连失败" ""
printf '%s' "$out" | grep -qa '草稿2 临时挂到 猫' && ok "临时挂到知识（猫）" || bad "anchor 失败" ""
printf '%s' "$out" | grep -qa '共 2 个' && ok "list 列出 2 个" || bad "list 不对" "$(printf '%s' "$out" | grep -a '共' | tail -1)"

echo "== ② 不污染知识 =="
out2=$(printf '::freesym new 草稿9\n::freesym anchor 草稿9 猫\n::rel 猫\n::quit\n' | "$ROOT/build_aip" --model "$W/m.bin" 2>&1)
if printf '%s' "$out2" | grep -a '猫 --\[' | grep -qa '草稿9'; then
    bad "自由符号污染了 ::rel 猫" "$(printf '%s' "$out2" | grep -a '草稿9' | head -2 | tr '\n' ' ')"
else
    ok "::rel 猫 里没有自由符号（知识没被污染）"
fi

echo "== ③ 可回收 =="
out3=$(printf '::freesym new 草稿7\n::freesym drop 草稿7\n::freesym list\n::quit\n' | "$ROOT/build_aip" --model "$W/m.bin" 2>&1)
printf '%s' "$out3" | grep -qa '回收 草稿7' && ok "drop 回收" || bad "drop 失败" ""
printf '%s' "$out3" | grep -qa '共 0 个' && ok "回收后 list 为空" || bad "回收后还在" "$(printf '%s' "$out3" | grep -a '共' | tail -1)"

echo "== ④ 稀疏激活 =="
out4=$(printf '::freesym new 草稿5\n::freesym activate 草稿5\n::quit\n' | "$ROOT/build_aip" --model "$W/m.bin" 2>&1)
printf '%s' "$out4" | grep -qa '激活 草稿5' && ok "activate 成功（只进焦点环）" || bad "activate 失败" ""

echo
printf '自由符号：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS freesym（可命名/互连/临时挂知识/不污染/可回收/稀疏激活）\n' || printf 'FAIL freesym\n'
[ "$fail" -eq 0 ]
