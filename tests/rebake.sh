#!/bin/sh
# tests/rebake.sh — **行为基线重标定**的看护（§6.774）
#
# 两件事：
#   ① 出厂基线（tests/baselines/factory.txt）必须与当前 model.bin 的 13 条探针答案一致
#      —— 谁改了图，这里立刻红（和 factorymodel.sh 的节点/边指纹互补：一个看结构，一个看行为）；
#   ② 帧模型（model_frames.bin，若有）与出厂基线在**核心探针**上一致
#      —— 证明"帧进图"没有改变日常对话行为。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/rebake"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
cd "$ROOT"

echo "== ① 出厂基线 ==="
if sh tools/rebake.sh model.bin factory --check > "$W/c1.log" 2>&1; then ok "model.bin 与出厂基线一致（13 条探针）"; else bad "出厂基线漂了" "$(tail -6 "$W/c1.log" | tr '\n' ' ')"; fi

echo "== ② 帧模型与出厂基线一致（有 model_frames.bin 才跑）=="
if [ -s model_frames.bin ]; then
    if sh tools/rebake.sh model_frames.bin frames --check > "$W/c2.log" 2>&1; then ok "model_frames.bin 与自己的基线一致"; else bad "帧模型基线漂了" "$(tail -4 "$W/c2.log" | tr '\n' ' ')"; fi
    if diff -q tests/baselines/factory.txt tests/baselines/frames.txt > /dev/null 2>&1; then ok "帧模型与出厂在 13 条核心探针上**逐条相同**"; else bad "帧模型改变了核心探针" "$(diff tests/baselines/factory.txt tests/baselines/frames.txt 2>/dev/null | head -6 | tr '\n' ' ')"; fi
else
    printf '  [SKIP] 没有 model_frames.bin（sh tools/build_frames_model.sh 可生成）\n'
fi

echo
printf '重标定基线：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS rebake（出厂与帧模型的行为基线都钉住了）\n' || printf 'FAIL rebake\n'
[ "$fail" -eq 0 ]
