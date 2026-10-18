#!/bin/sh
# tests/patcount.sh — **数据桶计数看护**（§6.352）
#
# 五次踩过"桶满了静默丢弃"（shape 8/32/64、reply 64…）。这条把"文件里有几行"
# 和"加载后有几条"钉在一起：任何一个桶对不上就红。
# 注意：`[shape]` 会**自动生成分词变体**，所以它的加载数 ≥ 文件行数，只查"不小于"。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f patterns.txt ] || { echo "FAIL: 缺 patterns.txt"; exit 1; }

got=$(AIP_PATDBG=1 timeout 30 "$BIN" --model model.bin </dev/null 2>&1 \
      | sed -n 's/.*\[patterns\] \(lead=.*\)/\1/p' | head -1)
[ -n "$got" ] || { echo "FAIL: AIP_PATDBG 没输出"; exit 1; }
fail=0
chk() {  # $1=桶名 $2=加载数 $3=文件行数
    if [ "$1" = "shape" ]; then
        [ "$2" -ge "$3" ] || { echo "  FAIL shape 加载 $2 < 文件 $3（被上限丢弃）"; fail=1; }
    else
        [ "$2" -eq "$3" ] || { echo "  FAIL $1 加载 $2 ≠ 文件 $3（被上限丢弃或解析失败）"; fail=1; }
    fi
}
for k in lead slot confuse reply shape syn affix tail; do
    n=$(printf '%s\n' "$got" | sed -n "s/.*\b$k=\([0-9]*\).*/\1/p" | head -1)
    f=$(grep -c "^\[$k\]" patterns.txt || true)
    chk "$k" "${n:-0}" "$f"
done
[ "$fail" -eq 0 ] && echo "PASS patcount（各桶加载数与文件行数一致；shape 含自动变体）"
exit "$fail"
