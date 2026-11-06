#!/bin/sh
# tests/circumfix.sh — **框式说法**（以X为食/为主食/为生/为家）+ 归纳（§6.762）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/cf"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

echo "== ① 内置框式：以X为食/为主食/为家 =="
( cd "$ROOT" && cp model.bin "$W/m.bin" && rm -f circumfix.map
  printf '企鹅以鱼为食。\n熊猫以竹子为主食。\n企鹅以山洞为家。\n' > "$W/in.txt"
  "$ROOT/build_aip" --model "$W/m.bin" < /dev/null > /dev/null 2>&1
  printf '::paper read %s\n::quit\n' "$W/in.txt" | "$ROOT/build_aip" --model "$W/m.bin" > "$W/out.txt" 2>&1 )
for t in "企鹅 吃 鱼" "熊猫 吃 竹子" "企鹅 在 山洞"; do
    if grep -qa -- "$t" "$W/out.txt"; then ok "$t"; else bad "缺「$t」" "$(grep -a '·' "$W/out.txt" | head -2 | tr '\n' ' ')"; fi
done

echo "== ② 归纳出来的框式（为生 不在内置表里）=="
printf '蜜蜂以花蜜为生\t蜜蜂 吃 花蜜\n' > "$W/ex.tsv"
: > "$W/map.txt"; : > "$W/cf.txt"
sh "$ROOT/tools/induce_paraphrase.sh" "$W/ex.tsv" "$W/map.txt" "$W/cf.txt" > /dev/null 2>&1
if grep -qa '以	为生	吃' "$W/cf.txt"; then ok "归纳出「以…为生 → 吃」"; else bad "没归纳出框式" "$(cat "$W/cf.txt")"; fi
( cd "$ROOT" && cp "$W/cf.txt" "$ROOT/circumfix.map" && cp model.bin "$W/m2.bin"
  printf '老虎以野猪为生。\n鲨鱼以小鱼为生。\n' > "$W/in2.txt"
  printf '::paper read %s\n::quit\n' "$W/in2.txt" | "$ROOT/build_aip" --model "$W/m2.bin" > "$W/out2.txt" 2>&1
  rm -f "$ROOT/circumfix.map" )
n=$(grep -acE '^  · .* 吃 ' "$W/out2.txt" || true)
if [ "${n:-0}" -ge 2 ]; then ok "没见过的「以X为生」两句都抽成「吃 X」"; else bad "只抽出 ${n:-0}/2" "$(grep -a '·' "$W/out2.txt" | head -2 | tr '\n' ' ')"; fi

echo
printf '框式说法：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS circumfix（以X为食/为主食/为家 + 归纳出为生）\n' || printf 'FAIL circumfix\n'
[ "$fail" -eq 0 ]
