#!/bin/sh
# tests/train_model.sh — 从零复现 model.bin
#
# 步骤：
#   ① 生成对话语料（traindialog → dialog.txt）
#   ② 合并三份语料：seed_full（模板共现）+ generated（纯共现）+ dialog（一问一答）
#   ③ 批量训练（每 200 行内联巩固/睡眠回放），存 model.bin
#
# 用法：sh tests/train_model.sh [输出文件]   默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/model.bin}
cd "$ROOT"

CC=${CC:-gcc}
echo "[1/3] 编译工具"
$CC -std=c11 -O2 -o build_aip main.c -lm
$CC -std=c11 -O2 -o traindialog traindialog.c

echo "[2/3] 生成对话语料 + 合并"
./traindialog dialog.txt
cat seed_full.txt generated.txt dialog.txt > _corpus.txt
wc -l seed_full.txt generated.txt dialog.txt _corpus.txt

echo "[3/3] 批量训练 → $OUT"
./build_aip _corpus.txt --batch --interval 200 --save "$OUT" < /dev/null 2>&1 | grep -v '^你 > '
ls -la "$OUT"
printf '\n现在可以聊了：\n  ./build_aip            # 自动加载 model.bin\n'
printf '  ./build_aip --model %s\n' "$OUT"
