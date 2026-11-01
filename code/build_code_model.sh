#!/bin/sh
# code/build_code_model.sh — **代码知识模型**（阶段四 M1；§6.699）
#
# 把「代码骨架知识」+「C API 知识」批量灌进出厂模型，产出 `code/code_model.bin`：
#   · `code/knowledge/skeletons.triples` —— 手写的骨架（任务描述 → 骨架 → 可编译的 C），**这是合成器的检索源**
#   · `code/knowledge/capi.triples`      —— 从 /usr/include 抽的真实 C API（函数原型/宏/类型/结构名）
#
# 为什么不直接改 `model.bin`：出厂模型是所有回归的基线（A/B、probematrix、几十套看护都拿它比）。
# 代码知识是**另一张图**，单独生成、单独量；要不要合进去是产品决定，不是这一轮能顺手做的。
#
# 用法：sh code/build_code_model.sh [--rebuild-api]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=code/code_model.bin
[ "${1:-}" = "--rebuild-api" ] && sh tools/headers2triples.sh > /dev/null

[ -s code/knowledge/skeletons.triples ] || { echo "FAIL: 缺 skeletons.triples"; exit 1; }
[ -s code/knowledge/capi.triples ] || { echo "FAIL: 缺 capi.triples（先跑 tools/headers2triples.sh）"; exit 1; }
[ -s code/knowledge/compose.triples ] || { echo "FAIL: 缺 compose.triples（组合知识）"; exit 1; }

mkdir -p _t
cp model.bin "$OUT.work"
# 两个文件按顺序灌（骨架在前：它是合成器最先要查的）
cp "$OUT.work" "$OUT.tmp.bin"
printf '::ingest code/knowledge/skeletons.triples\n::ingest code/knowledge/compose.triples\n::ingest code/knowledge/capi.triples\n::save %s\n::quit\n' "$OUT" > _t/codebuild.in
/usr/bin/time -f '  灌图+存盘墙钟 %e s（峰值 RSS %M KB）' ./build_aip --model "$OUT.tmp.bin" < _t/codebuild.in 2>&1 | grep -aE '导入|现在：|已保存|墙钟'
rm -f "$OUT.tmp.bin"

echo "== 生成的模型 =="
ls -l "$OUT" | awk '{printf "  %s：%s 字节\n", $9, $5}'
printf '::stats\n::quit\n' | ./build_aip --model "$OUT" 2>&1 | grep -a '符号节点' | head -1 | sed 's/^/  /'
echo "== 载入 / 查询 / 存盘 =="
printf '::quit\n' | /usr/bin/time -f '  载入（惰性）：%e s，RSS %M KB' ./build_aip --model "$OUT" 2>&1 >/dev/null | tail -1
printf '::rel 函数:printf\n::rel 任务:素数\n::quit\n' | ./build_aip --model "$OUT" 2>&1 | grep -aE '函数:printf|任务:素数|骨架:' | head -6 | sed 's/^/  /'
printf '::save /tmp/code_model_rt.bin\n::quit\n' | /usr/bin/time -f '  存盘：%e s' ./build_aip --model "$OUT" 2>&1 >/dev/null | tail -1
rm -f /tmp/code_model_rt.bin "$OUT.work"
