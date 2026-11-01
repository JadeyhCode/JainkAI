#!/bin/sh
# tests/codebench.sh — **「独立写代码」的判据与基线**（阶段四 M0；§6.698）
#
# 两段，判据都是"**产物自己跑绿**"，不是"看起来像代码"：
#   A 修复段：给一份**只有机械错误**的源文件（缺分号 / 缺 #include / 未声明变量），
#     程序要自己走完 `::code test` 闭环（编→看错→改→再编→跑），**不许人肉修补**。
#     这一段的现状是"能修"，它是后面自动合成的地基。
#   B 合成段：只给一句**中文任务描述**，要求它**自己产出**源文件并跑绿。
#     这一段是阶段四的目标；现在**没有实现**（基线必为 0），留着当尺子。
#
# 用法：sh tests/codebench.sh [--keep]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/codebench"
rm -rf "$WORK"; mkdir -p "$WORK/src"
cd "$WORK"
SRC="$ROOT/_t/work/cb"
rm -rf "$SRC"; mkdir -p "$SRC/src"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }
cp "$ROOT/relations.txt" . 2>/dev/null || true

A_PASS=0; A_TOTAL=0; B_PASS=0; B_TOTAL=0

# ---------- A 段：修复 ----------
# 每份源码自带测试（输出里 `FAIL` 行 ⇒ 不过），所以"跑绿"是**行为判据**不是"编过"。
mk_repair() {   # $1=文件名 $2=错误种类（写进 ../work/cb/src，因为 ::code 只认 _t/work/ 下的相对路径）
    case "$2" in
    semi)
        cat > "$SRC/src/$1" <<'EOF'
#include <stdio.h>
static int add(int a, int b) { return a + b; }
int main(void) {
    int got = add(2, 3)
    printf("%s\n", got == 5 ? "ok add" : "FAIL add");
    return got == 5 ? 0 : 1;
}
EOF
        ;;
    include)
        cat > "$SRC/src/$1" <<'EOF'
int main(void) {
    printf("%s\n", "ok printf");
    return 0;
}
EOF
        ;;
    undecl)
        cat > "$SRC/src/$1" <<'EOF'
#include <stdio.h>
int main(void) {
    total = 7;
    printf("%s\n", total == 7 ? "ok var" : "FAIL var");
    return total == 7 ? 0 : 1;
}
EOF
        ;;
    esac
}

echo "== A 段：机械错误修复（编→看错→改→再编→跑绿） =="
for spec in "a1.c semi" "a2.c include" "a3.c undecl"; do
    set -- $spec
    name=$1; kind=$2
    A_TOTAL=$((A_TOTAL + 1))
    mk_repair "$name" "$kind"
    printf '::act on\n::code test cb/src/%s\n::code\n::quit\n' "$name" > run.in
    # ⚠ 必须**从仓库根**跑：`code_round` 拼的是 `_t/work/<相对路径>`，
    # 在 `_t/codebench/` 里跑就变成 `_t/work/_t/work/...`（第一版就是这么"读不开"的）
    ( cd "$ROOT" && _t/codebench/build_aip --model _t/codebench/m.bin < _t/codebench/run.in > _t/codebench/out.txt 2>&1 )
    rc=$?
    if [ "$rc" -eq 0 ] && grep -qa '测试全绿' out.txt; then
        A_PASS=$((A_PASS + 1)); printf '  [OK]   %-6s（%s）%s\n' "$name" "$kind" "$(grep -a '写代码闭环' out.txt | tail -1 | sed 's/.*：//')"
    else
        printf '  [FAIL] %-6s（%s）rc=%s %s\n' "$name" "$kind" "$rc" "$(grep -a '写代码闭环' out.txt | tail -1 | sed 's/.*：//')"
    fi
done

# ---------- B 段：从中文任务描述合成 ----------
# 只给一句话。判据：目录里多出一个能编、能跑绿、且**自带测试**的 .c。
# 现在没有任何合成通道 ⇒ 预期 0；等阶段四把它做出来，这里的数字就是进度。
echo "== B 段：中文描述 → 可跑的 C 程序（阶段四目标；现在没有实现） =="
cat > tasks.txt <<'EOF'
写一个程序：读入 N（命令行参数），输出 1..N 里所有素数，每行一个。
写一个程序：读入一行字符串（stdin，≤200 字符），倒序输出。
写一个程序：读入 N，输出斐波那契数列前 N 项（用空格分隔）。
写一个程序：读入 N 个整数，输出它们的最大值、最小值、平均值。
写一个程序：实现二分查找，读入一个有序数组和一个 key，输出下标（没有输出 -1）并自带测试。
写一个程序：读入一个文件名，统计行数、单词数、字符数（类似 wc）。
EOF
i=0
while IFS= read -r task; do
    i=$((i + 1))
    B_TOTAL=$((B_TOTAL + 1))
    d="synth$i"
    mkdir -p "$d"
    # 走"人怎么说它就怎么做"的入口：直接把任务描述交给它（现在只会答话，不会产文件）
    printf '::act on\n%s\n::quit\n' "$task" > "$d/ask.in"
    ( cd "$d" && ../build_aip --model ../m.bin < ask.in > out.txt 2>&1 )
    made=$(ls "$d"/*.c 2>/dev/null | wc -l || true)
    green=0
    for f in "$d"/*.c; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        printf '::act on\n::code test %s\n::quit\n' "$base" > "$d/t.in"
        ( cd "$d" && ../build_aip --model ../m.bin < t.in > tout.txt 2>&1 ) && grep -qa '测试全绿' "$d/tout.txt" && green=1
    done
    if [ "${made:-0}" -gt 0 ] && [ "$green" -eq 1 ]; then
        B_PASS=$((B_PASS + 1)); printf '  [OK]   任务 %d：产出了 %s 并跑绿\n' "$i" "$(ls "$d"/*.c | head -1)"
    else
        printf '  [FAIL] 任务 %d：没有产出可运行的 .c（现有产物 .c 数=%s）—— %s\n' "$i" "${made:-0}" "$task"
    fi
done < tasks.txt

echo
printf 'A 修复段：%d / %d\n' "$A_PASS" "$A_TOTAL"
printf 'B 合成段：%d / %d（阶段四目标；现在是基线）\n' "$B_PASS" "$B_TOTAL"
[ -n "${1:-}" ] && [ "$1" = "--keep" ] || rm -rf "$WORK/src" "$WORK"/synth* "$WORK"/run.in "$WORK"/out.txt
# A 段必须全绿（这是已实现的能力）；B 段是**尺子**，不绿不算失败
[ "$A_PASS" -eq "$A_TOTAL" ] && printf 'PASS codebench（A 段全绿；B 段基线 %d / %d）\n' "$B_PASS" "$B_TOTAL" || printf 'FAIL codebench（A 段 %d / %d）\n' "$A_PASS" "$A_TOTAL"
[ "$A_PASS" -eq "$A_TOTAL" ]
