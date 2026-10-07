#!/bin/sh
# tests/probes_build.sh — **所有探针都要能编译、能跑出结论**
#
# 为什么单独一道门（血泪）：探针是"把拍的变成量的"那件工具，可它自己**没有看护**——
# `tests/all.sh` 只编译 build_aip 和少数几个 test，`tools/` 里的探针谁都不编译。
# 于是出现过：`affectprobe.c` 改了 API 之后**编译失败**，可旧二进制还在，
# 跑出来的数字看着像真的（当时还把 gcc 的输出用 `head` 吞了，连错都没看见）。
#
# 这道门做两件事：
#   ① 每个 `tools/*.c` 都用 `-Wall -Wextra -Werror` 编译一遍（探针也不许有警告）；
#   ② 三个"重"探针（d4/wgate/affect/premhold）用**最小输入**各跑一次，断言退出码 0 且打出了结论行
#      —— 防的是"能编译但跑起来是空的/崩的"。
# 缺输入文件（模型/语料）时第 ② 步跳过（只编译），这样在干净检出上也能过。
#
# 26 个探针的编译彼此独立，是这一步最费时间的部分（串行实测 ~90 秒），
# 所以交给 tests/pool.sh 并行（调度只有那一份实现）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

MODEL=${MODEL:-model.bin}
CORPUS=${CORPUS:-_corpus.txt}
OUT=./_t/probes
# 产物**留着**（二进制 + .hash）：要不要重编由下面那张内容哈希决定，而不是每轮无条件重编。
# 只清上一轮的调度结果与日志（它们是每轮重建的）。
mkdir -p "$OUT"
rm -rf "$OUT/q"
rm -f "$OUT"/*.log

. "$ROOT/tests/pool.sh"
POOL_DIR=$OUT/q
POOL_QUIET=1
cpun=$(nproc 2>/dev/null || echo 4)
POOL_JOBS=${JOBS:-$cpun}
if [ "$POOL_JOBS" -gt 8 ]; then POOL_JOBS=8; fi
: > "$OUT/list.txt"
n=0
for f in tools/*.c; do
    name=$(basename "$f" .c)
    printf '%s|gcc -std=c11 -O2 -Wall -Wextra -Werror -o %s %s -lm\n' \
        "$name" "$OUT/$name" "$f" >> "$OUT/list.txt"
    n=$((n + 1))
done
if [ "$POOL_JOBS" -gt "$n" ]; then POOL_JOBS=$n; fi

# ---- 内容哈希缓存：探针的**输入**（所有 tools/*.c + 所有头文件）一字未变就复用上次产物 ----
# 安全边界（照实说）：哈希只覆盖"源码与头文件"。**换编译器/换 gcc 版本**它看不出来
# （那时新警告会漏一次）—— 想强制重编：删掉 $OUT/.hash 或整个 _t/probes。
# 为什么敢缓存：探针门第 ② 件（跑出结论行）**每次照跑**；缓存只跳过第 ① 件（编译），
# 而它跳过的前提是"输入逐字节没变"。
STAMP=$OUT/.hash
NEW=$(cat "$ROOT"/*.h "$ROOT"/*.c tools/*.c 2>/dev/null | md5sum | cut -d' ' -f1)
OLD=$(cat "$STAMP" 2>/dev/null || echo none)
have=1
for f in tools/*.c; do
    [ -x "$OUT/$(basename "$f" .c)" ] || have=0
done
compiled=0
if [ -n "$NEW" ] && [ "$NEW" = "$OLD" ] && [ "$have" = 1 ]; then
    echo "  编译通过：$n 个探针（复用上次编译：源码与头文件哈希未变 $NEW）"
else
    pool_run "$OUT/list.txt"
    printf '%s\n' "$NEW" > "$STAMP"
    compiled=1
    echo "  编译通过：$n 个探针（-Wall -Wextra -Werror，JOBS=$POOL_JOBS）"
fi

# 只在**这一轮真编译过**时核对退出码：缓存命中时没有 .rc（也没编译，没什么可核对的）。
bad=0
for rc in "$OUT"/q/*.rc; do
    [ -f "$rc" ] || continue
    read -r r m < "$rc" || continue
    if [ "$r" -ne 0 ]; then
        nm=${rc##*/}; nm=${nm%.rc}
        echo "  [FAIL] $nm 编译不过（-Werror：探针也不许有警告）"
        tail -5 "$OUT/q/$nm.log"
        bad=1
    fi
done
# 硬门：结果文件数必须等于探针数（少了＝有编译没留下退出码，那种"编译门"是假的）
nres=$(ls "$OUT"/q/*.rc 2>/dev/null | wc -l)
if [ "$compiled" = 1 ] && [ "$nres" -ne "$n" ]; then
    echo "  [FAIL] 结果文件数（$nres）≠ 探针数（$n）：有编译没留下退出码"
    bad=1
fi
[ "$bad" -eq 0 ] || exit 1

if [ ! -f "$MODEL" ]; then
    echo "  跳过运行冒烟：缺 $MODEL"
    exit 0
fi

run_probe() {   # run_probe <二进制> <日志名> <出错时该看到的模式> <参数...>
    bin=$1; log=$2; pat=$3; shift 3
    if ! "$OUT/$bin" "$@" > "$OUT/$log" 2>&1; then
        echo "  [FAIL] $bin 跑不起来（退出码非 0）"
        tail -3 "$OUT/$log"
        exit 1
    fi
    if ! grep -q "$pat" "$OUT/$log"; then
        echo "  [FAIL] $bin 没打出结论行（模式：$pat）"
        tail -3 "$OUT/$log"
        exit 1
    fi
    echo "  [OK]   $bin 跑得出结论"
}

run_probe d4probe     d4.log     "当前阈值" "$MODEL" "$CORPUS" 200
run_probe wgateprobe  wgate.log  "关系边"   "$MODEL"
run_probe affectprobe affect.log "耦合表命中" "$MODEL" "$CORPUS" 30
run_probe premholdprobe premhold.log "阈值扫描" "$MODEL"
echo "PROBES OK"
