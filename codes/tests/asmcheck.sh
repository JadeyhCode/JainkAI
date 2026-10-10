#!/bin/sh
# tests/asmcheck.sh — **原语语言的解析器/校验器 + ASCII 代码词法**（第 32 套）
#
# 对应 `deepseek看这里.txt` 顶块「三、下一步顺序」第 5 条（线 A 的 ③ ④）：
#   ① 栈效应表按**代码真实行为**写（`token_lang.h` 的 PRIMS[]，见 audit §6.58）
#   ② `token_check_program()` 干跑校验（未知原语名 / 括号不配对 / 操作数不够 /
#      如果-结束不配对 / 空程序 / 参数缺失）
#   ③ `::asm check on|off`（默认关，`AIP_ASMCHECK=1`）：**没过就不执行**
#   ④ `lex.h` 的 ASCII 代码词法器（标识符/数字/逐字符标点、空白归一）+
#      `::lex <文本>` + `::ascii on|off`（接进 `::code` 的痕）
#
# 这一套盯六件事（每条都要能证伪）：
#   ① **默认关 = 与今天逐字节相同**：错名字被静默 intern 成 Token（Token 数 +1）、不报校验
#   ② 干跑**不 intern**：开着校验时拒了错名字，Token 数**一个不涨**
#   ③ 六条判据逐条认得出（未知原语名/括号/操作数/如果-结束/空程序/参数缺失）
#   ④ **没过就不执行**：图真的没动（`::edges` 看不到那条边），过了照旧执行
#   ⑤ 干跑按**真实行为**判：`分割`=清空整栈；`展开/检索` 推几个看数据 ⇒ 栈深不确定时
#      后面的操作数检查**跳过并记账**（认错比不认更糟 —— §6.58 那条纪律）
#   ⑥ 词法：往返判据（逐字保留 + canonical 幂等）与括号配对；`::ascii on` 才接进 `::code`
#
# 用法：sh tests/asmcheck.sh
#   ⚠ 开关反向测量（tests/switches_ab.sh 拿 `AIP_ASMCHECK=1` / `AIP_ASCII=1` 各跑一遍）时，
#     "默认关"那几条**自动跳过** —— 那时开关本来就是开着的（项目里成对开关的通用写法）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/asmcheck"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -x aip ] || { echo "FAIL: aip 没编出来"; exit 1; }
[ -s "$ROOT/model.bin" ] || { echo "FAIL: 找不到模型 $ROOT/model.bin"; exit 1; }
cp "$ROOT/model.bin" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# 会话：`::notepad off` 钉住流水线模式（环境变量强开草稿时也不影响本套的断言）
run() { { printf '::notepad off\n'; cat "$1"; } | ./aip --model model.bin > "$2" 2>&1 || true; }
# 日志里按顺序取第 n 个 `Token=N`（第 1 个是启动时的 [统计]，最后一个在退出时）
ntok() { sed -n 's/^\[统计\] Token=\([0-9]*\).*/\1/p' "$1" | sed -n "${2}p"; }

echo "== ① 默认关：错名字被静默吞掉（与今天逐字节相同） =="
cat > base_in.txt <<'EOF'
::stats
::asm [原语:连结] 猫
::stats
::asm check
::quit
EOF
run base_in.txt base.log
if [ -z "${AIP_ASMCHECK:-}" ]; then
    grep -qF '[执行了 2 个 token]' base.log \
        && ok "默认关：「::asm [原语:连结] 猫」 照旧执行（不校验、不报错）" \
        || bad "默认关的执行" "$(grep -m2 '执行了\|校验' base.log)"
    t1=$(ntok base.log 2); t2=$(ntok base.log 3)
    [ -n "$t1" ] && [ -n "$t2" ] && [ "$t2" -eq $((t1 + 1)) ] \
        && ok "默认关：打错的原语名被 intern 成真 Token（Token $t1 → $t2，+1 就是那个错字）" \
        || bad "默认关的 intern" "Token $t1 → $t2"
    if grep -q '\[校验\]' base.log; then bad "默认关时不该有校验输出" "$(grep -m1 '校验' base.log)"
    else ok "默认关：一个字都不多说（日志里没有 [校验] 行）"; fi
    grep -q '原语干跑校验 = 关' base.log \
        && ok "「::asm check」 自报默认关" || bad "check 状态" "$(grep -m1 '干跑校验' base.log)"
else
    # 反向测量（AIP_ASMCHECK=1）：默认值那几条按规矩跳过，但**必须**看到校验在起作用
    grep -qF '未知原语名：`[原语:连结]`' base.log && grep -qF '[校验] 没过' base.log \
        && ok "AIP_ASMCHECK=1（反向测量）：启动即开，第一个程序就被拦下" \
        || bad "反向测量 AIP_ASMCHECK" "$(grep -m3 '校验\|执行了' base.log)"
    grep -qF '[执行了 2 个 token]' base.log \
        && bad "反向测量：默认值断言没跳过（不该执行却执行了）" "$(grep -m1 '执行了' base.log)" \
        || ok "AIP_ASMCHECK=1：默认值断言按规矩跳过（行为与默认跑法**故意不同**）"
fi

echo "== ② 干跑不 intern + ③ 六条判据 + ④ 没过就不执行 + ⑤ 按真实行为判 =="
cat > chk_in.txt <<'EOF'
::stats
::asm check on
::asm [原语:连结] 猫
::stats
::asm [原语:好
::asm [原语:连接]
::asm [原语:对象] 猫 [原语:如果]
::asm [原语:结束]
::asm [原语:否则]
::asm [原语:对象]
EOF
printf '::asm \t\n' >> chk_in.txt          # 空程序：只有空白的序列（空白不进词，词数 0）
cat >> chk_in.txt <<'EOF'
::asm [原语:对象] 拒绝甲 [原语:对象] 拒绝乙 [原语:连接] [原语:连接]
::edges 拒绝甲
::asm [原语:对象] 校验甲 [原语:对象] 校验乙 [原语:连接]
::edges 校验甲
::asm [原语:对象] 校验丙 [原语:展开] [原语:连接]
::asm [原语:对象] 校验丁 [原语:对象] 校验戊 [原语:分割]
::asm check off
::asm [原语:连捷] 狗 猫
::stats
::quit
EOF
run chk_in.txt chk.log

grep -q '原语干跑校验 = 开' chk.log && ok "「::asm check on」 打开了" \
    || bad "check on" "$(grep -m1 '干跑校验' chk.log)"
grep -qF '未知原语名：`[原语:连结]`' chk.log \
    && ok "③ 未知原语名认得出来（并写明干跑不 intern）" \
    || bad "未知原语名" "$(grep -m2 '校验' chk.log)"
grep -qF '括号不配对：`[原语:好`' chk.log \
    && ok "③ 括号不配对认得出来（「[原语:好」 少了 「]」）" \
    || bad "括号不配对" "$(grep -m3 '校验\|括号' chk.log)"
grep -qF '操作数不够：`[原语:连接]` 要 2 个操作数，栈上只有 0 个' chk.log \
    && ok "③ 操作数不够认得出来（「[原语:连接]」 空栈）" \
    || bad "操作数不够" "$(grep -m4 '操作数不够' chk.log)"
grep -qF '如果-结束不配对：还开着 1 层' chk.log \
    && ok "③ 如果-结束不配对（开着 1 层）" \
    || bad "如果-结束配对" "$(grep -m5 '配对' chk.log)"
grep -qF '结束 没有对应的 如果' chk.log && grep -qF '否则 没有对应的 如果' chk.log \
    && ok "③ 孤立的 「结束」/「否则」 也认（没有对应的 如果）" \
    || bad "孤立块记号" "$(grep -m6 '对应的 如果' chk.log)"
grep -qF '参数缺失：`[原语:对象]` 后面没有词了' chk.log \
    && ok "③ 参数缺失认得出来（对象后面没词了）" \
    || bad "参数缺失" "$(grep -m3 '参数缺失' chk.log)"
grep -qF '空程序：一个词都没有' chk.log \
    && ok "③ 空程序认得出来（只有空白的序列）" \
    || bad "空程序" "$(grep -m2 '空程序' chk.log)"

t1=$(ntok chk.log 2); t2=$(ntok chk.log 3)
[ -n "$t1" ] && [ -n "$t2" ] && [ "$t2" -eq "$t1" ] \
    && ok "② 干跑**不 intern**：拒了一串错名字之后 Token 数一个不涨（$t1 → $t2）" \
    || bad "干跑不 intern" "Token $t1 → $t2"
if grep -qF '[执行了 2 个 token]' chk.log; then bad "没过却执行了" "$(grep -m2 '执行了' chk.log)"
else ok "④ 没过**就不执行**（日志里根本没有 「执行了 2 个 token」）"; fi
if grep -A6 -F '[拒绝甲]' chk.log | grep -q '拒绝乙'; then
    bad "没过却动了图" "$(grep -A6 -F '[拒绝甲]' chk.log | head -7)"
else ok "④ 被拒的程序**图一个字都没动**（::edges 拒绝甲 里没有 拒绝乙）"; fi
grep -qF '[校验] 通过：5 个词' chk.log \
    && ok "④ 合法序列校验通过（「[原语:对象] A [原语:对象] B [原语:连接]」）" \
    || bad "合法序列" "$(grep -m3 '校验' chk.log)"
grep -A6 -F '[校验甲]' chk.log | grep -q '校验乙' \
    && ok "④ 过了的那次真的落了边（第二次 ::edges 校验甲 看得到 校验乙）" \
    || bad "落了边" "$(grep -A6 -F '[校验甲]' chk.log | head -7)"
grep -qF '[校验] 通过：4 个词，干跑末态栈深不确定' chk.log && grep -qF '跳过 1 次操作数检查' chk.log \
    && ok "⑤ 「展开」 推几个看数据 ⇒ 栈深不确定、后面的操作数检查跳过**并记账**" \
    || bad "不确定的栈深" "$(grep -m3 '不确定\|跳过' chk.log)"
grep -qF '[校验] 通过：5 个词，干跑末态栈深 0' chk.log \
    && ok "⑤ 「分割」 按**真实行为**判：清空整栈（末态栈深 0，不是「退回本语句开头」）" \
    || bad "分割的栈效应" "$(grep -m6 '栈深' chk.log)"
t3=$(ntok chk.log 4)
[ -n "$t2" ] && [ -n "$t3" ] && [ "$t3" -gt "$t2" ] \
    && ok "过了的程序真的建了 Token（$t2 → $t3：校验甲/乙/丙/丁/戊）" \
    || bad "执行后的 Token 数" "$t2 → $t3"
if [ -z "${AIP_ASMCHECK:-}" ]; then
    grep -qF '[执行了 3 个 token]' chk.log && grep -qF '原语干跑校验 = 关' chk.log \
        && ok "「::asm check off」 之后回到默认（错名字又被吞掉、照旧执行）" \
        || bad "check off" "$(grep -m3 '干跑校验\|连捷\|执行了' chk.log)"
fi

echo "== ① 栈效应表是**看得见**的（::prims 打的就是校验器判的那张表） =="
cat > prims_in.txt <<'EOF'
::prims
::quit
EOF
run prims_in.txt prims.log
grep -qF '[原语:分割]  （栈：要 0 拿 0 放 0（真实行为：清空整栈））' prims.log \
    && ok "① 「分割」 写明是**清空整栈**（名字与真实行为不一致的地方明写）" \
    || bad "分割的效应" "$(grep -m1 '原语:分割' prims.log)"
grep -qF '[原语:嵌套]  （栈：要 2 拿 0 放 0（真实行为：整栈折成一个））' prims.log \
    && ok "① 「嵌套」 写明是**整栈折成一个**（同上，§6.58）" \
    || bad "嵌套的效应" "$(grep -m1 '原语:嵌套' prims.log)"
grep -qF '[原语:展开]' prims.log && grep -qF '推几个看数据 ⇒ 栈深不确定' prims.log \
    && ok "① 「展开」 写明栈深会不确定（校验器从那一步起跳过操作数检查）" \
    || bad "展开的效应" "$(grep -m1 '原语:展开' prims.log)"
grep -qF '[原语:对象]  （栈：要 0 拿 0 放 1，吃下一个词当参数）' prims.log \
    && ok "① 「对象」 要的是下一个词、不是栈上操作数（校验器分得清）" \
    || bad "对象的效应" "$(grep -m1 '原语:对象' prims.log)"

echo "== ③ 环境变量 AIP_ASMCHECK=1：不敲 ::asm check 也是开的 =="
cat > env_in.txt <<'EOF'
::asm [原语:连结] 猫
::asm [原语:对象] 校验己 [原语:对象] 校验庚 [原语:连接]
::asm check
::quit
EOF
{ printf '::notepad off\n'; cat env_in.txt; } | AIP_ASMCHECK=1 ./aip --model model.bin > env.log 2>&1 || true
grep -qF '未知原语名：`[原语:连结]`' env.log \
    && ok "AIP_ASMCHECK=1 启动即开（第一个程序就被拦）" \
    || bad "AIP_ASMCHECK" "$(grep -m2 '校验\|执行了' env.log)"
grep -qF '[校验] 通过：5 个词' env.log && grep -qF '[执行了 5 个 token]' env.log \
    && ok "同一会话里合法程序照旧跑（开了校验 ≠ 不能用）" \
    || bad "合法程序" "$(grep -m3 '校验\|执行' env.log)"

echo "== ⑥ ASCII 代码词法（lex.h）：往返判据 + 标点逐字符 + 空白归一 =="
cat > lex_in.txt <<'EOF'
::lex int main(void) { return 0; }
::lex    x   =   1 ;
::lex if (x { }
::lex 猫 = 1;
::lex char *s = "a b";
::lex a.b 12.5
::ascii
::quit
EOF
run lex_in.txt lex.log
grep -qF 'canonical：int main ( void ) { return 0 ; }' lex.log \
    && ok "⑥ 标点**逐字符**成词（canonical：int main ( void ) { return 0 ; }）" \
    || bad "标点逐字符" "$(grep -m1 'canonical' lex.log)"
grep -qF '词 10 个（标识符 4 / 数字 1 / 标点 5 / 非ASCII 0）' lex.log \
    && ok "⑥ 词数/类别分得清（10 个词：4 标识符 + 1 数字 + 5 标点）" \
    || bad "词数分类" "$(grep -m1 '代码词法' lex.log)"
grep -qF 'canonical：x = 1 ;' lex.log \
    && ok "⑥ 空白**归一**（多空格/制表都只算一处分隔）" \
    || bad "空白归一" "$(grep -m2 'canonical' lex.log)"
grep -qF '往返：逐字保留 ✓ / 幂等 ✓ → 成立' lex.log \
    && ok "⑥ 往返判据成立（判据只有 lex.h 一处）" \
    || bad "往返判据" "$(grep -m3 '往返' lex.log)"
grep -qF '括号 `()` `[]` `{}`：不配对 ✗' lex.log && grep -qF '还有 1 个开括号没关上' lex.log \
    && ok "⑥ 「if (x { }」 括号不配对（源码层面，与「一个词里成对」不是一回事）" \
    || bad "括号配对" "$(grep -m4 '括号' lex.log)"
grep -qF '非ASCII 3' lex.log \
    && ok "⑥ 非 ASCII 按字节数出来（「猫」 = 3 字节；名义边界，照实说）" \
    || bad "非 ASCII" "$(grep -m2 '非ASCII' lex.log)"
grep -qF '标识符`a` 标点`.` 标识符`b` 数字`12.5`' lex.log \
    && ok "⑥ 词边界：「a.b」 → a . b；「12.5」 → 一个数字" \
    || bad "词边界" "$(grep -m4 '词表' lex.log)"
grep -qF 'canonical：char * s = " a b " ;' lex.log \
    && ok "⑥ 字符串里的空格**不丢字节**（口径：非空白字节一个不改，canonical 供人看）" \
    || bad "字符串里的空格" "$(grep -m5 'canonical' lex.log)"
if [ -z "${AIP_ASCII:-}" ]; then
    grep -q '接进代码通道 = 关' lex.log \
        && ok "「::ascii」（默认关）—— 这一层没接进代码通道" \
        || bad "ascii 默认" "$(grep -m1 '代码通道' lex.log)"
else
    grep -q '接进代码通道 = 开' lex.log \
        && ok "AIP_ASCII=1（反向测量）：默认值断言按规矩跳过（自报是开的）" \
        || bad "AIP_ASCII 反向测量" "$(grep -m1 '代码通道' lex.log)"
fi

echo "== ⑥ ::ascii on 才接进 ::code 的痕（关着时逐字节同今天） =="
cat > code_in.txt <<'EOF'
::act on
::act write 词法.c #include <stdio.h>\nint main(void){puts("hi");}\n\n
::code try 词法.c
::ascii on
::code try 词法.c
::quit
EOF
run code_in.txt code.log
n_lex=$(grep -c '  词法：' code.log || true)
if [ -z "${AIP_ASCII:-}" ]; then
    [ "$n_lex" -eq 1 ] \
        && ok "关着那一次 「::code」 没有词法行、开着那一次有（整份日志正好 1 条）" \
        || bad "::code 的词法行数" "$(grep -n '词法：\|写代码闭环' code.log | head -6)"
    grep -q '接进代码通道 = 开' code.log && ok "「::ascii on」 打开了" \
        || bad "ascii on" "$(grep -m1 '代码通道' code.log)"
else
    [ "$n_lex" -eq 2 ] && ok "AIP_ASCII=1：两次 ::code 都带词法行（反向测量）" \
        || bad "AIP_ASCII 跑法" "$(grep -n '词法：' code.log | head -4)"
fi
grep -qF '词法：ASCII 逐字保留 ✓（canonical 幂等）' code.log \
    && ok "源码过了一遍**逐字保留**判据（写出去的字节词法层没改）" \
    || bad "逐字保留那行" "$(grep -m2 '词法' code.log)"

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS asmcheck（原语干跑校验 + ASCII 代码词法：$pass 项）"
else
    echo "FAIL asmcheck（$fail 项失败，$pass 项通过）"; exit 1
fi
