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

A_PASS=0; A_TOTAL=0; B_PASS=0; B_TOTAL=0; C_OK=0
ok() { printf '  [OK]   %s\n' "$1"; }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; }

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
# 走**真正的入口** `::code synth <中文描述>`（图里查任务/骨架/代码 → 落地 → 闭环）。
# 判据：产物能编、能跑、自带断言全绿（不是"看起来像代码"）。
echo "== B 段：中文描述 → 可跑的 C 程序（::code synth） =="
cat > tasks.txt <<'EOF'
写一个程序：读入 N（命令行参数），输出 1..N 里所有素数，每行一个。
写一个程序：读入一行字符串（stdin，≤200 字符），倒序输出。
写一个程序：读入 N，输出斐波那契数列前 N 项（用空格分隔）。
写一个程序：读入 N 个整数，输出它们的最大值、最小值、平均值。
写一个程序：实现二分查找，读入一个有序数组和一个 key，输出下标（没有输出 -1）并自带测试。
写一个程序：读入一个文件名，统计行数、单词数、字符数（类似 wc）。
写一个程序：读入 N 个整数，从小到大排序后输出。
写一个程序：两个 2x2 矩阵相乘并输出结果。
写一个程序：判断字符串是不是回文。
写一个程序：把十进制数转成二进制和十六进制输出。
写一个程序：求两个数的最大公约数。
写一个程序：统计字符串里的字母、数字、空格和其他字符个数。
EOF
MODEL=${CMODEL:-$ROOT/code/code_model.bin}
if [ ! -s "$MODEL" ]; then       # 生成物不进仓库：缺了就现场造（和 codeknow 一样）
    [ -s "$ROOT/code/knowledge/capi.triples" ] || sh "$ROOT/tools/headers2triples.sh" "$ROOT/code/knowledge/capi.triples" 100000 > /dev/null 2>&1
    sh "$ROOT/code/build_code_model.sh" > /dev/null 2>&1
fi
[ -s "$MODEL" ] || { echo "FAIL: 缺 $MODEL（先跑 sh code/build_code_model.sh）"; exit 1; }
i=0
while IFS= read -r task; do
    i=$((i + 1))
    B_TOTAL=$((B_TOTAL + 1))
    printf '::act on\n::code synth %s\n::quit\n' "$task" > ask.in
    ( cd "$ROOT" && _t/codebench/build_aip --model "$MODEL" < _t/codebench/ask.in > _t/codebench/synth.txt 2>&1 )
    if grep -qa '测试全绿' synth.txt; then
        B_PASS=$((B_PASS + 1))
        printf '  [OK]   任务 %d：%s\n' "$i" "$(grep -a '\[合成\]' synth.txt | head -1 | sed 's/.*\[合成\] //' | cut -c1-120)"
    else
        printf '  [FAIL] 任务 %d：%s —— %s\n' "$i" "$task" "$(grep -a '\[合成\]' synth.txt | head -1 | sed 's/.*\[合成\] //' | cut -c1-110)"
    fi
done < tasks.txt

# ---------- C 段：消融（证明知识真的在用） ----------
# 把 `骨架:* 代码` 这些边**删掉**（只灌不带代码的知识）⇒ 合成器必须**写不出来**。
# 如果删了还能"通过"，那说明代码是硬编码在程序里的 —— 那这条判据就是假的。
echo "== C 段：消融（删掉「骨架→代码」的边，必须写不出来） =="
# 只留**一个**骨架的代码边（这样 `代码` 这个关系仍然在图上 ⇒ 消融验的是"边没了"，
# 而不是"关系没登记"——后者是更弱的失败，会把"知识真的在用"这件事验偏）
awk -F'\t' 'BEGIN{OFS="\t"} { if ($2 == "代码" && $1 != "骨架:单词计数") next; print }' \
    "$ROOT/code/knowledge/skeletons.triples" > abl.triples
cp "$ROOT/model.bin" abl.bin
printf '::ingest %s\n::save %s/abl_model.bin\n::quit\n' "$WORK/abl.triples" "$WORK" > abl.in
( cd "$WORK" && "$ROOT/build_aip" --model abl.bin < abl.in > abl.log 2>&1 )
printf '::act on\n::code synth 写一个程序：输出 1..N 里的素数\n::quit\n' > abl2.in
( cd "$ROOT" && _t/codebench/build_aip --model "$WORK/abl_model.bin" < _t/codebench/abl2.in > _t/codebench/abl_out.txt 2>&1 )
if grep -qa '测试全绿' abl_out.txt; then
    bad "消融之后**居然还能写出来** —— 说明代码不是从图里来的" "$(grep -a '\[合成\]' abl_out.txt | head -1)"
elif grep -qa '没有它的 代码 边' abl_out.txt; then
    C_OK=1; ok "消融之后写不出来，而且**说清了原因**：$(grep -a '\[合成\]' abl_out.txt | head -1 | sed 's/.*\[合成\] //' | cut -c1-90)"
else
    bad "消融之后失败原因不对（应为「没有 代码 边」）" "$(grep -a '\[合成\]' abl_out.txt | head -1)"
fi

echo
printf 'A 修复段：%d / %d\n' "$A_PASS" "$A_TOTAL"
printf 'B 合成段：%d / %d（阶段四 M2 目标 ≥3/6）\n' "$B_PASS" "$B_TOTAL"
[ -n "${1:-}" ] && [ "$1" = "--keep" ] || rm -rf "$WORK/src" "$WORK"/synth* "$WORK"/run.in "$WORK"/out.txt
# A 段必须全绿（这是已实现的能力）；B 段是**尺子**，不绿不算失败
[ "$A_PASS" -eq "$A_TOTAL" ] && [ "$C_OK" -eq 1 ] && printf 'PASS codebench（A 修复 %d/%d；B 合成 %d/%d；C 消融通过）\n' "$A_PASS" "$A_TOTAL" "$B_PASS" "$B_TOTAL" || printf 'FAIL codebench（A %d/%d）\n' "$A_PASS" "$A_TOTAL"
[ "$A_PASS" -eq "$A_TOTAL" ]
