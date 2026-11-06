#!/bin/sh
# tests/induce.sh — **从例句归纳说法族**（§6.760）：看两条例子 ⇒ 会一族
#
# 流程：给 2 条例句（原句 + 规范事实）⇒ `tools/induce_paraphrase.sh` 归纳出 `擅长→会`
#      ⇒ 用它去跑**没出现过**的同类句（鲨鱼/老虎/小狗）⇒ 全都抽成规范关系。
# 另外验 `::law`：声明 unique 后 ::solve 会说"图里声明了唯一"，并解出糖果题。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/induce"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

echo "== ① 归纳：两条例子 ⇒ 一条映射 =="
printf '企鹅擅长游泳\t企鹅 会 游泳\n老虎擅长奔跑\t老虎 会 奔跑\n' > "$W/ex.tsv"
: > "$W/map.txt"
sh "$ROOT/tools/induce_paraphrase.sh" "$W/ex.tsv" "$W/map.txt" > "$W/ind.log" 2>&1
if grep -q "^擅长	会$" "$W/map.txt"; then ok "归纳出「擅长 → 会」"; else bad "没归纳出来" "$(cat "$W/ind.log" | tail -3)"; fi

echo "== ② 用归纳结果跑没教过的句子 =="
printf '鲨鱼擅长潜水。\n老虎擅长奔跑。\n小狗擅长游泳。\n' > "$W/unseen.txt"
( cd "$ROOT" && cp model.bin "$W/m.bin" && cp "$W/map.txt" "$ROOT/paraphrase.map" 2>/dev/null
  printf '::paper read %s\n::quit\n' "$W/unseen.txt" | "$ROOT/build_aip" --model "$W/m.bin" > "$W/out.txt" 2>&1 )
n=$(grep -acE '^  · .* 会 ' "$W/out.txt" || true)
if [ "${n:-0}" -ge 3 ]; then ok "3 句没见过的「擅长X」都抽成「会 X」"; else bad "只抽出 ${n:-0}/3" "$(grep -a '·' "$W/out.txt" | head -3 | tr '\n' ' ')"; fi

echo "== ③ 逻辑律 ::law unique ⇒ ::solve 从数据读律 =="
( cd "$ROOT" && cp model.bin "$W/m2.bin" && rm -f laws.txt
  printf '红糖果 是 糖果\n绿糖果 是 糖果\n蓝糖果 是 糖果\n小红 是 人\n小明 是 人\n小刚 是 人\n小红 吃 红糖果\n小明 不 吃 红糖果\n小刚 不 吃 蓝糖果\n::law unique 吃\n::solve 吃 红糖果 绿糖果 蓝糖果 -- 小红 小明 小刚\n::quit\n' > "$W/in.txt"
  "$ROOT/build_aip" --model "$W/m2.bin" < "$W/in.txt" > "$W/out2.txt" 2>&1
  rm -f laws.txt )
if grep -qa '图里声明了' "$W/out2.txt"; then ok "::solve 说「图里声明了唯一」（从 laws.txt 读）"; else bad "没有从数据读律" "$(grep -a '\[求解\]' "$W/out2.txt" | head -1)"; fi
if grep -qa '小刚：绿糖果' "$W/out2.txt" && grep -qa '小明：蓝糖果' "$W/out2.txt"; then ok "糖果题解出完整答案"; else bad "糖果题没解出" "$(sed -n '/答案表/,$p' "$W/out2.txt" | head -4 | tr '\n' ' ')"; fi

# 复原（测试改过仓库根的两个数据文件）
rm -f "$ROOT/paraphrase.map" "$ROOT/laws.txt"
echo
printf '归纳与律：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS induce（例句归纳说法族 + ::law 逻辑律进数据）\n' || printf 'FAIL induce\n'
[ "$fail" -eq 0 ]
