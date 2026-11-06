#!/bin/sh
# tests/graphsink.sh — **三种语言件都在图里**（§6.765–767）
#
# ① 框式模式：`以为业 --[同义]--> 吃` ⇒ 抽取器从图读（无 circumfix.map 也能抽「以X为业」）
# ② 逻辑律：  `吃 --[有]--> 唯一性`   ⇒ ::solve 从图读律并解出糖果题（无 laws.txt）
# ③ 帧：      `帧库 --[帧]--> 帧:…`    ⇒ frames.txt 生成的帧知识能查（::rel 帧库）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/graphsink"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
cd "$ROOT"

echo "== ① 框式模式进图 =="
printf '以为业\t同义\t吃\n' > "$W/cf.triples"
cp model.bin "$W/m1.bin"
printf '::ingest %s\n::save %s\n::quit\n' "$W/cf.triples" "$W/m1b.bin" | "$ROOT/build_aip" --model "$W/m1.bin" > /dev/null 2>&1
rm -f circumfix.map
printf '蜜蜂以采蜜为业。\n' > "$W/cf.txt"
printf '::paper read %s\n::quit\n' "$W/cf.txt" | "$ROOT/build_aip" --model "$W/m1b.bin" > "$W/cf.out" 2>&1
if grep -qa '蜜蜂 吃 采蜜' "$W/cf.out"; then ok "「以X为业」从图读模式，抽成「吃 X」"; else bad "框式模式没起作用" "$(grep -a '·' "$W/cf.out" | head -2 | tr '\n' ' ')"; fi

echo "== ② 逻辑律进图 =="
printf '吃\t有\t唯一性\n' > "$W/law.triples"
cp model.bin "$W/m2.bin"
printf '::ingest %s\n::save %s\n::quit\n' "$W/law.triples" "$W/m2b.bin" | "$ROOT/build_aip" --model "$W/m2.bin" > /dev/null 2>&1
rm -f laws.txt
{ printf '红糖果 是 糖果\n绿糖果 是 糖果\n蓝糖果 是 糖果\n小红 是 人\n小明 是 人\n小刚 是 人\n'
  printf '小红 吃 红糖果\n小明 不 吃 红糖果\n小刚 不 吃 蓝糖果\n'
  printf '::solve 吃 红糖果 绿糖果 蓝糖果 -- 小红 小明 小刚\n::quit\n'; } > "$W/law.in"
"$ROOT/build_aip" --model "$W/m2b.bin" < "$W/law.in" > "$W/law.out" 2>&1
if grep -qa '已声明「唯一」' "$W/law.out" && grep -qa '小刚：绿糖果' "$W/law.out"; then ok "律从图读（吃 有 唯一性），糖果题解出"; else bad "律没从图读到" "$(grep -a '\[求解\]' "$W/law.out" | head -2 | tr '\n' ' ')"; fi

echo "== ③ 帧知识进图 =="
sh "$ROOT/tools/frames2triples.sh" > "$W/fr.triples" 2>/dev/null
n_tr=$(grep -c . "$W/fr.triples" || true)
cp model.bin "$W/m3.bin"
printf '::ingest %s\n::save %s\n::quit\n' "$W/fr.triples" "$W/m3b.bin" | "$ROOT/build_aip" --model "$W/m3.bin" > /dev/null 2>&1
n_fr=$(printf '::rel 帧库\n::quit\n' | "$ROOT/build_aip" --model "$W/m3b.bin" 2>&1 | grep -ac -- '--\[帧\]-->' || true)
if [ "${n_tr:-0}" -gt 0 ] && [ "${n_fr:-0}" -ge 10 ]; then ok "帧知识 $n_tr 条进图，::rel 帧库 列出 $n_fr 个帧"; else bad "帧没进图（生成 $n_tr / 查到 $n_fr）" ""; fi

echo
printf '下沉验收：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS graphsink（框式模式 / 逻辑律 / 帧 都能存在图里）\n' || printf 'FAIL graphsink\n'
[ "$fail" -eq 0 ]
