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
    # 命令池：**语法全部正确**（用法错误的命令只会打印帮助，等于白跑一步）
    awk -v s="$i" -v n="$STEPS" '
    BEGIN{
        srand(s);
        c[0]="::brain new";          c[1]="::brain list";      c[2]="::brain use 1";
        c[3]="::stats";              c[4]="::mods";            c[5]="::learn";
        c[6]="::src 猫 属于 动物";   c[7]="::rel 猫";          c[8]="::rel 麒麟";
        c[9]="::askconflict off";    c[10]="::askconflict on";
        c[11]="::natcoin on";        c[12]="::natcoin off";
        c[13]="::op 加前提 猫 动物"; c[14]="::op 回收临时";    c[15]="::op 中间插词 鸟 飞";
        c[16]="::teach 麒麟 属于 神兽"; c[17]="::teach 月亮 喵 鱼";
        c[18]="::save m.bin";        c[19]="::load m.bin";
        c[20]="::brain save 1 b1.bin"; c[21]="::brain drop 1";
        c[22]="猫 属于 动物 吗";     c[23]="麒麟 属于 神兽 吗";
        c[24]="::parts save p.parts"; c[25]="::parts attach p.parts";
        c[26]="::parts detach";      c[27]="::gc";
        c[28]="::lex int main(void){return 0;}";
        c[29]="::brain fork";
        c[30]="::revcheck";
        # 前几步固定：保证多脑那条路一定被走到（随机池里 5/31 的概率不够硬）
        printf "::notepad off\n::char off\n";
        printf "::brain new\n::brain use 0\n::brain save 1 b1.bin\n::brain use 1\n";
        printf "::brain drop 0\n::brain use 0\n";
        for (k = 0; k < n; k++) printf "%s\n", c[int(rand()*31)];
        printf "::revcheck\n";
    }' > "$W/s.txt"
    ( cd "$W" && "$AIP" --model m.bin < s.txt > out.txt 2>&1 )
    rc=$?
    why=""
    [ "$rc" -eq 0 ] || why="退出码 $rc"
    grep -qE "段错误|Segmentation|Assertion|Aborted|core dumped" "$W/out.txt" && why="崩溃字样"
    grep -q "命令表里有" "$W/out.txt" && why="命令表与实现漂移"
    grep -q "反查表缺 0 条" "$W/out.txt" || [ -n "$why" ] || why="最后 ::revcheck 没说缺 0 条"
    [ -s "$W/out.txt" ] || why="没有任何输出"
    grep -q "多脑" "$W/out.txt" || [ -n "$why" ] || why="多脑那条路没跑到"
    if [ -n "$why" ]; then
        echo "  [FAIL] 种子 $i（$why）"
        tail -6 "$W/out.txt" | sed 's/^/        /'
        fail=$((fail+1))
    else
        pass=$((pass+1))
    fi
    i=$((i+1))
done
rm -rf "$W"
echo "命令面随机会话：通过 $pass / 失败 $fail（种子 × 步数 = $SEEDS × $STEPS）"
[ "$fail" -eq 0 ] && echo "PASS cmdfuzz（命令面随机会话：切脑/存读/局部加载/op 组合没有把状态搞坏）" \
                  || echo "FAIL cmdfuzz（$fail 个种子红了）"
[ "$fail" -eq 0 ] || exit 1
exit 0
