#!/bin/sh
# tests/cadence.sh — **周期性那几段真的会跑**（叙事/诊断/漂移/想象/内部对话）（§6.179）
#
# 为什么单有一份：局限总表 §一.11 曾写着「帧/检索接管之后，元认知/想象/叙事那一段**走不到**，
# 实测 叙事=0」。复核发现那是**周期没到**，不是路径被掐断：
#   `narrate_every=500` / 诊断每 300 轮 / `drift_sample_every=200`，
#   而当时只跑了 20 轮 —— 叙事当然是 0。
# 实测 520 轮（每轮都由帧/检索这类快路答复）之后：叙事=1、诊断=2、漂移=32、
# 想象=42、内部对话=520 —— 全部在跑。这一套把这件事钉住，免得再把"周期没到"读成"走不到"。
#
# 用法：sh tests/cadence.sh        （约 3 秒：520 轮 ≈ 2.2 s + 启动）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# 520 轮「你好呀」：每一轮都走快路答复（不是深推理），正好对应"周期段会不会被掐掉"这个问题
out=$(
    { printf '::load %s\n' "$MODEL"
      i=0
      while [ "$i" -lt 520 ]; do printf '你 好 呀\n'; i=$((i + 1)); done
      printf '::stats\n::quit\n'; } | "$BIN" 2>&1
)
line=$(printf '%s\n' "$out" | grep -a "想象=.*内部对话=.*叙事=" | tail -1)

num() { printf '%s\n' "$line" | sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }
narr=$(num 叙事); imag=$(num 想象); inner=$(num 内部对话)
diag=$(printf '%s\n' "$out" | grep -a "诊断=" | tail -1 | sed -n 's/.*诊断=\([0-9]*\).*/\1/p')
drift=$(printf '%s\n' "$out" | grep -a "诊断=" | tail -1 | sed -n 's/.*漂移=\([0-9]*\).*/\1/p')

[ -n "$narr" ] && [ -n "$imag" ] && [ -n "$inner" ] && [ -n "$diag" ] && [ -n "$drift" ] || {
    echo "FAIL cadence（没读到统计行：$line）"; exit 1; }

echo "① 周期性段（520 轮之后）"
[ "$narr" -ge 1 ] && chk "叙事 ≥ 1（实测 $narr，narrate_every=500）" 1 \
    || chk "叙事 ≥ 1（实测 $narr）" 0
[ "$diag" -ge 1 ] && chk "诊断 ≥ 1（实测 $diag，每 300 轮）" 1 \
    || chk "诊断 ≥ 1（实测 $diag）" 0
[ "$drift" -ge 1 ] && chk "漂移 ≥ 1（实测 $drift，每 200 轮采样）" 1 \
    || chk "漂移 ≥ 1（实测 $drift）" 0

echo "② 每轮/常开的两段"
[ "$inner" -ge 500 ] && chk "内部对话 ≈ 每轮都跑（实测 $inner/520）" 1 \
    || chk "内部对话 ≈ 每轮都跑（实测 $inner/520）" 0
[ "$imag" -ge 1 ] && chk "想象 ≥ 1（实测 $imag）" 1 \
    || chk "想象 ≥ 1（实测 $imag）" 0

if [ "$fail" -ne 0 ]; then
    echo "FAIL cadence（周期段没跑起来）"
    exit 1
fi
echo "PASS cadence（520 轮：叙事/诊断/漂移/想象/内部对话 都在跑 —— 之前记的「走不到」是周期没到）"
