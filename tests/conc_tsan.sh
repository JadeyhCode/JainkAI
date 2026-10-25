#!/bin/sh
# tests/conc_tsan.sh — 并发看护（TSan）：**数据竞争必须是 0**
#
# 为什么单列一套（而不是塞进 `conc` 那套）：
#   TSan 把每次内存访问都插桩，模型要加载 5 遍（热场 + 4 个脑）⇒ 约 **50 秒**。
#   普通 `conc` 套件 1.5 秒就能验"并行结果 == 串行结果"；真正抓竞争的还是这一套。
#
# 已知环境坑（实测）：内核 ASLR 随机化会让 TSan 起不来 ——
#   `FATAL: ThreadSanitizer: unexpected memory mapping`
#   ⇒ 用 `setarch -R` 关掉 ASLR 再跑。没有 setarch、或者还是起不来，就显式 **SKIP**
#      （不装作通过：SKIP 会打印原因）。
#
# 用法：sh tests/conc_tsan.sh [模型文件]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK=${WORK:-$ROOT/_t/conc_tsan}
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }

gcc -std=c11 -O1 -g -fsanitize=thread -o conc_tsan "$ROOT/tools/concprobe.c" -lm -pthread 2> build.log
if [ ! -x conc_tsan ]; then
    echo "FAIL: TSan 版本编不出来"; tail -5 build.log; exit 1
fi

PREFIX=""
if command -v setarch >/dev/null 2>&1; then
    PREFIX="setarch $(uname -m) -R"
fi
# halt_on_error=0：一次跑完把所有竞争都记下来（默认第一条就退出，看不到全貌）
$PREFIX env TSAN_OPTIONS="halt_on_error=0" ./conc_tsan "$MODEL" > run.log 2>&1 || true

if grep -q "unexpected memory mapping" run.log; then
    echo "SKIP conc_tsan（这台机器的 ASLR 让 TSan 起不来：unexpected memory mapping）"
    echo "     想真跑：sudo sysctl -w vm.mmap_rnd_bits=28 或者用 setarch -R（本脚本已尝试）"
    exit 0
fi
if grep -q "ThreadSanitizer" run.log && grep -q "WARNING: ThreadSanitizer" run.log; then
    echo "FAIL: 有数据竞争（$(grep -c 'WARNING: ThreadSanitizer' run.log) 处）"
    grep -E "Location is|SUMMARY: ThreadSanitizer" run.log | head -12
    exit 1
fi
# 探针本身没问题（9 项全过）才算过 —— 只看"没有 WARNING"不够，可能它压根没跑起来
if ! grep -q "通过 .* / 失败 0" run.log; then
    echo "FAIL: 探针没跑完（见 $WORK/run.log）"; tail -15 run.log; exit 1
fi
tail -2 run.log
echo "PASS conc_tsan（0 处数据竞争；并行结果与串行逐字节一致）"
