#!/bin/sh
# tests/candy.sh — **糖果推理**（约束/排除法）电池：`::solve`（§6.758）
#
# 模型：每人在该关系上恰好一个值、每个值至多一个人取（糖果题的标准前提）。
# 每题：给定事实（正/负），要求求解器给出**完整答案表**且与预期一致。
# 判据：答案表里每行都等于预期值；对"约束不够"的题必须**明说推不出来**（不许硬猜）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/candy"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

# 跑一题：$1=题名 $2=事实（多行，用 \n 分隔） $3=值域 $4=预期（"甲:值 乙:值 …"）
run_puzzle() {
    name=$1; facts=$2; vals=$3; expect=$4
    cp "$ROOT/model.bin" "$W/m.bin"
    {
        printf '%s\n' "$facts"
        printf '::solve 吃 %s\n::quit\n' "$vals"
    } > "$W/in.txt"
    out=$("$ROOT/build_aip" --model "$W/m.bin" < "$W/in.txt" 2>&1)
    miss=""
    for pair in $expect; do
        who=${pair%%:*}; val=${pair#*:}
        printf '%s' "$out" | grep -qa -- "$who：$val" || miss="$miss $who=$val"
    done
    if [ -z "$miss" ]; then ok "$name"; else bad "$name（缺:$miss）" "$(printf '%s' "$out" | sed -n '/答案表/,$p' | head -6 | tr '\n' ' ')"; fi
}

echo "== ① 3×3：一人已知 + 一人排除（唯一解）=="
run_puzzle "红/绿/蓝 各一颗" '红糖果 是 糖果
绿糖果 是 糖果
蓝糖果 是 糖果
小红 是 人
小明 是 人
小刚 是 人
小红 吃 红糖果
小明 不 吃 红糖果
小刚 不 吃 蓝糖果' "红糖果 绿糖果 蓝糖果 -- 小红 小明 小刚" "小红:红糖果 小明:蓝糖果 小刚:绿糖果"

echo "== ② 3×3：两条已知（第三个人直接定）=="
run_puzzle "两条已知" '红糖果 是 糖果
绿糖果 是 糖果
蓝糖果 是 糖果
小明 是 人
小红 是 人
小刚 是 人
小明 吃 红糖果
小红 吃 绿糖果' "红糖果 绿糖果 蓝糖果 -- 小明 小红 小刚" "小明:红糖果 小红:绿糖果 小刚:蓝糖果"

echo "== ③ 4×4：链式排除 =="
run_puzzle "四个人四颗糖" '红糖果 是 糖果
绿糖果 是 糖果
蓝糖果 是 糖果
黄糖果 是 糖果
小明 是 人
小红 是 人
小刚 是 人
小丽 是 人
小明 吃 红糖果
小红 不 吃 红糖果
小红 不 吃 绿糖果
小刚 不 吃 蓝糖果
小刚 不 吃 黄糖果
小丽 不 吃 红糖果
小丽 不 吃 蓝糖果' "红糖果 绿糖果 蓝糖果 黄糖果 -- 小明 小红 小刚 小丽" "小明:红糖果 小红:蓝糖果 小刚:绿糖果 小丽:黄糖果"

echo "== ④ 约束不够时必须明说推不出来（不许硬猜）=="
cp "$ROOT/model.bin" "$W/m.bin"
printf '方糖 是 糖果\n奶糖 是 糖果\n小明 是 人\n小红 是 人\n::solve 吃 方糖 奶糖 -- 小明 小红\n::quit\n' > "$W/in2.txt"
out=$("$ROOT/build_aip" --model "$W/m.bin" < "$W/in2.txt" 2>&1)
if printf '%s' "$out" | grep -qa '约束不够\|没有能被排除法逼出来'; then ok "约束不够 ⇒ 明说推不出来"; else bad "该说推不出来" "$(printf '%s' "$out" | sed -n '/\[求解\]/,$p' | head -3 | tr '\n' ' ')"; fi

echo
printf '糖果推理：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS candy（排除法：3×3/两条已知/4×4 链式/约束不够不硬猜）\n' || printf 'FAIL candy\n'
[ "$fail" -eq 0 ]
