#!/bin/sh
# tests/patcount.sh — **数据桶计数看护**（§6.352；§6.379 起改成"从数据里发现桶"）
#
# 五次踩过"桶满了静默丢弃"（shape 8/32/64、reply 64…）。这条把"文件里有几行"
# 和"加载后有几条"钉在一起：任何一个桶对不上就红。
#
# §6.379 两处修正：
#   ① 桶名**不再写死在脚本里**（以前漏了 `fill`：数据里有这个桶、脚本不查它）；
#      现在从 `patterns.txt` 里把出现的桶全捞出来，逐个比。
#   ② 每个出现在数据里的桶，`tests/bucketload.sh` 里**必须有一条探针**（否则新桶没人看护）。
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

echo "① 每个出现在数据里的桶：加载数 == 文件行数（shape 只查不小于）"
buckets=$(grep -oE '^\[[a-z]+\]' patterns.txt | tr -d '[]' | sort -u)
[ -n "$buckets" ] || { echo "FAIL: patterns.txt 里没读到任何桶"; exit 1; }
for k in $buckets; do
    n=$(printf '%s\n' "$got" | sed -n "s/.*\b$k=\([0-9]*\).*/\1/p" | head -1)
    if [ -z "$n" ]; then
        echo "  FAIL 桶 [$k] 在数据里有行，但 AIP_PATDBG 不报它（加载器不认识这个桶？）"
        fail=1
        continue
    fi
    f=$(grep -c "^\[$k\]" patterns.txt || true)
    chk "$k" "$n" "$f"
done

echo "② 每个桶在 bucketload.sh 里都要有探针（新桶必须补一行）"
for k in $buckets; do
    if grep -qE "^${k}\|" tests/bucketload.sh 2>/dev/null; then :; else
        echo "  FAIL 桶 [$k] 没有探针：往 tests/bucketload.sh 的表里补一行「${k}|<探针输入>」"
        fail=1
    fi
done

nb=$(printf '%s\n' "$buckets" | grep -c . || true)
[ "$fail" -eq 0 ] && echo "PASS patcount（$nb 个桶全部对上，且每个桶都有承重探针）"
exit "$fail"
