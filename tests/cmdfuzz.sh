#!/bin/sh
# tests/cmdfuzz.sh — **命令面随机会话**（阶段②，§6.600）
#
# 为什么要有它：`bughunt` 打的是"引擎内部的随机操作"，走的是 `agent_step` 那条路；
# 而 **CLI 命令面**（`::brain` 槽位管理 / `::op` / `::parts` / `::save`+`::load` /
# `::teach` / `::learn` / `::src` …）只有"点测"（cmdtable 逐条打一遍、tokenop 各点一次）。
# 这些命令**组合起来**会不会把状态搞坏（切脑/存读/局部加载/多脑存盘），没人随机打过。
#
# 做法：固定种子的 LCG 生成一段命令脚本（含陈述句），整段喂给一个进程，跑完再 `::revcheck`。
# 判据（任一不成立就红）：
#   ① 进程退出码 0（没崩）；
#   ② 输出里没有崩溃/断言字样；
#   ③ 没有"命令表里有 X 但没有实现"（那是命令表与实现的漂移，§6.576 的老 bug 形态）；
#   ④ 最后那条 `::revcheck` 报"反查表缺 0 条"（反复切脑/存读之后反查表仍然自洽）；
#   ⑤ 输出非空（真跑起来了）。
#
# 用法： sh tests/cmdfuzz.sh [种子数] [每段步数]
#   环境： AIP=<可执行> MODEL=<模型> （默认 ./build_aip / model.bin）
set -u
# 和其它 CLI 套件同一个套路：自己算 ROOT/WORK、用 tests/prebuilt.sh 拿 aip、把模型与数据拷进来
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${MODEL:-$ROOT/model.bin}
SEEDS=${1:-6}
STEPS=${2:-40}
WORK="$ROOT/_t/cmdfuzz"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" "$WORK/model.base"   # 名字不带 .bin：每轮清理 `*.bin` 时不会把它删掉
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$WORK/"; done
AIP="$WORK/aip"
MODEL="$WORK/model.base"
pass=0; fail=0
W="$WORK"
cd "$W"
i=1
while [ "$i" -le "$SEEDS" ]; do
    rm -f "$W"/*.bin "$W"/*.parts 2>/dev/null
    cp "$MODEL" "$W/m.bin"
    # 命令池：**从 `::help` 自动抽**（新增命令不会再漏）+ 一批"带参数"的手工形态。
    # 黑名单：会结束会话 / 训练语料 / 碰外部世界的几条，随机会话里不该出现。
    POOL="$W/pool.txt"
    if [ ! -f "$POOL" ]; then
        "$AIP" --model m.bin 2>/dev/null <<'HELPEOF' | sed -n 's/^  ::\([A-Za-z0-9_-]*\).*/::\1/p' \
            | grep -vxE '::(quit|train|batch|act|gen)' > "$POOL"
::help
::quit
HELPEOF
        cat >> "$POOL" <<'CURATED'
::brain new
::brain list
::brain use 1
::brain save 1 b1.bin
::brain drop 1
::brain fork
::src 猫 属于 动物
::rel 猫
::rel 麒麟
::askconflict off
::askconflict on
::natcoin on
::natcoin off
::locative on
::revise off
::revise on
::op 加前提 猫 动物
::op 回收临时
::op 中间插词 鸟 飞
::op 建过程 清洗
::teach 麒麟 属于 神兽
::teach 月亮 喵 鱼
::save m.bin
::load m.bin
猫 属于 动物 吗
麒麟 属于 神兽 吗
::parts save p.parts
::parts attach p.parts
::parts detach
::lex int main(void){return 0;}
::revcheck
CURATED
        wc -l < "$POOL" | sed 's/^/  [池] 命令形态 /;s/$/ 条/'
    fi
    awk -v s="$i" -v n="$STEPS" -v pool="$POOL" '
    BEGIN{
        srand(s);
        k = 0;
        while ((getline line < pool) > 0) if (length(line) > 0) c[k++] = line;
        if (k == 0) { print "::revcheck"; exit 1; }
        # 前几步固定：保证多脑那条路一定被走到（纯随机不够硬）
        printf "::notepad off\n::char off\n";
        printf "::brain new\n::brain use 0\n::brain save 1 b1.bin\n::brain use 1\n";
        printf "::brain drop 0\n::brain use 0\n";
        for (j = 0; j < n; j++) printf "%s\n", c[int(rand()*k)];
        printf "::revcheck\n";
        # 末尾再存一次盘：下一步用第二个进程读回来 —— 随机命令序列跑完的图也要读得回
        printf "::save m.bin\n";
    }' > "$W/s.txt"
    ( cd "$W" && "$AIP" --model m.bin < s.txt > out.txt 2>&1 )
    rc=$?
    why=""
    [ "$rc" -eq 0 ] || why="退出码 $rc"
    grep -qaE "段错误|Segmentation|Assertion|Aborted|core dumped" "$W/out.txt" && why="崩溃字样"
    grep -qa "命令表里有" "$W/out.txt" && why="命令表与实现漂移"
    grep -qa "反查表缺 0 条" "$W/out.txt" || [ -n "$why" ] || why="最后 ::revcheck 没说缺 0 条"
    [ -s "$W/out.txt" ] || why="没有任何输出"
    grep -qa "多脑" "$W/out.txt" || [ -n "$why" ] || why="多脑那条路没跑到"
    # **存读往返**：同一段随机操作跑完的图，用第二个进程读回来验一遍（§6.598 那类问题的现场）
    if [ -z "$why" ]; then
        printf '::notepad off\n::char off\n::revcheck\n猫 属于 动物 吗\n' > "$W/s2.txt"
        ( cd "$W" && "$AIP" --model m.bin < s2.txt > out2.txt 2>&1 )
        rc2=$?
        [ "$rc2" -eq 0 ] || why="读回进程退出码 $rc2"
        grep -qa "反查表缺 0 条" "$W/out2.txt" || [ -n "$why" ] || why="读回之后反查表不自洽"
        grep -qa "AI > " "$W/out2.txt" || [ -n "$why" ] || why="读回之后问不出一句话"
        grep -qaE "段错误|Segmentation|Assertion" "$W/out2.txt" && why="读回进程崩溃字样"
    fi
    if [ -n "$why" ]; then
        echo "  [FAIL] 种子 $i（$why）"
        tail -4 "$W/out.txt" | sed 's/^/        /'
        [ -f "$W/out2.txt" ] && tail -2 "$W/out2.txt" | sed 's/^/        /'
        fail=$((fail+1))
    else
        pass=$((pass+1))
    fi
    i=$((i+1))
done
[ -n "${AIP_CMDFUZZ_KEEP:-}" ] || rm -rf "$W"
[ -n "${AIP_CMDFUZZ_KEEP:-}" ] && echo "  [保留] $W"
echo "命令面随机会话：通过 $pass / 失败 $fail（种子 × 步数 = $SEEDS × $STEPS）"
[ "$fail" -eq 0 ] && echo "PASS cmdfuzz（命令面随机会话：切脑/存读/局部加载/op 组合没有把状态搞坏）" \
                  || echo "FAIL cmdfuzz（$fail 个种子红了）"
[ "$fail" -eq 0 ] || exit 1
exit 0
