#!/bin/sh
# tests/all.sh — 一把跑完全部回归（**并行：每套一个沙箱**）
#
# 为什么并行（用户要求省时间：整包串行一轮 5–7 分钟，一轮改动的收尾跑一次就够呛）：
#   清单里的套件分给 JOBS 个 worker，**每条任务有自己的沙箱目录**，用完即弃：
#     · 只读件：*.h *.c CMakeLists.txt _corpus.txt seed_full.txt → 软链回仓库（不占地方）
#     · 会被写的：model.bin / frames.txt / relations.txt          → 真副本
#     · tests/ tools/                                             → 真副本
#       （套件用 ROOT=$(dirname $0)/.. 定位仓库根：这两目录若用软链，
#         套件的 ROOT 会指回真仓库，`$ROOT/model.bin` 之类就不再是沙箱里的副本了。）
#   于是"某套把仓库根的 frames.txt 重训一遍"这类**仓库内串台**在并行下不会发生
#   （串行时 tests/frames.sh 真的会改写仓库根的 frames.txt —— 并行顺带把它关进沙箱）。
#
# 清单在 tests/suites.txt（**唯一一份**：加套件只改那里，别在这儿再列一遍）。
#
# 用法：sh tests/all.sh [语料文件]                  默认 _corpus.txt
#       JOBS=1 sh tests/all.sh                     串行（排查用；同一套沙箱机制）
#       JOBS=8 sh tests/all.sh                     指定并行度（默认 min(nproc,8)）
#       SUITES="features tokenop" sh tests/all.sh  只跑这几套（名字＝清单第一列）
#
# 产物：_t/par/<名字>.log（该套全部输出）、_t/par/<名字>.rc（退出码 + 毫秒）
#
# 套件要的 `aip` 从哪来：第 0 步刚编好 build_aip，把它当**预置件**发给套件
# （AIP_PREBUILT + 源码内容哈希；套件那头只有 tests/prebuilt.sh 一份实现）。
# 原先 9 个套件各自再编一遍同一个 main.c（各 ~16 s）—— 这是套件相位的一半。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
CORPUS=${1:-_corpus.txt}
export CORPUS
LIST=$ROOT/tests/suites.txt
PAR=$ROOT/_t/par

# 锁：同一份 _t/par 不能给两个 all.sh 同时用 —— 调度靠 todo/ + mv 领取，
# 两个实例会互相抢任务、互相 rm -rf 对方的沙箱（本轮真踩过：一个 SUITES=frames 的跑法
# 被紧接着启动的整包挤掉，两边都用 _t/par/w1）。撞上就**立刻**退出，别浪费一次整包。
mkdir -p "$PAR"
if ! mkdir "$PAR/.lock" 2>/dev/null; then
    echo "另有一个 tests/all.sh 在跑（$PAR/.lock 还在）：等它跑完再启动。"
    echo "确认没在跑：rm -rf \"$PAR/.lock\" 之后重试（或整个 rm -rf _t/par）。"
    exit 3
fi
trap 'rm -rf "$PAR/.lock"' EXIT INT TERM

# —— 先把任务清单组好（SUITES 写错了要立刻报错，别让人白等第 0 步的编译+探针）——
mkdir -p "$PAR"
TASKS=$PAR/tasks.txt
grep -v '^[[:space:]]*#' "$LIST" | grep -v '^[[:space:]]*$' > "$PAR/all.txt"
if [ -n "${SUITES:-}" ]; then
    : > "$TASKS"
    for s in $SUITES; do
        if grep -q "^$s|" "$PAR/all.txt"; then
            grep "^$s|" "$PAR/all.txt" >> "$TASKS"
        else
            echo "清单里没有这套：$s（见 tests/suites.txt）"
            exit 2
        fi
    done
else
    cp "$PAR/all.txt" "$TASKS"
fi
total=$(grep -c . "$TASKS")

cpun=$(nproc 2>/dev/null || echo 4)
JOBS=${JOBS:-$cpun}
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi
if [ "$JOBS" -gt "$total" ]; then JOBS=$total; fi
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi

echo "=== 0 编译（严格零警告 + 所有探针）==="
# 这两件事**互不依赖**（探针不碰 build_aip，build_aip 也不碰探针）→ 并行跑。
# 量过：`build_aip` 单编 ~16 s、26 个探针并行编译 ~11 s，串着跑就是白等 11 s。
# 探针那边限 4 路（`JOBS=4`）：主程序那条 -O2 编译本身要吃内存，两边都满速容易换页。
mkdir -p "$PAR"
JOBS=4 sh tests/probes_build.sh > "$PAR/probes.log" 2>&1 &
probes_pid=$!
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_aip main.c -lm
echo "BUILD OK"
# 预置件的"新鲜度"凭据：源码 + 全部头文件的内容哈希（口径同 tests/probes_build.sh）。
# 套件那头对不上就自己编（防"拿旧二进制量出新结论"）。
AIP_PREBUILT_HASH=$(cat "$ROOT"/*.h "$ROOT"/*.c | md5sum | cut -d' ' -f1)
export AIP_PREBUILT_HASH
# 探针也要编译得过、跑得出结论 —— 它们是"把拍的变成量的"那件工具，自己不能没看护。
# （血泪：driveprobe 改 API 后编译失败，旧二进制照跑，量出来的数看着像真的。）
if ! wait "$probes_pid"; then
    echo "  ^^^ 探针门失败："
    cat "$PAR/probes.log"
    exit 1
fi
cat "$PAR/probes.log"

# 调度只用一份实现（tests/pool.sh）；这里的两个钩子负责"每条任务一个干净沙箱"。
. "$ROOT/tests/pool.sh"
POOL_DIR=$PAR
POOL_JOBS=$JOBS

pool_task_setup() {   # $1=套件名（没用上） $2=worker 号
    SB=$PAR/w$2
    rm -rf "$SB"; mkdir -p "$SB"
    for f in "$ROOT"/*.h "$ROOT"/*.c "$ROOT"/CMakeLists.txt \
             "$ROOT"/_corpus.txt "$ROOT"/seed_full.txt; do
        if [ -e "$f" ]; then ln -s "$f" "$SB/"; fi
    done
    cp -a "$ROOT/tests" "$SB/tests"
    cp -a "$ROOT/tools" "$SB/tools"
    cp "$ROOT/model.bin" "$SB/model.bin"
    cp "$ROOT/build_aip" "$SB/build_aip"
    # 套件里 `sh "$ROOT/tests/prebuilt.sh" aip …` 复制的就是这一份（不再各编 16 s）
    AIP_PREBUILT="$SB/build_aip"; export AIP_PREBUILT
    for f in frames.txt relations.txt; do
        if [ -f "$ROOT/$f" ]; then cp "$ROOT/$f" "$SB/$f"; fi
    done
    cd "$SB"
}
pool_task_cleanup() { rm -rf "$PAR/w$2"; }

echo "=== 跑 $total 套（JOBS=$JOBS，每条一个沙箱）==="
start=$(date +%s%N)
pool_run "$TASKS"
end=$(date +%s%N)

pass=0; fail=0; failed=""
for f in "$PAR"/*.rc; do
    [ -f "$f" ] || continue
    name=${f##*/}; name=${name%.rc}
    read -r rc ms < "$f" || continue
    if [ "$rc" -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $name"; fi
done
# 硬门：结果文件数必须等于任务数。少了就是"有任务没留下退出码"（worker 被杀、
# 沙箱建不起来之类）—— 那种情况汇总看着是绿的，其实有套件根本没跑。宁可变红。
nres=$(ls "$PAR"/*.rc 2>/dev/null | wc -l)
if [ "$nres" -ne "$total" ]; then
    echo "  ^^^ 结果文件数（$nres）≠ 任务数（$total）：有套件没留下退出码，汇总不可信"
    fail=1
fi
for name in $failed; do
    echo
    echo "--- $name 失败（最后 20 行；全文 _t/par/$name.log）---"
    tail -20 "$PAR/$name.log"
done
echo
echo "=== 套件结果：$total 套，通过 $pass，失败 $fail，用时 $(( (end-start)/1000000000 ))s（JOBS=$JOBS）==="

echo "=== 收官：交互冒烟（读盘后各派生层是否都在 + 混合模式 + 开关）==="
# 硬断言：读盘之后**派生层**必须都在。这几层不进模型文件、靠加载器现读/现算，
# 曾经因为一次字符串替换把 ai_embed_build + frames_load 删掉，
# 症状只是「帧库空了」→ 帧层与思维链步骤静默消失，而 roundtrip 照样 PASS
# （它只比状态转储，不看这些层）。所以这里补一道硬门。
smoke=$(printf '::load model.bin\n::frames\n::rel 猫\n::quit\n' | ./build_aip 2>&1)
sfail=0
printf '%s\n' "$smoke" | grep -q '没有帧库' && { echo "  ^^^ 加载后帧库是空的（frames_load 没被调用？）"; sfail=1; }
printf '%s\n' "$smoke" | grep -q -- '--\[属于\]-->' || { echo "  ^^^ 加载后关系知识是空的（relations_load 没被调用？）"; sfail=1; }
[ "$sfail" -eq 0 ] || fail=1

printf '::load model.bin\n你好呀\n::free\n::framegate\n::coin\n::seg\n::relay\n::fall\n::near 猫\n::rel 猫\n::der\n::hypo\n::world\n::walk\n你想学画画吗\n::quit\n' \
    | ./build_aip 2>&1 | grep -E 'AI >|内部联想|帧读关联链|现场造词|换切分|中转词|降级答复|联想直接输出' \
    || { echo "FAIL 交互冒烟"; fail=1; }

echo
[ "$fail" -eq 0 ] && echo "全部回归通过" || { echo "有回归失败"; exit 1; }
