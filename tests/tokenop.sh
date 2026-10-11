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

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# `::notepad off`：**钉住"流水线模式"** —— 这样环境变量强制打开"AI 自己写草稿"时
# （`AIP_PATCH=记事本 AIP_NOTEPAD=on`），这套看护的断言依然测的是它要测的东西。
run() { cp model.bin m.bin; { printf '::notepad off\n::rel analogy off\n'; printf '%b' "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 行为表：列得出来，跑得动 =="
run '::op\n' o1
grep -q 'Token 行为（图操作）：15 条' o1 && ok "::op 列出 15 条行为（表生成；过程记号+执行槽那九条在**表尾**）" \
    || bad "行为表" "$(grep -A3 'Token 行为' o1 | head -4)"
for op in 加前提 加反前提 前提接线 中间插词 回收临时 调用补丁 建过程 加步骤 加调用 加回边 加条件 加否则 加异常 加断言 加执行; do
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
echo "== ⑨ 过程记号（第 9 条 ①②）：过程边与共现边靠 role 分开 =="
# 用户 2026-10-10 拍板：过程边用 **role 标记**（四选一里的那一个）。
# 这一节钉：① 默认没有过程（::proc 给的是提示，不是空的成功）；② `::op` 建过程/加步骤
# 走的是**受审通道**（输出里有"应用 N 条"）；③ 记号与 role 都真的落上了（::proc 打出来）。
run '::proc\n' o9
grep -q '还没有过程' o9 && ok "默认没有过程（::proc 给提示 —— 既有跑法一个字不变）" \
    || bad "::proc 默认" "$(head -3 o9)"
run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 泡茶叶\n::op 加调用 泡茶 洗杯子\n::op 加回边 泡茶 1\n::proc 泡茶\n' o10
grep -q '应用 1 条' o10 && ok "建过程/加步骤/加调用/加回边都走**受审通道**（报告「应用 N 条」）" \
    || bad "受审通道" "$(grep -m2 '行为' o10)"
grep -q '2 步 + 0 条件 + 0 否则 + 0 异常 + 1 调用 + 1 回边' o10 && ok "过程结构对：2 步 + 1 调用 + 1 回边" \
    || bad "过程结构" "$(grep -m1 '过程:泡茶' o10)"
grep -q '第 1 步 烧水' o10 && grep -q '第 2 步 泡茶叶' o10 \
    && ok "步骤**按步号排**打出来（顺序写在记号上，不靠边序）" \
    || bad "步骤顺序" "$(grep '步 ' o10 | head -3)"
grep -q '记号 \[步骤:1\]，角色=过程' o10 \
    && ok "动作槽挂的是「步骤:N」，边的 role 是**过程**（记号 + role 两样都在）" \
    || bad "记号/role" "$(grep -m2 '角色=' o10)"
grep -q '回边 →  \[步骤:1\]' o10 && ok "回边指回**第 1 步**（给步号：「加回边 泡茶 1」）" \
    || bad "回边" "$(grep -m1 '回边' o10)"
grep -q '另有 1 条\*\*不是过程边\*\*的边' o10 && ok "同一张图上**过程边与普通共现边分得开**（挂靠边被单独标出来）" \
    || bad "过程/共现分开" "$(grep -c '角色=' o10)"
run '::op 加步骤 没有的过程 烧水\n' o11
grep -q '没有这个过程' o11 && ok "给不存在的过程加步骤：明说没有（不静默建）" \
    || bad "不存在的过程" "$(head -3 o11)"
run '::proc 泡茶\n' o12
grep -q '没有这个过程：泡茶' o12 && ok "过程**不跨会话**（没建过的会话里 ::proc 说没有 —— 记号在内存/存盘里，按需重建）" \
    || bad "::proc 空会话" "$(head -2 o12)"


echo "== ⑩ 过程执行引擎（第 9 条 ③，默认关）：逐步核对 + 受审执行 + 断点 =="
# 用户拍板 role 方案后 ①+② 落地（§6.70）；这一节钉 ③：**跑得起来、也停得下来**。
# ⚠ 反向测量（AIP_PROCRUN=1）时"默认关"这条断言的**方向反过来**（按规矩分叉）：
# 那时它本来就是开的，要钉的是"env 一开、不用 ::proc on 就能跑"。
if [ -z "${AIP_PROCRUN:-}" ]; then
    run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::proc run 泡茶\n' o13
    grep -q '过程执行引擎关着' o13 && ok "引擎**默认关**：run 只回一句「关着」（既有行为一个字不动）" \
        || bad "引擎默认关" "$(grep -m2 'proc\|过程' o13)"
else
    run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::proc run 泡茶\n' o13
    grep -q '跑过程' o13 && ok "AIP_PROCRUN=1（反向测量）：env 一开就能跑（无需 ::proc on）" \
        || bad "反向测量" "$(grep -m2 'proc\|过程' o13)"
fi
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::proc run 泡茶\n' o14
grep -q '第 1 步 烧水' o14 && grep -q '词在 + 前提成立' o14 \
    && ok "普通步骤：核到"词在 + 前提成立"就过（做不做得了**明说不判**）" \
    || bad "普通步骤" "$(grep -m3 '步 ' o14)"
grep -q '执行了行为「回收临时」（受审通道）' o14 \
    && ok "是 Token 行为的步骤：走**受审通道真执行**（有审查/预算/账）" \
    || bad "受审执行" "$(grep -m2 '回收临时' o14)"
grep -q '汇总：2 步' o14 && grep -q '轨迹（复用 Derivation）' o14 \
    && ok "打汇总 + 轨迹（复用 Derivation：goal + 每步 + conclude）" \
    || bad "汇总/轨迹" "$(grep -m2 '汇总\|轨迹' o14)"
# 要参数的行为：**不猜参数**，记断点
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 加步骤\n::proc run 泡茶\n' o15
grep -q '要参数（v1 不支持，不猜）' o15 && ok "要参数的行为步骤：记断点**不猜参数**" \
    || bad "不猜参数" "$(grep -m2 '加步骤' o15)"
# 前提不成立 = 断点（跨会话：前提不在注意窗口里）
{ printf '::notepad off\n::rel analogy off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加前提 [过程:泡茶] 烧水 有热水\n::save proc.bin\n::quit\n'; } \
    | ./aip --model model.bin > /dev/null 2>&1 || true
{ printf '::notepad off\n::rel analogy off\n::proc on\n::proc dry off\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc.bin > o16 2>&1 || true
grep -q '断点：前提「有热水」没成立' o16 \
    && ok "前提没成立 ⇒ **断点**（用现成的三档判据，不是新拍的）" \
    || bad "前提断点" "$(grep -m3 '步 \|前提' o16)"
grep -q '不许说跑通了' o16 && ok "有断点时**明说"不许说跑通了"**（断点在哪停在哪）" \
    || bad "断点措辞" "$(grep -m1 '轨迹' o16)"
# 过程边是**结构**：加前提不许把它拆掉（拆了过程就少一步）
{ printf '::notepad off\n::rel analogy off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::op 加前提 [过程:泡茶] 烧水 有热水\n::proc 泡茶\n::quit\n'; } \
    | ./aip --model model.bin > o17 2>&1 || true
grep -q '2 步 + 0 条件 + 0 否则 + 0 异常 + 0 调用 + 0 回边' o17 \
    && ok "给一步挂前提之后，**步骤还是 2 步**（过程边是结构，插词/加前提不许拆）" \
    || bad "过程边被拆" "$(grep -m2 '过程:泡茶' o17)"
# 调用 + 回边：递归跑子过程、回边按上限重跑
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 建过程 洗杯子\n::op 加步骤 洗杯子 冲水\n::op 加调用 泡茶 洗杯子\n::op 加回边 泡茶 1\n::proc run 泡茶\n' o18
grep -q '调用 \[过程:洗杯子\]' o18 && ok "「调用」→ **递归跑子过程**（子过程自己一份步数）" \
    || bad "调用" "$(grep -m3 '调用' o18)"
n_back=$(grep -ac '（重跑）' o18 || true)
[ "$n_back" -eq 2 ] && ok "「回边」跳回第 1 步并按**上限 2 次**重跑（实测 $n_back 次）" \
    || bad "回边上限" "重跑 $n_back 次"


echo "== ⑪ ④ 干跑（假设世界）：假定前提 + 按步回退 + 跑完 pop 干净 =="
# §6.56 第 4 条：`Hypo` 当执行栈与作用域，回退用 pop。这一节钉干跑与实跑的区别、回退、
# 以及"跑完一个字都不漏"（假设层深度回到原样）。
{ printf '::notepad off\n::rel analogy off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::op 加前提 [过程:泡茶] 烧水 有热水\n::save proc.bin\n::quit\n'; } \
    | ./aip --model model.bin > /dev/null 2>&1 || true
if [ -z "${AIP_PROCDRY:-}" ]; then
    run '::proc dry\n' o19
    grep -q '过程\*\*干跑\*\* = 关（默认）' o19 && ok "干跑**默认关**（实跑 = ③ 的语义，一个字不变）" \
        || bad "干跑默认" "$(grep -m2 '干跑' o19)"
else
    run '::proc dry\n' o19
    grep -q '过程\*\*干跑\*\* = 开' o19 && ok "AIP_PROCDRY=1（反向测量）：env 一开就是干跑" \
        || bad "干跑反测" "$(grep -m2 '干跑' o19)"
fi
{ printf '::notepad off\n::rel analogy off\n::proc on\n::proc dry on\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc.bin > o20 2>&1 || true   # 显式开：不受 env 影响
grep -q '干跑：\*\*过\*\*（假定前提「有热水」成立' o20 \
    && ok "干跑：前提不成立 → **假定成立**（记进假设层：烧水 需要 有热水）" \
    || bad "假定前提" "$(grep -m3 '步 \|假定' o20)"
grep -q '行为「回收临时」\*\*假定能做，不执行\*\*' o20 \
    && ok "干跑：行为步骤**不执行**（只记假定能做 —— 与实跑的区别就在这）" \
    || bad "干跑不执行" "$(grep -m2 '回收临时' o20)"
grep -q '真执行 0 个行为' o20 && ok "干跑这一轮**一个行为都没执行**（汇总也这么报）" \
    || bad "干跑计数" "$(grep -m1 '汇总' o20)"
grep -q '深度 0 → 0（跑完回到原样，一个字都没漏）' o20 \
    && ok "跑完假设层**回到原深度**（pop 干净，不漏）" \
    || bad "pop 干净" "$(grep -m1 '假设层' o20)"
grep -q '跑完核对 \*\*1 条还在层里\*\*（应当 1' o20 \
    && ok "假定进了层、跑完核对还在（「hypo_find」独立核过）" \
    || bad "假定核对" "$(grep -m1 '假定（' o20)"
# 按步回退：先假定了前提、随后发现这一步做不了（要参数）⇒ 撤掉那一步的假设
{ printf '::notepad off\n::rel analogy off\n::op 建过程 泡茶\n::op 加步骤 泡茶 加步骤\n::op 加前提 [过程:泡茶] 加步骤 有热水\n::save proc4.bin\n::quit\n'; } \
    | ./aip --model model.bin > /dev/null 2>&1 || true
{ printf '::notepad off\n::rel analogy off\n::proc on\n::proc dry on\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc4.bin > o21 2>&1 || true
grep -q '回退：这一步的假设撤了' o21 && ok "**按步回退**：这一步的假设撤了（假设层回到这一步之前）" \
    || bad "按步回退" "$(grep -m3 '回退\|假定' o21)"
grep -q '回退撤掉 1 条；跑完核对 \*\*0 条还在层里\*\*（应当 0' o21 \
    && ok "回退的账对得上（假定 1 条、撤 1 条、核对 0 条）" \
    || bad "回退账" "$(grep -m1 '假定（' o21)"
# 实跑不受影响：前提不成立仍是断点、行为仍真执行
{ printf '::notepad off\n::rel analogy off\n::proc on\n::proc dry off\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc.bin > o22 2>&1 || true
grep -q '断点：前提「有热水」没成立' o22 && grep -q '执行了行为「回收临时」' o22 \
    && ok "干跑关着时回到实跑：前提不成立=断点、行为真执行（③ 语义没变）" \
    || bad "实跑不变" "$(grep -m3 '步 \|执行了' o22)"


echo "== ⑫ ⑤ 分支/循环/异常：条件走哪一支、出错跳哪、循环什么时候停 =="
# §6.56 五步走的最后一步。条件判据**不另造**：就是前提那三档（`ai_premise_holds`）。
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加条件 泡茶 猫 泡茶叶\n::op 加否则 泡茶 去烧水\n猫 有 毛 吗\n::proc run 泡茶\n' o23
grep -q '条件「猫」\*\*成立\*\* → 走它那一支 泡茶叶' o23 \
    && ok "条件成立：走条件那一支（判据就是前提那三档）" \
    || bad "条件成立" "$(grep -m3 '条件\|步 ' o23)"
grep -q '条件分支 泡茶叶' o23 && ok "条件那一支**真的跑了**（按步核过：词在 + 前提成立）" \
    || bad "条件分支跑" "$(grep -m2 '泡茶叶' o23)"
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加条件 泡茶 鱼 泡茶叶\n::op 加否则 泡茶 去烧水\n猫 有 毛 吗\n::proc run 泡茶\n' o24
grep -q '条件「鱼」\*\*不成立\*\* → 走 「\[否则\]」 那一支 去烧水' o24 \
    && ok "条件不成立：走 `[否则]` 那一支" \
    || bad "否则支" "$(grep -m3 '条件\|否则' o24)"
grep -q '否则分支 去烧水' o24 && ok "否则那一支**真的跑了**" \
    || bad "否则分支跑" "$(grep -m2 '去烧水' o24)"
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加条件 泡茶 有个没这个词 泡茶叶\n::proc run 泡茶\n' o25
grep -q '图里没有这个词 ⇒ \*\*判不了\*\*（按不成立处理）' o25 \
    && ok "条件词图里没有 ⇒ **明说判不了**（按不成立处理，不静默当成立）" \
    || bad "判不了" "$(grep -m2 '条件' o25)"
grep -q '跳过（不当成断点）' o25 && ok "没有否则支时：**跳过**（不当断点 —— 没走这条不算错）" \
    || bad "跳过" "$(grep -m2 '跳过\|汇总' o25)"
# 异常：哪一步失败就跑处理步，而且**账照旧记**（不假装没发生）
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 加步骤\n::op 加异常 泡茶 没参数 用微波炉\n::proc run 泡茶\n' o26
grep -q '出错了 → 跑 「\[异常:没参数\]」 那一支 用微波炉' o26 \
    && ok "出错 → 跑 `[异常:类型]` 那一支" \
    || bad "异常处理" "$(grep -m3 '断点\|异常' o26)"
grep -q '账照旧记' o26 && grep -q '1 个断点' o26 \
    && ok "处理步过了也**不抹掉出错那一步的账**（断点仍然算）" \
    || bad "异常账" "$(grep -m2 '断点\|处理步' o26)"
# 循环：回边 + 上限（③）；带条件的回边 = while（每轮先看条件）
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加回边 泡茶 1 猫\n猫 有 毛 吗\n::proc run 泡茶\n' o27
grep -q '循环条件「猫」成立 ⇒ 按上限 2 次重跑，每轮再看一次' o27 \
    && ok "带条件的回边 = **while**（每轮先看条件）" \
    || bad "while" "$(grep -m2 '回边 →' o27)"
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加回边 泡茶 1 鱼\n猫 有 毛 吗\n::proc run 泡茶\n' o28
grep -q '循环条件「鱼」\*\*不成立\*\* ⇒ 跳出循环（一次都不重跑）' o28 \
    && ok "条件一开就不成立：**一次都不重跑**（while 的正常出口）" \
    || bad "while 出口" "$(grep -m2 '回边 →' o28)"
grep -q '跳过 1 次' o28 && ok "⑤ 的账单独报（跳过/异常处理各几次）" \
    || bad "⑤ 账" "$(grep -m1 '⑤' o28)"


echo "== ⑬ ① 步骤带对象：跑的时候 `rel_solve` + `der_verify` **独立复核** =="
run '::proc on\n::proc dry off\n::op 建过程 核对\n::op 加断言 核对 猫 属于 动物\n::proc run 核对\n' o29
grep -q '\*\*独立复核过\*\*（要核「猫 属于 动物」，链 2 步、缺口 0）' o29 \
    && ok "带对象的步骤：**独立复核过**（求解器推 + 验证器复核，链长也报出来）" \
    || bad "独立复核" "$(grep -m3 '步 \|复核' o29)"
grep -q '① 步骤带对象：.*独立复核通过 1 个 / 不过 0 个' o29 && ok "复核结果进汇总（通过/不过各几个）" \
    || bad "复核账" "$(grep -m1 '①' o29)"
run '::proc on\n::proc dry off\n::op 建过程 核对\n::op 加断言 核对 猫 属于 鸟\n::proc run 核对\n' o30
grep -q '\*\*复核不过\*\*（要核「猫 属于 鸟」' o30 && grep -q '1 个断点' o30 \
    && ok "核不过 ⇒ **断点**（不是"反正词在就放行"）" \
    || bad "核不过" "$(grep -m3 '步 \|汇总' o30)"
run '::proc on\n::proc dry off\n::op 建过程 核对\n::op 加断言 核对 猫 属于 动物 不\n::proc run 核对\n' o31
grep -q '要核「不 猫 属于 动物」' o31 && grep -q '复核不过' o31 \
    && ok "`不` 那一档也认（要它**不**成立 —— 图上成立所以核不过，正好证明确实按否定核的）" \
    || bad "否定断言" "$(grep -m2 '步 \|复核' o31)"
run '::op 加断言 核对 猫 属于 动物\n' o32
grep -q '参数不对' o32 || grep -q '没有这个过程' o32 \
    && ok "CLI 参数上限修好了（`加断言` 要 4 个参数，原来 `args[4]` 只给 3 个）" \
    || bad "参数上限" "$(head -3 o32)"


echo "== ⑭ ② 过程记号接进对话（默认关）：说话就能建/加/跑 =="
if [ -z "${AIP_PROCTALK:-}" ]; then
    run '记个过程 泡茶\n' o33
    grep -aq '记下 了 （ \[过程:泡茶\] ' o33 && bad "默认关不该认" "$(grep -m1 'AI >' o33)" \
        || ok "默认关：这些话照走流水线（一句都不认）"
    run '::proc talk\n' o34
    grep -q '过程记号接进对话 = 关（默认）' o34 && ok "::proc talk 自报默认关" || bad "talk 默认" "$(head -2 o34)"
else
    ok "AIP_PROCTALK=1（反向测量）：默认值断言按规矩跳过"
fi
run '::proc talk on\n记个过程 泡茶\n第 1 步 烧水\n第 2 步 泡茶叶\n::proc 泡茶\n' o35
grep -aq '过程 「 泡茶 」 记下 了' o35 && ok "「记个过程 X」→ 建过程（名字拼回去：分词器把「泡茶」切开了）" \
    || bad "记个过程" "$(grep -m2 'AI >' o35)"
grep -aq '第 1 步 是 「 烧水 」' o35 && grep -aq '第 2 步 是 「 泡茶叶 」' o35 \
    && ok "「第 N 步 动作」→ 加步骤（步号认得出、动作拼回去）" || bad "第N步" "$(grep -m3 'AI >' o35)"
grep -q '2 步 + 0 条件' o35 && ok "说出来的过程**真的建起来了**（::proc 看得到 2 步）" \
    || bad "建起来了" "$(grep -m1 '过程]' o35)"
# `::proc off`：这一条测的是"引擎关着时怎么答"——显式钉住（反向测量 AIP_PROCRUN=1 时也要关）
run '::proc talk on\n::proc off\n记个过程 泡茶\n第 1 步 烧水\n跑 泡茶\n' o36
grep -aq '执行 引擎 关着' o36 && ok "引擎关着时：「跑 X」明说关着（不假装跑了）" \
    || bad "引擎关着" "$(grep -m2 'AI >' o36)"
run '::proc talk on\n::proc on\n记个过程 泡茶\n第 1 步 烧水\n跑 泡茶\n' o37
grep -aq '跑 完 了 「 泡茶 」' o37 && ok "引擎开着时：「跑 X」**真的跑一遍**（并说清有没有断点）" \
    || bad "跑一遍" "$(grep -m2 'AI >' o37)"
grep -aq "应用 1 条" o37 && ok "对话入口走的还是**受审通道**（报告「应用 N 条」）" \
    || bad "受审通道" "$(grep -m1 '记下 了' o37)"


echo "== ⑮ ② 过程接进思维（默认关、只读）：思考收尾自己认「这事我记过过程」 =="
if [ -z "${AIP_PROCTHINK:-}" ]; then
    run '::proc think off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做 呢\n::delib\n' o38
    n=$(grep -ac '这我记过过程' o38 || true)
    [ "$n" -eq 0 ] && ok "默认关：思考里**一个字都不加**" || bad "默认关" "$(grep -m1 '记过过程' o38)"
    run '::proc think\n' o39
    grep -q '过程接进思维 = 关（默认）' o39 && ok "::proc think 自报默认关" || bad "think 默认" "$(head -2 o39)"
else
    ok "AIP_PROCTHINK=1（反向测量）：默认值断言按规矩跳过"
fi
run '::proc think on\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做 呢\n::delib\n' o40
grep -q '这我记过过程' o40 && grep -q '这件事我记过过程「泡茶」（1 步）' o40 \
    && ok "开着：思考收尾认出来了，**写进痕**（追问的是一件事、答的是"我记过过程"，不是事实）" \
    || bad "记过过程" "$(grep -m3 '记过\|过程]' o40)"
run '::proc think on\n::proc on\n::proc dry on\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做 呢\n::delib\n' o41
grep -q '干跑一遍：每一步都过得去' o41 \
    && ok "引擎+干跑开着时：顺手**干跑一遍**（只读 —— 不执行行为、假定只在假设层）并写进痕" \
    || bad "干跑进痕" "$(grep -m2 '记过过程' o41)"
run '::proc think on\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n猫 有 毛 吗\n::delib\n' o42
n=$(grep -ac '这我记过过程' o42 || true)
[ "$n" -eq 0 ] && ok "问的是别的事（猫有毛吗）→ **不硬扯**（没命中就不说）" \
    || bad "别的事也硬扯" "$(grep -m1 '记过过程' o42)"


echo "== ⑯ ② 过程接进决策（默认关）：问「怎么做 X」直接用计划回答 =="
if [ -z "${AIP_PROCPLAN:-}" ]; then
    run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做\n' o43
    n=$(printf '%s\n' "$(cat o43)" | grep -ac '我 记过 过程' || true)
    [ "$n" -eq 0 ] && ok "默认关：照旧走流水线（不拿计划当回答）" || bad "默认关" "$(grep -m1 '记过 过程' o43)"
    run '::proc plan\n' o44
    grep -q '过程接进决策 = 关（默认）' o44 && ok "::proc plan 自报默认关" || bad "plan 默认" "$(head -2 o44)"
else
    ok "AIP_PROCPLAN=1（反向测量）：默认值断言按规矩跳过"
fi
run '::proc plan on\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 泡茶叶\n::op 建过程 洗杯子\n::op 加步骤 洗杯子 冲水\n::op 加调用 泡茶 洗杯子\n泡茶 怎么 做\n' o45
grep -q '我 记过 过程 「 泡茶 」 ： 第 1 步 烧水' o45 && grep -q '第 2 步 泡茶叶' o45 \
    && ok "开着：**计划就是回答**（步骤按号列出来）" || bad "计划回答" "$(grep -m2 'AI >' o45)"
grep -q '调用 \[过程:洗杯子\]' o45 && ok "调用/条件/异常这些也一并列出来" \
    || bad "其它记号" "$(grep -m1 'AI >' o45)"
grep -q '执行 引擎 关着 ， 只 给 计划' o45 && ok "引擎关着时**明说只给计划**（不假装跑过）" \
    || bad "引擎关着" "$(grep -m1 'AI >' o45)"
run '::proc plan on\n::proc on\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做\n' o46
grep -q '干跑 一遍 ： 每 一步 都 过 得 去' o46 \
    && ok "引擎开着时附**干跑结论**（只读；结论进回答、过程不执行）" || bad "干跑结论" "$(grep -m1 'AI >' o46)"
run '::proc plan on\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n猫 怎么 做\n' o47
n=$(printf '%s\n' "$(cat o47)" | grep -ac '我 记过 过程' || true)
[ "$n" -eq 0 ] && ok "没记过那个过程（猫）→ **不硬扯**计划" || bad "硬扯" "$(grep -m1 '记过 过程' o47)"


echo "== ⑰ ① 解释器补观察遍 + 语料行校验（§6.80） =="
run '::asm 甲 乙 丙 连接\n::edges 甲\n' o48
grep -aq '→ 乙' o48 && ok "`::asm` 也走**观察遍**了：`甲 乙 丙 连接` 里 甲→乙 那条共现边不再丢（老 bug）" \
    || bad "解释器观察遍" "$(grep -m3 '→' o48)"
run '::asm check on\n::asm [原语:如果] 甲\n' o49
grep -aq '校验\] 没过' o49 && ok "`::asm check on` 时坏程序**不执行**（图一个字都不动）" \
    || bad "校验不执行" "$(grep -m2 '校验' o49)"
printf '[原语:如果] 甲\n' > bad_corpus_test.txt
out=$(BIN=$BIN sh -c '"$BIN" --model model.bin bad_corpus_test.txt <<< "::quit"' 2>&1 || true)
rm -f bad_corpus_test.txt
if [ -n "${AIP_ASMCHECK:-}" ]; then
    printf '%s\n' "$out" | grep -aq '语料校验' && ok "语料行校验：坏行**不执行**并报出来" \
        || bad "语料行校验" "$(printf '%s\n' "$out" | head -2)"
else
    ok "语料行校验默认关（AIP_ASMCHECK=1 时才有那行报告）"
fi

echo "== ⑱ ① 动作 token + 执行槽：步骤会真的调动作（三条路都验） =="
run '::proc on\n::proc dry on\n::op 建过程 试跑\n::op 加执行 试跑 echo 你好\n::proc run 试跑\n' o50
grep -aq '干跑：动作\*\*不执行\*\*' o50 && ok "干跑：动作**不执行**（只核过，不动外部世界）" \
    || bad "干跑不执行" "$(grep -m2 '执行' o50)"
run '::proc on\n::proc dry off\n::op 建过程 试跑\n::op 加执行 试跑 echo 你好\n::proc run 试跑\n' o51
grep -aq '外部执行层\*\*关着\*\*' o51 && grep -aq '不许假装做过' o51 \
    && ok "执行层关着 ⇒ **断点**（"假装做过"是最坏的）" || bad "执行层关着" "$(grep -m2 '执行' o51)"
run '::act on\n::proc on\n::proc dry off\n::op 建过程 试跑\n::op 加执行 试跑 echo 你好\n::proc run 试跑\n' o52
grep -aq '\*\*真执行了\*\*：echo 你好' o52 && ok "开着 ⇒ **真执行**（走 action_run：白名单+超时+账）" \
    || bad "真执行" "$(grep -m3 '执行\|汇总' o52)"
grep -aq '真执行 1 个行为' o52 && ok "汇总里记着"真执行 1 个行为"" \
    || bad "执行计数" "$(grep -m1 '汇总' o52)"
run '::op 建过程 试跑\n::op 加执行 试跑 echo|x\n' o53
grep -aq '白名单\|命令名' o53 && ok "带管道的动作词当场拒（别把 shell 语法塞进图里）" \
    || bad "拒 shell 语法" "$(grep -m2 '没跑成\|加执行' o53)"

echo "== ⑲ ② 求值层样板：条件里的数值比较真算一遍 =="
# 显式 `::proc eval off`：这一条测的是"默认关时的行为"，反向测量（AIP_EVALNUM=1）下也要关
run '::proc on\n::proc dry off\n::proc eval off\n::op 建过程 比\n::op 加条件 比 3<5 走这支\n::op 加否则 比 走那支\n::proc run 比\n' o54
grep -aq '图里没有这个词 ⇒ \*\*判不了\*\*' o54 && ok "默认关：数值条件照旧**判不了**（回退图内判据，行为不变）" \
    || bad "默认关" "$(grep -m2 '条件' o54)"
run '::proc on\n::proc dry off\n::proc eval on\n::op 建过程 比\n::op 加条件 比 3<5 走这支\n::op 加否则 比 走那支\n::proc run 比\n' o55
grep -aq '\*\*求值层算了一遍\*\*（数值比较）⇒ 成立' o55 && grep -aq '条件分支 走这支' o55 \
    && ok "开着：`3<5` **真算一遍** ⇒ 成立 ⇒ 走条件那一支" || bad "真算成立" "$(grep -m3 '条件\|分支' o55)"
run '::proc on\n::proc dry off\n::proc eval on\n::op 建过程 比\n::op 加条件 比 5<3 走这支\n::op 加否则 比 走那支\n::proc run 比\n' o56
grep -aq '⇒ 不成立' o56 && grep -aq '否则分支 走那支' o56 \
    && ok "`5<3` ⇒ 不成立 ⇒ 走否则支（真值说了算，不是"词在就成立"）" || bad "真算不成立" "$(grep -m3 '条件\|分支' o56)"
run '::proc on\n::proc dry off\n::proc eval on\n::op 建过程 比\n::op 加条件 比 三<五 走这支\n::op 加否则 比 走那支\n::proc run 比\n' o57
grep -aq '判不了' o57 && ok "认不出形状（`三<五`）⇒ **判不了**，绝不默认成立" \
    || bad "判不了" "$(grep -m2 '条件' o57)"

echo "== ⑳ ② 词表覆盖：lexicon.txt 让池里没有的多字词按整词切 =="
rm -f lexicon.txt
# ⚠ "切开 vs 整词"的**机械判据在 C 级**（optest ㉟：ai_tokenize 数 token）——
#    这里原想用"学不学得成"当判据，实测**两种切法都学得成**（断言前提是错的），所以只留正向那条。
printf '泡茶\n' > lexicon.txt
run '猫 属于 泡茶\n' o59
grep -aq '记下 了 ： 猫 属于 泡茶' o59 \
    && ok "有词表时：**整词切** ⇒ 同一句话学得成（数据驱动，不动算法）" \
    || bad "整词切" "$(grep -m2 "AI >" o59)"
rm -f lexicon.txt

echo "== ㉑ 特征集合 → 概念（只读；答的是「像什么」不是「是什么」） =="
run '::feat 毛\n' o60
grep -aq '像是什么' o60 && grep -aq '动物' o60 \
    && ok "`::feat 毛` ⇒ 投出「动物」（只投**关系边**：出厂模型里 动物—有→毛 才是关系边）" \
    || bad "特征投票" "$(grep -m3 '像是\|命中' o60)"
grep -aq '这是\*\*联想\*\*' o60 && ok "措辞明说这是**联想**，不是判定" \
    || bad "措辞" "$(grep -m1 '联想' o60)"
run '::feat 没这个词\n' o61
grep -aq '不在图上' o61 && ok "没在图上 ⇒ 如实说（不瞎投）" || bad "不在图上" "$(head -2 o61)"

if [ "$fail" -eq 0 ]; then
    echo "PASS tokenop（Token 行为：接线/插词/中转 + 临时 Token 不存盘 + 修正的记忆 + 原语组合 + 前提闸门/极性/进答复/对话触发 + 过程记号 + 过程执行引擎 + 干跑（假设世界）+ 分支/循环/异常 + 步骤带对象 + 对话入口 + 接进思维 + 接进决策 + 解释器观察遍 + 动作执行槽 + 求值层样板 + 词表覆盖 + 特征投票，$pass 项）"
else
    echo "FAIL tokenop（$fail 项失败，$pass 项通过）"
    exit 1
fi
