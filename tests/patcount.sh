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
# §6.388 [shape] 的 rhs 不许指向"只有早期判据才认"的形状
# 背景：`[shape]` 改写发生在管线中后段，而量/尺度（`有多大/多重…`）、名字反查（`是谁`）、
# 保证请求（`你确定/真的吗`）都看**改写前**的输入 ⇒ 改写成果它们永远看不到，句子会掉到生成层。
# 实测踩过两次（§6.384 答出「重 要」、§6.386 改写无效），这条把"再写一次"挡住。
echo "③ [shape] 的 rhs 不许指向早期判据才认的形状"
n_bad=0
while IFS= read -r line; do
    case "$line" in \[shape\]*) ;; *) continue ;; esac
    rhs=${line#*=}
    rhs=$(printf '%s' "$rhs" | tr -d ' ')   # 数据里的改写是空格分隔的，比较前把空格去掉
    for m in 多大 多高 多重 多长 多远 多久 多快 多贵 多深 是谁 是啥 你确定 真的吗; do
        case "$rhs" in
        *"$m"*)
            echo "  FAIL rhs 里出现「$m」：那类判据看的是**改写前**的输入（§6.384/§6.388）→ $line"
            fail=1; n_bad=$((n_bad + 1)) ;;
        esac
    done
done < patterns.txt
[ "$n_bad" -eq 0 ] && echo "  OK 没有指向早期判据的改写"

[ "$fail" -eq 0 ] && echo "PASS patcount（$nb 个桶全部对上，且每个桶都有承重探针）"
exit "$fail"
