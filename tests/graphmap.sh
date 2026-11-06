#!/bin/sh
# tests/graphmap.sh — **说法映射存进图里**（§6.764）：不靠 paraphrase.map，抽取器从图读
#
# 1) 教两句陈述：`同义 是 一种 关系` + `擅长 同义 会` ⇒ 图里多一条 `擅长 --[同义]--> 会`
# 2) 存盘；**删掉 paraphrase.map**；用这个模型去抽没见过的「擅长X」句子
# 3) 必须抽成规范关系（会 X）—— 证明映射真在图里，不在文件里
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/graphmap"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

cd "$ROOT"
rm -f paraphrase.map
cp model.bin "$W/m.bin"
printf '同义 是 一种 关系\n擅长 同义 会\n::save %s\n::quit\n' "$W/m2.bin" > "$W/t.in"
"$ROOT/build_aip" --model "$W/m.bin" < "$W/t.in" > "$W/t.out" 2>&1
if grep -qa '记下 了 ： 擅长 同义 会' "$W/t.out"; then ok "「擅长 同义 会」学进图"; else bad "映射没进图" "$(grep -a '记下' "$W/t.out" | tail -2 | tr '\n' ' ')"; fi
if [ -s "$W/m2.bin" ]; then ok "存盘成功"; else bad "存盘失败" ""; fi

printf '鲨鱼擅长潜水。\n老虎擅长奔跑。\n小狗擅长游泳。\n' > "$W/in2.txt"
printf '::paper read %s\n::quit\n' "$W/in2.txt" > "$W/in2.in"
"$ROOT/build_aip" --model "$W/m2.bin" < "$W/in2.in" > "$W/out2.txt" 2>&1
n=$(grep -acE '^  · .* 会 ' "$W/out2.txt" || true)
if [ "${n:-0}" -ge 3 ]; then ok "没有 paraphrase.map，3 句没见过的「擅长X」都抽成「会 X」"; else bad "只抽出 ${n:-0}/3" "$(grep -a '·' "$W/out2.txt" | head -3 | tr '\n' ' ')"; fi

echo
printf '图内映射：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS graphmap（说法映射存图里，不靠外部文件）\n' || printf 'FAIL graphmap\n'
[ "$fail" -eq 0 ]
