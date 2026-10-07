#!/bin/sh
# tests/tokenop.sh — **Token 行为（图操作）+ 临时 Token + 修正的记忆**（CLI 级）
#
# 对应 C 级看护 tools/optest.c。这里盯的是「在真模型上跑起来是什么样」：
#
#   ① ::op 表列出来、::op 跑一条真能改图（改完 ::edges 看得见）
#   ② 中间插词：A—B 的直连被拆，改走 A→[临:中转]→B（**不直连**，经它中转）
#   ③ 直连是**知识**（观测到的关系边）时不拆 —— 铁律 2 没松，只加中转
#   ④ 随语境接线 / 指定接线：给「带某 token 的连接」接上目标（语境一起带过去）
#   ⑤ 临时 Token **不存盘**：存读之后它不在（也不留悬空边），模型照常能装
#   ⑥ 修正的记忆：改口留 [修正:…] 标记、::revise 列得出、**存读还在**（来源监控持久）
#
# 用法：sh tests/tokenop.sh [模型文件]        默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/tokenop"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o aip "$ROOT/main.c" -lm
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# `::notepad off`：**钉住"流水线模式"** —— 这样环境变量强制打开"AI 自己写草稿"时
# （`AIP_PATCH=记事本 AIP_NOTEPAD=on`），这套看护的断言依然测的是它要测的东西。
run() { cp model.bin m.bin; { printf '::notepad off\n'; printf "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 行为表：列得出来，跑得动 =="
run '::op\n' o1
grep -q 'Token 行为（图操作）：6 条' o1 && ok "::op 列出 6 条行为（表生成）" \
    || bad "行为表" "$(grep -A3 'Token 行为' o1 | head -4)"
for op in 加前提 加反前提 前提接线 中间插词 回收临时 调用补丁; do
    grep -q "::op $op" o1 || bad "表里少了行为 $op" "$(grep '::op' o1 | head -5)"
done
grep -q '当前未回收的临时 Token：0 个' o1 && ok "开局没有临时 Token（草稿是干净的）" \
    || bad "临时计数" "$(grep '临时 Token' o1 | head -2)"

echo "== ② 中间插词：不直连，经临时 Token 中转 =="
run '::mod try 加边 猫 月亮\n::op 中间插词 猫 月亮\n::edges 猫\n::edges [临:中转]\n' o2
grep -q '行为 中间插词' o2 && ok "行为跑起来了（展开成意图并应用）" \
    || bad "中间插词" "$(grep '行为' o2 | head -3)"
grep -qE '→ \[临:中转\] *_? *hits=[1-9]' o2 || grep -q '→ \[临:中转\]' o2 \
    && ok "A → 中转 建出来了（hits>0，不是「没依据的残留」）" \
    || bad "中转边" "$(grep '→' o2 | tail -4)"
grep -q '\[\[临:中转\]\] 有 1 条边' o2 && ok "中转 Token 自己连到 B（查得到它的边）" \
    || bad "中转的边" "$(grep -A2 '临:中转' o2 | tail -3)"
# 只看 ::edges 猫 那一段（中转自己那条 → 月亮 是另一段，别混进来）。
# 注意输出带 "你 > " 前缀，所以按「含 [猫] 有 N 条边」到下一个 "你 > " 之间取。
awk '/\[猫\] 有 [0-9]+ 条边/{f=1;next} f&&/^你 > /{f=0} f' o2 > o2_cat.txt
if [ ! -s o2_cat.txt ]; then
    bad "没取到 ::edges 猫 那一段（测试自己写错了）" "$(grep -n '猫\] 有' o2 | head -2)"
elif grep -qE '→ 月亮' o2_cat.txt; then
    bad "A—B 的**直连还在**（语义要求：不要直接联想到）" "$(cat o2_cat.txt)"
else
    ok "A—B 的直连被拆了（连接改走中转）"
fi

echo "== ③ 直连是知识（观测关系边）时：不拆，只加中转 =="
run '::op 中间插词 猫 鱼\n::edges 猫\n' o3
grep -qE '→ 鱼 *_? *hits=' o3 && ok "「猫 吃 鱼」是知识 → 不拆（铁律 2）" \
    || bad "关系边被拆了" "$(grep '鱼' o3 | head -3)"
grep -q '→ \[临:中转\]' o3 && ok "但中转照样接上（联想改走中转）" \
    || bad "中转没接上" "$(grep '→' o3 | tail -4)"

echo "== ④ 加前提 / 前提接线：前提 token 是**普通 token**，逻辑查得到它 =="
# 「淘汰语境」后的表示：A —[前提]→ P → B。P 是普通 token（联想照走），
# `[前提]` 是挂在动作槽上的行为 token（标记），::premise 直接答出 P 是谁。
run '::premhold mention\n::op 加前提 猫 月亮
::edges 猫
::premise 猫 月亮
' o4
grep -q '行为 加前提' o4 && ok "::op 加前提 跑通（A —[前提]→ P → B）" \
    || bad "加前提" "$(grep '行为' o4 | head -3)"
grep -q '\*\*前提\*\*' o4 && ok "::edges 把前提链接标出来（不是藏在字段里）" \
    || bad "前提标记" "$(grep -E '→|前提' o4 | head -4)"
grep -q '之间的前提 token' o4 && ok "::premise 答出「这两个之间的前提 token 是什么」" \
    || bad "::premise" "$(grep -A2 '前提' o4 | head -4)"
# 前提用**真词**时：它照常能被联想到（有 hits、可走边），逻辑也知道它是前提
run '::premhold mention\n::op 加前提 猫 月亮 食物
::edges 猫
::premise 猫 月亮
' o4b
grep -qE '→ 食物 *_? *hits=[1-9]' o4b && ok "前提用真词（食物）：前提 token 有边有权重 —— 能被正常联想到" \
    || bad "前提可联想" "$(grep '食物' o4b | head -3)"
grep -q '食物' o4b && grep -q '之间的前提 token' o4b && ok "逻辑同时知道「食物 是这两个之间的前提」" \
    || bad "前提可识别" "$(grep -A2 '前提 token' o4b | head -3)"
# 前提接线：让 A 经前提 P 也能到 Y（选择判据是图里的前提链接）
run '::premhold mention\n::op 加前提 猫 月亮 食物
::op 前提接线 猫 食物 鱼
::edges 食物
::premise 猫 鱼
' o5
grep -q '行为 前提接线' o5 && ok "::op 前提接线 跑通（经前提 P 接上目标）" \
    || bad "前提接线" "$(grep '行为' o5 | head -3)"
grep -qE '→ 鱼 *_? *hits=[1-9]' o5 && ok "前提的出口 P→鱼 建出来了" \
    || bad "前提出口" "$(grep '鱼' o5 | head -3)"
grep -q '之间的前提 token' o5 && ok "::premise 猫 鱼 也答得出前提（食物）" \
    || bad "前提查询" "$(grep -A2 '前提 token' o5 | head -3)"

echo "== ⑤ 临时 Token 不存盘：存读之后它不在，模型照常装 =="
run '::op 中间插词 猫 月亮\n::save slot\n' o7
[ -s slot ] && ok "存盘成功（临时 Token 与指向它的边被跳过）" || bad "存盘" "$(tail -2 o7)"
mv slot m.bin
printf '::op\n::edges [临:中转]\n::quit\n' | ./aip --model m.bin > o8 2>&1 || true
grep -q '当前未回收的临时 Token：0 个' o8 && ok "重载后没有临时 Token（草稿没被写成知识）" \
    || bad "临时 Token 被存下来了" "$(grep '临时 Token' o8 | head -2)"
grep -q '没有这个 Token' o8 && ok "指向它的边也没有留下悬空引用（装载不报错）" \
    || bad "悬空引用" "$(grep -A2 '临:中转' o8 | head -3)"

echo "== ⑥ 修正的记忆（来源监控）：改口留痕、列得出、存读还在 =="
run '猫 有 毛\n猫 没有 毛\n::revise\n::rel 猫\n' o9
grep -q '改 过来 了' o9 && ok "改口时如实说「改过来了」" || bad "改口" "$(grep 'AI >' o9 | tail -2)"
grep -q '\[修正:猫_不_有_毛\]' o9 && ok "落下 [修正:猫_不_有_毛] 标记（记得住「改过口」）" \
    || bad "修正标记" "$(grep -A4 '修正的记忆' o9)"
grep -q '猫 --\[不 有\]--> 毛$' o9 && ok "知识跟着后说的走（留痕不改知识）" \
    || bad "知识极性" "$(grep '毛' o9 | head -3)"
grep -q '猫 --\[有\]--> 毛$' o9 && bad "旧的极性复活了（留痕不该改知识）" "$(grep '毛' o9 | head -3)" \
    || ok "旧的极性没有复活"
run '猫 有 毛\n猫 没有 毛\n::save slot2\n' o10
mv slot2 m.bin
printf '::revise\n::quit\n' | ./aip --model m.bin > o11 2>&1 || true
grep -q '\[修正:猫_不_有_毛\]' o11 && ok "重载后修正的记忆还在（来源监控是持久的）" \
    || bad "修正记忆丢失" "$(grep -A3 '修正的记忆' o11)"

echo "== ⑦ 原语栈式组合：把行为写进语言层（复用已有栈机，不新造栈） =="
# [原语:对象] A [原语:对象] B [原语:插词] —— 用已有的 Interp 栈机组合出「插词」
run '::mod try 加边 猫 月亮
::asm [原语:对象] 猫 [原语:对象] 月亮 [原语:插词]
::edges 猫
::edges [临:中转]
' o12
grep -q '\[原语 中间插词\]' o12 && ok "[原语:插词] 跑通了（原语 → 行为 → 安全内核）" \
    || bad "原语插词" "$(grep '原语' o12 | head -3)"
awk '/\[猫\] 有 [0-9]+ 条边/{f=1;next} f&&/^你 > /{f=0} f' o12 > o12_cat.txt
if [ ! -s o12_cat.txt ]; then
    bad "没取到 ::edges 猫 那一段（测试自己写错了）" "$(grep -n '猫\] 有' o12 | head -2)"
else
    grep -qE '→ \[临:中转\]' o12_cat.txt && ok "A → 中转 建出来了" \
        || bad "中转边" "$(cat o12_cat.txt)"
    grep -qE '→ 月亮' o12_cat.txt && bad "直连还在（插词应当拆掉它）" "$(cat o12_cat.txt)" \
        || ok "A—B 直连被拆（与 ::op 同一条路）"
fi

# [原语:插词为] <名字>：中转 Token 用指定名字
run '::asm [原语:对象] 猫 [原语:对象] 桌子 [原语:插词为] 家具
::edges 猫
' o13
grep -q '\[原语 中间插词\]' o13 && grep -qE '→ 家具 *_? *hits=[1-9]' o13 \
    && ok "[原语:插词为] <名字>：中转用指定 Token（家具）" \
    || bad "插词为" "$(grep -E '原语|家具' o13 | head -4)"

# [原语:前关联] P + [原语:连接] A B → A —[前提]→ P → B（语境的替代表示）
run '::premhold mention\n::asm [原语:对象] 猫 [原语:对象] 吃 [原语:前关联] 饿 [原语:连接]\n::edges 猫\n::premise 猫 饿\n' o14
grep -q '\[原语 加前提\]' o14 && ok "[原语:前关联]+[原语:连接] → 走「加前提」" \
    || bad "原语加前提" "$(grep '原语' o14 | head -3)"
grep -q '\*\*前提\*\*' o14 && ok "建出的是前提链接（不是退役的语境字段）" \
    || bad "前提链接" "$(grep -E '饿|前提' o14 | head -4)"
grep -q '之间的前提 token' o14 && ok "::premise 猫 饿 答得出前提" \
    || bad "原语前提查询" "$(grep -A2 '前提 token' o14 | head -4)"
# [原语:前提接线]：让 A 经前提 P 也能到 Y
run '::asm [原语:对象] 吃 [原语:对象] 猫 [原语:对象] 火 [原语:前提接线]\n::edges 吃\n' o15
grep -q '\[原语 前提接线\]' o15 && ok "[原语:前提接线] 跑通了" \
    || bad "原语前提接线" "$(grep '原语' o15 | head -3)"
grep -qE '→ 火 *_? *hits=[1-9]' o15 && ok "前提的出口 P→火 建出来了" \
    || bad "前提接线结果" "$(grep '火' o15 | head -4)"

# [原语:回收临时]：没人引用的草稿才收（这里中转还在用，应当 0 条）
run '::asm [原语:对象] 猫 [原语:对象] 月亮 [原语:插词]
::asm [原语:回收临时]
' o16
grep -q '\[原语 回收临时\] 没人引用的临时 Token 0 个' o16 \
    && ok "[原语:回收临时] 在位，且在用中的草稿不收" || bad "原语回收" "$(grep '回收' o16)"

# ::prims 也必须由**原语表**生成（以前它手抄了第三份清单，会漏列新原语）
run '::prims\n' o17
declared=$(sed -n 's/.*\[原语 \([0-9]*\) 条 + 符号 \([0-9]*\) 个\].*/\1/p' o17 | head -1)
listed=$(grep -c '^  \[原语:' o17 || true)
[ -n "$declared" ] && [ "$declared" = "$listed" ] \
    && ok "::prims 由原语表生成（表 $declared 条 / 列 $listed 条）" \
    || bad "::prims 与表不一致" "表 $declared / 列 $listed"
miss_prims=0
for p in 前提接线 插词 插词为 回收临时; do
    grep -q "\[原语:$p\]" o17 || { miss_prims=1; echo "        漏了新原语 $p"; }
done
[ "$miss_prims" -eq 0 ] && ok "4 条新原语入口都在 ::prims 里" || bad "新原语没列出来" "$(grep '原语:' o17 | tail -5)"

echo "== ⑧ 推理/思维层按前提决定用不用（R10：前提是闸门，不是路障） =="
# 「人 会 做饭」是模型里已有的知识；把它当前提挂在 猫 与 做饭 之间：
#   人 在语境里 → 用这条连结；不在 → 不用（不是"没知识"，是"现在用不了"）
run '::premhold mention\n::op 加前提 猫 做饭 人
猫 会 做饭 吗
::delib
' o18
grep -q '有前提：人（没成立）' o18 && ok "::delib 说清楚：有前提「人」、没成立，所以不用它" \
    || bad "前提闸门痕迹" "$(grep -E '问前提|前提：' o18 | head -3)"
grep -q '不知道' o18 && ok "关系层答「不知道」（没有硬把这条连结用上）" \
    || bad "不该断言" "$(grep 'AI >' o18 | tail -1)"
run '人 好
::op 加前提 猫 做饭 人
猫 会 做饭 吗
' o19
grep -q '对 ， 猫 -\[前提\]-> 人' o19 && ok "前提「人」在语境里 → 用这条连结，链上带前提这一跳" \
    || bad "前提成立时该用" "$(grep 'AI >' o19 | tail -1)"
# 联想不依赖前提：前提不在语境，A→前提 这条边照样是普通边（可走、有权重）
run '::premhold mention\n::op 加前提 猫 做饭 人
::edges 猫
' o20
grep -qE '→ 人 *_? *hits=[1-9]' o20 && ok "**联想照走**：前提不成立也不挡回忆（A→前提 是普通边）" \
    || bad "联想不该被前提挡住" "$(grep '人' o20 | head -3)"

echo "== ⑨ 反前提（[前提不]）+ 前提说明进答复（A/B 量过 0/8 才翻成默认开） =="
# 反前提：条件是「前提 token 不在注意窗口里」。不提「人」→ 条件成立 → 用这条连结。
run '::premhold mention\n::op 加反前提 猫 做饭 人\n::premise 猫 做饭\n猫 会 做饭 吗\n' o21
grep -q '前提不：人' o21 && ok "::premise 报出极性（前提不：人）" \
    || bad "反前提登记" "$(grep -A2 '前提 token' o21 | head -3)"
grep -q '对 ， 猫 -\[前提不\]-> 人' o21 && ok "人没出现 → 条件成立 → 用这条连结（链上读作 前提不）" \
    || bad "反前提该成立" "$(grep 'AI >' o21 | tail -1)"
# 提到「人」之后 → 条件不成立 → 不用它。
# 下面每条行为断言都**显式写开关**（不依赖环境默认），这样反向测量
# `AIP_PREMISEOUT=0 sh tests/all.sh` 也能整包跑绿；默认值单列一条断言 ——
# 规矩和当年 ::thinkout 那次一样。
run '人 好\n::op 加反前提 猫 做饭 人\n::premiseout on\n猫 会 做饭 吗\n' o22
grep -q '没成立' o22 && ok "人出现了 → 条件不成立" \
    || bad "反前提不该成立" "$(grep 'AI >' o22 | tail -1)"
grep -q '待验：这件事有个前提' o22 && ok "开关打开：回答后面**追加**了前提说明（标成**待验**，不是否定）" \
    || bad "前提说明没进答复" "$(grep 'AI >' o22 | tail -1)"
run '人 好\n::op 加反前提 猫 做饭 人\n::premiseout off\n猫 会 做饭 吗\n' o23
if grep -q '待验：这件事有个前提' o23; then
    bad "关掉之后还在追加" "$(grep 'AI >' o23 | tail -1)"
else
    ok "::premiseout off → 不追加（只留在 ::delib 的痕上）"
fi
grep -q '前提说明进答复 = 关' o23 && ok "开关报出当前状态（关）" \
    || bad "开关状态" "$(grep '前提说明' o23 | head -2)"
if [ "${AIP_PREMISEOUT:-}" = "0" ]; then
    ok "反向测量（AIP_PREMISEOUT=0）：跳过默认值断言"
else
    run '::premiseout\n' o24
    grep -q '前提说明进答复 = 开（默认）' o24 && ok "默认是**开**（A/B 量过 0/8 才翻的）" \
        || bad "默认值" "$(grep '前提说明' o24 | head -2)"
fi

echo "== ⑩ 对话触发：自然语言直接干活（窄集；提问不误触发） =="
run '猫 和 鱼 之间 加 个 词\n::edges 猫\n' o25
grep -q '照做 了' o25 && ok "「猫 和 鱼 之间 加 个 词」→ 真的动手了" \
    || bad "对话触发" "$(grep 'AI >' o25 | tail -1)"
grep -qE '→ \[临:中转\]' o25 && ok "插进去的是**临时 token**（草稿纸，不存盘）" \
    || bad "中转 token" "$(grep '临:' o25 | head -3)"
run '把 猫 和 鱼 之间 加 个 前提 饿\n::premise 猫 鱼\n' o26
grep -q '前提：饿' o26 && ok "「之间 加 个 前提 饿」→ 建出前提（顺手把分词切开的「前/提」也认了）" \
    || bad "前提触发" "$(grep -E '照做|前提' o26 | head -3)"
run '猫 和 鱼 之间 有 什么 关系\n' o27
if grep -q '照做 了' o27; then
    bad "提问被误当成命令" "$(grep 'AI >' o27 | tail -1)"
else
    ok "提问**不**误触发（必须「之间 + 动词 + 种类词」才算命令）"
fi
# 同义说法：动词多收几个（塞/放/搭…），种类词也认「条件」；分词器切开的「词儿」也要认
run '::premhold mention\n猫 和 鱼 之间 塞 个 条件 饿\n::premise 猫 鱼\n' o28b
grep -q '前提：饿' o28b && ok "「之间 塞 个 **条件**」也算加前提（同义说法）" \
    || bad "同义说法（条件）" "$(grep -E '照做|前提' o28b | head -3)"
run '::premhold mention\n猫 和 鱼 之间 搭 个 词儿\n::edges 猫\n' o28c
grep -qE '→ \[临:中转\]' o28c && ok "「搭 个 词儿」→ 中间插词（被切开的「词儿」也认）" \
    || bad "同义说法（词儿）" "$(grep -E '照做|临:' o28c | head -3)"
run '::triggers\n' o28
grep -q '之间 加|插 个 词' o28 && ok "::triggers 列出所有说法（说法归一表只有一处）" \
    || bad "::triggers" "$(grep '触发' o28 | head -3)"

# ---- 说法归一表 + 槽位（新匹配器）：覆盖量出来的 8 处缺口（§6.44；量法 tests/meas_trigger.sh）----
# 断言里**显式写开关**（`::triggers norm on`），所以反向测量 AIP_TRIGGER=kw 也能整包跑绿。
run '::triggers norm on\n猫 和 鱼 当中 加 个 词\n::edges 猫\n' o29
grep -qE '→ \[临:中转\]' o29 && ok "新匹配器认「当中」（老匹配器漏）" \
    || bad "当中" "$(grep -E '照做|临:' o29 | head -3)"
run '::triggers norm on\n猫 和 鱼 之间 堆 个 词\n::edges 猫\n' o30
grep -qE '→ \[临:中转\]' o30 && ok "新匹配器认动词「堆/搁」（说法归一表）" \
    || bad "堆" "$(grep -E '照做|临:' o30 | head -3)"
run '::triggers norm on\n给 猫 和 鱼 之间 加 个 词\n::edges 猫\n' o31
grep -qE '→ \[临:中转\]' o31 && ok "填充词在任意位置也算（「给 … 之间 加 个 词」）" \
    || bad "填充词" "$(grep -E '照做|临:' o31 | head -3)"
run '::triggers norm on\n加 个 词 在 猫 和 鱼 之间\n::edges 猫\n' o32
grep -qE '→ \[临:中转\]' o32 && ok "**动词在前**的语序也认（「加 个 词 在 A 和 B 之间」）" \
    || bad "动词在前" "$(grep -E '照做|临:' o32 | head -3)"
run '::triggers norm on\n猫 和 鱼 之间 加 上 个 词\n::edges 猫\n' o33
if grep -qE '→ \[临:中转\]' o33; then
    ok "动词词尾不当名字（「加 **上** 个 词」插的是临时 token，不是「上」）"
elif grep -q '→ 上' o33; then
    bad "词尾被当名字" "$(grep '→ 上' o33 | head -2)"
else
    bad "加上个词" "$(grep -E '照做|临:' o33 | head -3)"
fi
run '::triggers norm on\n猫 和 鱼 之间 别 加 词\n' o34
if grep -q '照做 了' o34; then
    bad "**否定命令**被当成命令" "$(grep 'AI >' o34 | tail -1)"
else
    ok "否定命令**不动手**（「别 加 词」）—— 老匹配器在这里是误触发"
fi
run '::triggers norm on\n猫 和 鱼 之间 不 要 塞 个 条件\n' o35
if grep -q '照做 了' o35; then
    bad "否定命令（不要）被当成命令" "$(grep 'AI >' o35 | tail -1)"
else
    ok "否定命令（「不 要 塞 个 条件」）不动手"
fi
run '::triggers norm on\n猫 和 鱼 之间 加 个 词 是 什么 意思\n' o36
if grep -q '照做 了' o36; then
    bad "问句被当成命令" "$(grep 'AI >' o36 | tail -1)"
else
    ok "问句不动手（带「什么」的一律不当命令）"
fi
if [ "${AIP_TRIGGER:-}" = "kw" ]; then
    ok "反向测量（AIP_TRIGGER=kw）：跳过默认值断言"
else
    run '::triggers\n' o37
    grep -q '说法归一表 + 槽位（新）' o37 \
        && ok "默认用**新**匹配器（覆盖 25/33 → 33/33 之后才翻的默认）" \
        || bad "默认匹配器" "$(grep '匹配器 =' o37 | head -2)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS tokenop（Token 行为：接线/插词/中转 + 临时 Token 不存盘 + 修正的记忆 + 原语组合 + 前提闸门/极性/进答复/对话触发，$pass 项）"
else
    echo "FAIL tokenop（$fail 项失败，$pass 项通过）"
    exit 1
fi
