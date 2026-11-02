#!/bin/sh
# tests/codebench.sh — **「独立写代码」的判据与基线**（阶段四 M0；消融/扩图 = M4；§6.698 / §6.707）
#
# 判据都是"**产物自己跑绿**"（产物自带 ok/FAIL 断言行、rc=0），不是"看起来像代码"：
#   A 修复段：给一份**只有机械错误**的源文件（缺分号 / 缺 #include / 未声明变量），
#     程序要自己走完 `::code test` 闭环（编→看错→改→再编→跑），**不许人肉修补**。
#   B 合成段：只给一句**中文任务描述**，要求它自己产出源文件并跑绿（14/14 是现状基线）。
#   C 消融段（M4 判据④「知识真的在用」）：
#     C1 **全量消融**：删掉全部「骨架/片段/模板 → 代码」边 ⇒ 14 个任务**一个都写不出来**；
#     C2 **只删片段**：骨架和模板的代码都留着 ⇒ 两个**组合**任务缺辅助函数必编不过、其余照旧过。
#   D 扩图段（M4 判据④后半「扩图不降语义」）：图扩到 **≥10⁶ 边**（70 万条与代码无关的知识）
#     ⇒ B 段仍然 14/14。
#
# 为什么 C 段要**保留一条无关的 `代码` 边**：全删会让失败原因变成"图上没有 `代码` 这种关系"，
# 那验的是"关系没登记"（更弱的失败），而不是"知识真的在用"（§6.700 踩过）。
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

A_PASS=0; A_TOTAL=0; D_OK=1
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

# ---------- 任务清单：B/C/D 三段共用**唯一一份** ----------
# B 段判"能不能写出来"，C 段判"删掉知识以后是不是真写不出来"，D 段判"图变大以后会不会掉"。
# 清单必须是同一份，否则"消融后 0 通过"可能只是清单换了（§6.609 的教训：量尺量不到）。
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
写一个程序：统计 2..N 里素数的个数（复用已有的素数判断）。
写一个程序：把数组排序后取中位数。
写一个程序：做素数计数（片段有依赖，被调用者写在了后面）。
EOF
TASKS=tasks.txt
N_TASKS=$(grep -c . "$TASKS" || true)

# ---------- 通用：跑一份清单（每任务一个日志，避免互相覆盖） ----------
# run_set <模型> <结果目录> <清单>   ⇒ 全局 _pass / _total
run_set() {
    _pass=0; _total=0
    rm -rf "$2"; mkdir -p "$2"
    _i=0
    while IFS= read -r _task; do
        _i=$((_i + 1)); _total=$((_total + 1))
        printf '::act on\n::code synth %s\n::quit\n' "$_task" > "$2/ask.in"
        # 同上：`::code synth` 也把产物写到 `_t/work/`，cwd 必须回仓库根
        ( cd "$ROOT" && "$WORK/build_aip" --model "$1" < "$WORK/$2/ask.in" > "$WORK/$2/r$_i.txt" 2>&1 )
        if grep -qa '测试全绿' "$WORK/$2/r$_i.txt"; then _pass=$((_pass + 1)); fi
    done < "$3"
}

# report_set <清单> <结果目录> —— 逐条打 OK/FAIL + 那一行的 `[合成]` 摘要
report_set() {
    _n=0
    while IFS= read -r _task; do
        _n=$((_n + 1))
        _line=$(grep -a '\[合成\]' "$2/r$_n.txt" 2>/dev/null | head -1 || true)
        _line=${_line##*\[合成\] }
        if grep -qa '测试全绿' "$2/r$_n.txt"; then
            printf '  [OK]   任务 %d：%s\n' "$_n" "$(printf '%s' "$_line" | cut -c1-104)"
        else
            printf '  [FAIL] 任务 %d：%.34s —— %s\n' "$_n" "$_task" "$(printf '%s' "$_line" | cut -c1-96)"
        fi
    done < "$1"
}

# ---------- B 段：从中文任务描述合成 ----------
# 走**真正的入口** `::code synth <中文描述>`（图里查任务/骨架/代码 → 落地 → 闭环）。
# 判据：产物能编、能跑、自带断言全绿（不是"看起来像代码"）。
echo "== B 段：中文描述 → 可跑的 C 程序（::code synth） =="
MODEL=${CMODEL:-$ROOT/code/code_model.bin}
if [ ! -s "$MODEL" ]; then       # 生成物不进仓库：缺了就现场造（和 codeknow 一样）
    [ -s "$ROOT/code/knowledge/capi.triples" ] || sh "$ROOT/tools/headers2triples.sh" "$ROOT/code/knowledge/capi.triples" 100000 > /dev/null 2>&1
    sh "$ROOT/code/build_code_model.sh" > /dev/null 2>&1
fi
[ -s "$MODEL" ] || { echo "FAIL: 缺 $MODEL（先跑 sh code/build_code_model.sh）"; exit 1; }
run_set "$MODEL" bset "$TASKS"
B_PASS=$_pass; B_TOTAL=$_total
report_set "$TASKS" bset

# ---------- C 段：消融（证明知识真的在用，M4 判据④） ----------
echo "== C 段：消融（删掉代码知识 ⇒ 必须写不出来） =="
# `代码` 边是**多行记录**（行尾 `\` 续行）：删主行必须连着删它的续行，
# 否则续行会变成"没有 TAB 的坏行"（§6.707 实测：6 条坏行，不致命但会把知识切碎）。
strip_code() {   # $1=输入文件 $2=只删哪些主体（正则）$3=输出（追加）
    awk -F'\t' -v pat="$2" 'BEGIN{OFS="\t"; skip=0}
        skip==1 { skip = ($0 ~ /\\$/); next }                 # 上一条是被删记录的续行 ⇒ 一起扔
        $2=="代码" && $1 ~ pat { skip = ($0 ~ /\\$/); next }
        { print }' "$1" >> "$3"
}

# C1 **全量消融**：骨架 + 片段 + 模板的代码全删 ⇒ 一条现成程序都查不到。
: > abl1.triples
strip_code "$ROOT/code/knowledge/skeletons.triples" "^(骨架|片段|模板):" abl1.triples
if [ -s "$ROOT/code/knowledge/compose.triples" ]; then
    strip_code "$ROOT/code/knowledge/compose.triples" "^(骨架|片段|模板):" abl1.triples
fi
# 只留一条**无关的** `代码` 边把关系登记住（否则失败原因变成"关系没登记"，那是更弱的判据）
printf '消融:占位\t代码\t消融:保留关系\n' >> abl1.triples
cp "$ROOT/model.bin" abl1.bin
printf '::ingest %s\n::save %s/abl1_model.bin\n::quit\n' "$WORK/abl1.triples" "$WORK" > abl1.in
./build_aip --model abl1.bin < abl1.in > abl1.log 2>&1
run_set "$WORK/abl1_model.bin" c1set "$TASKS"
C1_PASS=$_pass
printf '  C1 全量消融：%d / %d 个任务仍能写出来（要求 **0**）\n' "$C1_PASS" "$N_TASKS"
# 失败原因也得对：要么"没有它的 代码 边"（v1 路径），要么"没编过"（组合路径：壳在、片段没了）
C1_BADREASON=0
_n=0
while IFS= read -r _task; do
    _n=$((_n + 1))
    if ! grep -qa '没有它的 代码 边\|没编过' "c1set/r$_n.txt"; then C1_BADREASON=$((C1_BADREASON + 1)); fi
done < "$TASKS"
if [ "$C1_PASS" -eq 0 ]; then
    ok "C1 全量消融：$N_TASKS 个任务**一个都写不出来**"
else
    bad "C1 全量消融：删了全部代码边，居然还有 $C1_PASS 个任务写出来了" \
        "$(grep -la '测试全绿' c1set/r*.txt 2>/dev/null | tr '\n' ' ')"
fi
if [ "$C1_BADREASON" -eq 0 ]; then
    ok "C1 失败原因照实：每个任务都说清「没有它的 代码 边 / 没编过」"
else
    bad "C1 有 $C1_BADREASON 个任务的失败原因不对（应说「没有它的 代码 边」或「没编过」）" \
        "$(grep -La '没有它的 代码 边\|没编过' c1set/r*.txt 2>/dev/null | head -3 | tr '\n' ' ')"
fi

# C2 **只删片段**：骨架（整程序）和模板（main 壳）都留着 ⇒ 两个组合任务缺辅助函数必编不过，
# 其余 v1 任务一条都不该受影响。这条验的是"§6.703 的片段复用是承重的"，不是可有可无的装饰。
: > abl2.triples
cp "$ROOT/code/knowledge/skeletons.triples" abl2.triples      # 骨架全留（只为拿到 任务/骨架 边）
if [ -s "$ROOT/code/knowledge/compose.triples" ]; then
    strip_code "$ROOT/code/knowledge/compose.triples" "^片段:" abl2.triples          # 片段代码删掉，模板/主体留
fi
cp "$ROOT/model.bin" abl2.bin
printf '::ingest %s\n::save %s/abl2_model.bin\n::quit\n' "$WORK/abl2.triples" "$WORK" > abl2.in
./build_aip --model abl2.bin < abl2.in > abl2.log 2>&1
run_set "$WORK/abl2_model.bin" c2set "$TASKS"
C2_PASS=$_pass
C2_BAD=0; C2_COMPOSE=0
_n=0
while IFS= read -r _task; do
    _n=$((_n + 1))
    if grep -qa '组合：' "c2set/r$_n.txt"; then        # 组合任务：必须失败，且原因说得清
        C2_COMPOSE=$((C2_COMPOSE + 1))
        if grep -qa '测试全绿' "c2set/r$_n.txt"; then C2_BAD=$((C2_BAD + 1)); fi
        if ! grep -qa '没编过' "c2set/r$_n.txt"; then C2_BAD=$((C2_BAD + 1)); fi
    else                                              # v1 任务：不该受影响
        if ! grep -qa '测试全绿' "c2set/r$_n.txt"; then C2_BAD=$((C2_BAD + 1)); fi
    fi
done < "$TASKS"
printf '  C2 只删片段：%d / %d 通过（组合任务 %d 个，要求它们**失败**）\n' "$C2_PASS" "$N_TASKS" "$C2_COMPOSE"
if [ "$C2_BAD" -eq 0 ] && [ "$C2_COMPOSE" -gt 0 ]; then
    ok "C2 只删片段：$C2_COMPOSE 个组合任务因缺辅助函数编不过，其余照旧绿 ⇒ 片段复用是承重的"
else
    bad "C2 只删片段的结果不对（组合 $C2_COMPOSE 个；异常 $C2_BAD 处）" \
        "$(grep -la '组合：' c2set/r*.txt 2>/dev/null | head -2 | tr '\n' ' ')"
fi

# ---------- D 段：扩图不降语义（M4 判据④后半） ----------
# 加几十万条**与代码无关**的知识（噪声词/噪声类）：图从 3.3×10⁵ 边涨到 ≥10⁶ 边，
# 代码知识一条不动 ⇒ B 段必须仍然 15/15。这就是"扩图不会把已经会的东西弄丢"的证据。
echo "== D 段：扩图到 ≥10⁶ 边 ⇒ B 段不下降 =="
D_PASS=0
if [ -s "$MODEL" ]; then
    # 噪声条数**按现有边数现算**：沙箱里 `code_model.bin` 是现场重建的（`capi.triples` 不复制、
    # 原型上限也更小），写死 70 万可能凑不到 10⁶ ⇒ D 段会红，但红的是"图没扩够"不是产品。
    BASE_EDGES=$(printf '::stats\n::quit\n' | ./build_aip --model "$MODEL" 2>&1 | grep -a '符号节点' | head -1 | sed 's/.*边=//' | tr -dc '0-9' || true)
    BASE_EDGES=${BASE_EDGES:-0}
    NEED=$((1050000 - BASE_EDGES))
    [ "$NEED" -lt 10000 ] && NEED=10000
    printf '  （基线 %s 条边 ⇒ 需要 %d 条噪声知识）\n' "$BASE_EDGES" "$NEED"
    awk -v need="$NEED" 'BEGIN{ for (a = 0; a < 1300; a++) for (b = 0; b < 1300; b++) {
             if (a * 1300 + b >= need) exit
             printf "噪声词:%04d\t属于\t噪声类:%04d\n", a, b } }' > noise.triples
    cp "$MODEL" big.bin
    printf '::ingest %s/noise.triples\n::save %s/big_model.bin\n::quit\n' "$WORK" "$WORK" > big.in
    ./build_aip --model big.bin < big.in > big.log 2>&1
    BIG_EDGES=$(printf '::stats\n::quit\n' | ./build_aip --model big_model.bin 2>&1 | grep -a '符号节点' | head -1 | sed 's/.*边=//' | tr -dc '0-9' || true)
    BIG_EDGES=${BIG_EDGES:-0}
    # 硬门：图**真的**变大了。否则 D 段测的是"没扩图"，diff 为空也说明不了任何事。
    if [ "$BIG_EDGES" -ge 1000000 ]; then
        ok "扩图：$BASE_EDGES → $BIG_EDGES 条边 ≥ 10⁶"
    else
        bad "扩图没到 10⁶ 边（实测 $BIG_EDGES）—— D 段测的是空" ""
        D_OK=0
    fi
    # 载入也要量：10⁶ 边的图如果载入崩到几十秒，"扩图"就不是可用状态（阶段四 M3 口径 ≤2 s）
    _T0=$(date +%s%N)
    printf '::quit\n' | ./build_aip --model big_model.bin > /dev/null 2>&1
    _T1=$(date +%s%N)
    BIG_MS=$(( (_T1 - _T0) / 1000000 ))
    if [ "$BIG_MS" -le 3000 ]; then
        ok "10⁶ 边载入 ${BIG_MS} ms ≤ 3000 ms（单跑约 1.05 s）"
    else
        bad "10⁶ 边载入 ${BIG_MS} ms > 3000 ms" ""
        D_OK=0
    fi
    run_set "$WORK/big_model.bin" dset "$TASKS"
    D_PASS=$_pass
    printf '  D 扩图后 B 段：%d / %d\n' "$D_PASS" "$N_TASKS"
    if [ "$D_PASS" -eq "$B_PASS" ] && [ "$D_PASS" -eq "$N_TASKS" ]; then
        ok "扩图后 B 段 $D_PASS/$N_TASKS 不下降（与 B 段基线一致）"
    else
        bad "扩图后 B 段从 $B_PASS/$B_TOTAL 掉到 $D_PASS/$N_TASKS" \
            "$(grep -La '测试全绿' dset/r*.txt 2>/dev/null | head -3 | tr '\n' ' ')"
        D_OK=0
    fi
else
    bad "没有可用的代码模型 ⇒ D 段跳过（扩图不降语义没验）" "$MODEL"
    D_OK=0
fi

echo
printf 'A 修复段：%d / %d\n' "$A_PASS" "$A_TOTAL"
printf 'B 合成段：%d / %d（阶段四 M2 目标 ≥5/6）\n' "$B_PASS" "$B_TOTAL"
printf 'C 消融：C1 全量消融后 %d / %d 通过（要求 0）；C2 只删片段 %d / %d\n' "${C1_PASS:-0}" "$N_TASKS" "${C2_PASS:-0}" "$N_TASKS"
printf 'D 扩图：%d / %d（要求与 B 段相同）\n' "${D_PASS:-0}" "$N_TASKS"
[ -n "${1:-}" ] && [ "$1" = "--keep" ] || rm -rf "$WORK/src" "$WORK"/bset "$WORK"/c1set "$WORK"/c2set "$WORK"/dset \
    "$WORK"/synth* "$WORK"/run.in "$WORK"/out.txt "$WORK"/*.triples "$WORK"/big.bin "$WORK"/abl1.bin "$WORK"/abl2.bin \
    "$WORK"/big_model.bin "$WORK"/abl1_model.bin "$WORK"/abl2_model.bin 2>/dev/null
if [ "$A_PASS" -eq "$A_TOTAL" ] && [ "$B_PASS" -eq "$B_TOTAL" ] && \
   [ "$C1_PASS" -eq 0 ] && [ "$C1_BADREASON" -eq 0 ] && [ "$C2_BAD" -eq 0 ] && [ "$D_OK" -eq 1 ]; then
    printf 'PASS codebench（A %d/%d；B %d/%d；C1 全量消融 0/%d；C2 片段承重；D 扩图 %d/%d）\n' \
        "$A_PASS" "$A_TOTAL" "$B_PASS" "$B_TOTAL" "$N_TASKS" "$D_PASS" "$N_TASKS"
    exit 0
else
    printf 'FAIL codebench（A %d/%d；B %d/%d；C1 %d；C2 异常 %d；D %d）\n' \
        "$A_PASS" "$A_TOTAL" "$B_PASS" "$B_TOTAL" "${C1_PASS:-0}" "$C2_BAD" "$D_OK"
    exit 1
fi
