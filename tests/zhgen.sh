#!/bin/sh
# tests/zhgen.sh — **中文资料 → 知识**的泛化（说法归一，§6.757）
#
# 同一批事实用**不同说法**写 16 句（是…的一种 / 可以 / 生活在 / 害怕 / 喜欢吃），
# 要求抽出来的关系词**全部归一**成图里已有的规范关系（属于/会/在/吃/怕/有/需要），
# 并且不许出现「是 / 可以 / 生活在 / 害怕」这种**没归一**的残渣。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/zhgen"; rm -rf "$W"; mkdir -p "$W"
cp "$ROOT/tests/fixtures/zh_para.txt" "$W/zh.txt"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
cp "$ROOT/model.bin" "$W/m.bin"
out=$(printf '::paper read %s\n::claim list\n::quit\n' "$W/zh.txt" | "$ROOT/build_aip" --model "$W/m.bin" 2>&1)
printf '%s\n' "$out" | grep -a '抽到' | head -2

echo "== ① 归一后的规范三元组都在 =="
for t in "企鹅 属于 鸟类" "企鹅 会 游泳" "企鹅 在 南极" "企鹅 吃 鱼" "企鹅 怕 北极熊" \
         "企鹅 有 翅膀" "企鹅 需要 空气" "鲸鱼 属于 哺乳动物" "鲸鱼 会 潜水" "鲸鱼 在 海洋" \
         "鲸鱼 吃 磷虾" "鲸鱼 怕 虎鲸" "鲸鱼 有 肺" "鲸鱼 需要 呼吸"; do
    if printf '%s' "$out" | grep -qa -- "$t"; then ok "$t"; else bad "缺「$t」" "$(printf '%s' "$out" | grep -a '·' | head -2 | tr '\n' ' ')"; fi
done

echo "== ② 不许有没归一的残渣（是/可以/生活在/害怕）=="
for w in " 是 " " 可以 " "生活在" " 害怕 " "拥有"; do
    if printf '%s' "$out" | grep -qa -- "· .*$w"; then bad "残渣「$w」还在" "$(printf '%s' "$out" | grep -a -- "$w" | head -1)"; else ok "没有残渣「$w」"; fi
done

echo "== ③ 全部进待证（不直接算数）=="
if printf '%s' "$out" | grep -qa '都不算数'; then ok "抽到的都进待证"; else bad "有东西直接算数了" ""; fi

echo
printf '中文泛化：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS zhgen（中文资料 → 规范关系：说法归一 + 待证）\n' || printf 'FAIL zhgen\n'
[ "$fail" -eq 0 ]
