#!/bin/sh
# tests/conc_big.sh — **并发 + 大图**（阶段三 M4-4；§6.692/§6.693）
#
# 为什么单独一支：`conc_tsan.sh` 跑的是 `model.bin`（1 119 个符号节点 / 4 万边），
# 并发看护只在"小图"上验过。扩图之后要回答的是：**图大了以后并发还安不安全**
# （池子更大、共享表条目更多、realloc 更频繁）。
#
# 为什么用 **ASan 而不是 TSan**（实测结论）：
#   TSan 在 5 万词 / 20 万边的图上跑了 **20 多分钟没跑完**（每次内存访问都插桩，
#   而这张图会触发 emb 构建 = 5 万 × 1024 的循环）。TSan 留在 `conc_tsan.sh` 的**小图**上；
#   大图这一支用 **ASan**（它能抓 double free / UAF —— §6.693 抓到的那个就是这么来的），
#   另一条判断"没有数据竞争"的证据是**并行结果与串行逐字节相同**（见下）。
#
# 用法：sh tests/conc_big.sh [词数] [边数]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NW=${1:-10000}
NF=${2:-50000}
WORK="$ROOT/_t/conc_big"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -o mkcorpus "$ROOT/tools/mkcorpus.c" -lm || { echo "FAIL: mkcorpus 编不过"; exit 1; }
echo "== 造图（$NW 词 / $NF 边，幂律 + 长尾） =="
./mkcorpus big.bin "$NW" "$NF" zipf tail 7 2000 | head -2
[ -s big.bin ] || { echo "FAIL: 造图失败"; exit 1; }

echo "== ① 原生并发（并行 vs 串行逐字节相同 + 没被串行化） =="
$CC -std=c11 -O2 -Wall -Wextra -o conc_native "$ROOT/tools/concprobe.c" -lm -pthread 2>/dev/null \
    || { echo "FAIL: concprobe 编不过"; exit 1; }
if timeout 300 ./conc_native big.bin > native.log 2>&1; then
    grep -aE '逐字节相同|共享到同一个源|引用计数' native.log | sed 's/^/         /'
    ok "原生并发在大图上通过（$(( 0 )) 项断言见 native.log）"
else
    bad "原生并发在大图上失败/超时" "$(tail -3 native.log | tr '\n' ' ')"
fi

echo "== ② ASan 并发（double free / UAF 必须 0） =="
$CC -std=c11 -O1 -g -fsanitize=address -o conc_asan "$ROOT/tools/concprobe.c" -lm -pthread 2>/dev/null \
    || { echo "FAIL: ASan 版编不过"; exit 1; }
if ASAN_OPTIONS=detect_leaks=0 timeout 600 ./conc_asan big.bin > asan.log 2>&1; then
    if grep -qaE 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' asan.log; then
        bad "ASan 报了内存错误" "$(grep -aE 'ERROR: AddressSanitizer' asan.log | head -1)"
    else
        ok "ASan 干净（没有 double free / UAF），最后一行：$(tail -1 asan.log)"
    fi
else
    bad "ASan 版跑挂了" "$(grep -aE 'ERROR|SUMMARY' asan.log | head -2 | tr '\n' ' ')"
fi

echo
printf '并发+大图：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS conc_big（%s 词 / %s 边：并行==串行、ASan 干净）\n' "$NW" "$NF" || printf 'FAIL conc_big\n'
[ "$fail" -eq 0 ]
