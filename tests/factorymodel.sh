#!/bin/sh
# tests/factorymodel.sh — **出厂模型指纹**（§6.771）
#
# 为什么单有一份：对话行为套件（relations/tokenop/bucketload…）的断言是**按出厂图标定的**。
# 实测：往 `model.bin` 里只加 **2 条事实（+3 节点）**，relations 和 bucketload 当场变红 ——
# 不是 bug，是"图的内容本身就是行为的一部分"（节点数会改 idf/激活/选路）。
#
# 所以：`model.bin` 是**标定产物**，不许悄悄改。改了就要：
#   ① 更新下面的期望值；② 把行为套件全跑一遍、逐条确认变化是有意的；③ 在 CHANGELOG 记一笔。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/factorymodel"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

[ -s "$ROOT/model.bin" ] || { printf '  [FAIL] 缺 model.bin\n'; printf 'FAIL factorymodel\n'; exit 1; }
stats=$(printf '::stats\n::quit\n' | "$ROOT/build_aip" --model "$ROOT/model.bin" 2>/dev/null | grep -a '符号节点=' | head -1)
nodes=$(printf '%s' "$stats" | sed 's/.*符号节点=\([0-9]*\).*/\1/')
edges=$(printf '%s' "$stats" | sed 's/.*边=\([0-9]*\).*/\1/')
# 出厂指纹（改 model.bin 时必须同时更新这三行，并在 CHANGELOG 记）
EXP_NODES=1282
EXP_EDGES=41198
printf '  出厂模型：节点 %s / 边 %s（期望 %s / %s）\n' "$nodes" "$edges" "$EXP_NODES" "$EXP_EDGES"
[ "${nodes:-0}" -eq "$EXP_NODES" ] && ok "节点数一致" || bad "节点数变了（${nodes:-?}）⇒ 行为套件是按旧图标定的，改图要重跑并更新期望" ""
[ "${edges:-0}" -eq "$EXP_EDGES" ] && ok "边数一致" || bad "边数变了（${edges:-?}）⇒ 同上" ""

echo
printf '出厂指纹：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS factorymodel（model.bin 是标定产物：节点/边与期望一致）\n' || printf 'FAIL factorymodel\n'
[ "$fail" -eq 0 ]
