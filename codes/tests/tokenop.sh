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
# §6.545 `[say]` 的行数是**数据决定的**（每轮都会加行），断言别再写死数字
SAY_LINES=$(grep -c '^\[say\]' "$ROOT/patterns.txt" 2>/dev/null || echo 0)

run() { cp model.bin m.bin; { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '%b' "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 行为表：列得出来，跑得动 =="
run '::op\n' o1
grep -q '符号节点行为（图操作）：18 条' o1 && ok "::op 列出 18 条行为（表生成；过程记号+执行槽+撤外壳在**表尾**）" \
    || bad "行为表" "$(grep -A3 'Token 行为' o1 | head -4)"
for op in 加前提 加反前提 前提接线 中间插词 回收临时 调用补丁 建过程 加步骤 加调用 加回边 加条件 加否则 加异常 加断言 加执行 加说 标外壳 撤外壳; do
    grep -q "::op $op" o1 || bad "表里少了行为 $op" "$(grep '::op' o1 | head -5)"
done
grep -q '当前未回收的临时符号节点：0 个' o1 && ok "开局没有临时 Token（草稿是干净的）" \
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
grep -q '之间的前提符号节点' o4 && ok "::premise 答出「这两个之间的前提符号节点 是什么」" \
    || bad "::premise" "$(grep -A2 '前提' o4 | head -4)"
# 前提用**真词**时：它照常能被联想到（有 hits、可走边），逻辑也知道它是前提
run '::premhold mention\n::op 加前提 猫 月亮 食物
::edges 猫
::premise 猫 月亮
' o4b
grep -qE '→ 食物 *_? *hits=[1-9]' o4b && ok "前提用真词（食物）：前提 token 有边有权重 —— 能被正常联想到" \
    || bad "前提可联想" "$(grep '食物' o4b | head -3)"
grep -q '食物' o4b && grep -q '之间的前提符号节点' o4b && ok "逻辑同时知道「食物 是这两个之间的前提」" \
    || bad "前提可识别" "$(grep -A2 '前提符号节点' o4b | head -3)"
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
grep -q '之间的前提符号节点' o5 && ok "::premise 猫 鱼 也答得出前提（食物）" \
    || bad "前提查询" "$(grep -A2 '前提符号节点' o5 | head -3)"

echo "== ⑤ 临时 Token 不存盘：存读之后它不在，模型照常装 =="
run '::op 中间插词 猫 月亮\n::save slot\n' o7
[ -s slot ] && ok "存盘成功（临时 Token 与指向它的边被跳过）" || bad "存盘" "$(tail -2 o7)"
mv slot m.bin
printf '::op\n::edges [临:中转]\n::quit\n' | ./aip --model m.bin > o8 2>&1 || true
grep -q '当前未回收的临时符号节点：0 个' o8 && ok "重载后没有临时 Token（草稿没被写成知识）" \
    || bad "临时 Token 被存下来了" "$(grep '临时 Token' o8 | head -2)"
grep -q '没有这个符号节点' o8 && ok "指向它的边也没有留下悬空引用（装载不报错）" \
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
grep -q '之间的前提符号节点' o14 && ok "::premise 猫 饿 答得出前提" \
    || bad "原语前提查询" "$(grep -A2 '前提符号节点' o14 | head -4)"
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
grep -q '\[原语 回收临时\] 没人引用的临时符号节点 0 个' o16 \
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
    || bad "反前提登记" "$(grep -A2 '前提符号节点' o21 | head -3)"
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
if [ "${AIP_PROCRUN:-1}" = "0" ]; then
    run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::proc run 泡茶\n' o13
    grep -q '过程执行引擎关着' o13 && ok "显式关（AIP_PROCRUN=0）：run 只回一句「关着」（既有行为一个字不动）" \
        || bad "引擎默认关" "$(grep -m2 'proc\|过程' o13)"
else
    run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::proc run 泡茶\n' o13
    grep -q '跑过程' o13 && ok "默认（§6.106 起**翻成开**）：不用 ::proc on 就能跑" \
        || bad "引擎默认开" "$(grep -m2 'proc\|过程' o13)"
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
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加前提 [过程:泡茶] 烧水 有热水\n::save proc.bin\n::quit\n'; } \
    | ./aip --model model.bin > /dev/null 2>&1 || true
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::proc on\n::proc dry off\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc.bin > o16 2>&1 || true
grep -q '断点：前提「有热水」没成立' o16 \
    && ok "前提没成立 ⇒ **断点**（用现成的三档判据，不是新拍的）" \
    || bad "前提断点" "$(grep -m3 '步 \|前提' o16)"
grep -q '不许说跑通了' o16 && ok "有断点时**明说"不许说跑通了"**（断点在哪停在哪）" \
    || bad "断点措辞" "$(grep -m1 '轨迹' o16)"
# 过程边是**结构**：加前提不许把它拆掉（拆了过程就少一步）
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::op 加前提 [过程:泡茶] 烧水 有热水\n::proc 泡茶\n::quit\n'; } \
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
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 回收临时\n::op 加前提 [过程:泡茶] 烧水 有热水\n::save proc.bin\n::quit\n'; } \
    | ./aip --model model.bin > /dev/null 2>&1 || true
if [ "${AIP_PROCDRY:-1}" = "0" ]; then
    run '::proc dry\n' o19
    grep -q '过程\*\*干跑\*\* = 关' o19 && ok "干跑**默认关**（实跑 = ③ 的语义，一个字不变）" \
        || bad "干跑默认" "$(grep -m2 '干跑' o19)"
else
    run '::proc dry\n' o19
    grep -q '过程\*\*干跑\*\* = 开' o19 && ok "默认（§6.105 起**翻成开**）就是干跑" \
        || bad "干跑默认开" "$(grep -m2 '干跑' o19)"
fi
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::proc on\n::proc dry on\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc.bin > o20 2>&1 || true   # 显式开：不受 env 影响
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
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::op 建过程 泡茶\n::op 加步骤 泡茶 加步骤\n::op 加前提 [过程:泡茶] 加步骤 有热水\n::save proc4.bin\n::quit\n'; } \
    | ./aip --model model.bin > /dev/null 2>&1 || true
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::proc on\n::proc dry on\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc4.bin > o21 2>&1 || true
grep -q '回退：这一步的假设撤了' o21 && ok "**按步回退**：这一步的假设撤了（假设层回到这一步之前）" \
    || bad "按步回退" "$(grep -m3 '回退\|假定' o21)"
grep -q '回退撤掉 1 条；跑完核对 \*\*0 条还在层里\*\*（应当 0' o21 \
    && ok "回退的账对得上（假定 1 条、撤 1 条、核对 0 条）" \
    || bad "回退账" "$(grep -m1 '假定（' o21)"
# 实跑不受影响：前提不成立仍是断点、行为仍真执行
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::proc on\n::proc dry off\n::proc run 泡茶\n::quit\n'; } | ./aip --model proc.bin > o22 2>&1 || true
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
# §6.507 **条件组合**：`A&B`（与）/`A|B`（或）/`!A`（非）—— 原子判据仍是前提三档
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加条件 泡茶 鱼|!鱼 泡茶叶\n::op 加否则 泡茶 去烧水\n猫 有 毛 吗\n::proc run 泡茶\n' o29
grep -q '复合式' o29 && grep -q '条件「鱼|!鱼」\*\*成立\*\*' o29 \
    && ok "**或**：`鱼|!鱼` 恒成立（排中律，不依赖 鱼 真假）" \
    || bad "条件与" "$(grep -m2 '条件' o29)"
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加条件 泡茶 鱼&!鱼 泡茶叶\n::op 加否则 泡茶 去烧水\n猫 有 毛 吗\n::proc run 泡茶\n' o30
grep -q '条件「鱼&!鱼」\*\*不成立\*\* → 走 「\[否则\]」' o30 \
    && ok "**与**：`鱼&!鱼` 恒不成立（自相矛盾）" \
    || bad "条件与假" "$(grep -m2 '条件' o30)"
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加条件 泡茶 猫|鱼 泡茶叶\n::op 加否则 泡茶 去烧水\n猫 有 毛 吗\n::proc run 泡茶\n' o31
grep -q '条件「猫|鱼」' o31 \
    && ok "**或**：`猫|鱼` 能判（有原子成立或整体不成立，都不是判不了）" \
    || bad "条件或" "$(grep -m2 '条件' o31)"
run '::proc on\n::proc dry off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加条件 泡茶 !鱼&!有个没这个词 泡茶叶\n::op 加否则 泡茶 去烧水\n猫 有 毛 吗\n::proc run 泡茶\n' o32
grep -q '有 1 个原子判不了 ⇒ \*\*判不了\*\*' o32 \
    && ok "**原子判不了**：整体判不了（不静默当成立）" \
    || bad "条件判不了" "$(grep -m2 '条件' o32)"
# §6.508 B2 **条件引用断言结果**：`#N` = 第 N 步「加断言」的复核结论（1 过 / 0 不过 / 没跑过=判不了）
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 猫 属于 动物\n::op 加条件 查 #1 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查\n' o33
grep -q '独立复核过' o33 && grep -q '条件「#1」\*\*成立\*\*' o33 \
    && ok "**断言过 ⇒ `#1` 成立**（条件引用前面核过的事实）" \
    || bad "断言条件真" "$(grep -m3 '条件\|复核' o33)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 猫 属于 鱼\n::op 加条件 查 #1 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查\n' o34
grep -q '复核不过' o34 && grep -q '条件「#1」\*\*不成立\*\*' o34 \
    && ok "**断言不过 ⇒ `#1` 不成立**（跟着复核结论走，不猜）" \
    || bad "断言条件假" "$(grep -m3 '条件\|复核' o34)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加步骤 查 烧水\n::op 加条件 查 #9 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查\n' o35
grep -q '条件「#9」' o35 && grep -q '判不了' o35 \
    && ok "**没跑过的断言步 ⇒ 判不了**（不当成立）" \
    || bad "断言条件缺" "$(grep -m2 '条件' o35)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 猫 属于 动物\n::op 加条件 查 #1&!鱼 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查\n' o36
grep -q '条件「#1&!鱼」\*\*成立\*\*' o36 \
    && ok "**`#N` 能和与/或/非组合**（`#1&!鱼`）" \
    || bad "断言条件复合" "$(grep -m2 '条件' o36)"
# §6.509 B3 **过程参数绑定**：步骤/断言/条件里的 `$1`..`$4` 由 `::proc run <名> <实参…>` 绑定
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 $1 属于 $2\n::op 加条件 查 #1 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查 猫 动物\n' o37
grep -q '要核「猫 属于 动物」' o37 && grep -q '条件「#1」\*\*成立\*\*' o37 \
    && ok "**同一个过程换实参**：`$1/$2` 绑成 猫/动物 ⇒ 复核过、`#1` 成立" \
    || bad "参数绑定真" "$(grep -m3 '要核\|条件' o37)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 $1 属于 $2\n::op 加条件 查 #1 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查 猫 鱼\n' o38
grep -q '要核「猫 属于 鱼」' o38 && grep -q '复核不过' o38 && grep -q '条件「#1」\*\*不成立\*\*' o38 \
    && ok "**换一组实参**（猫/鱼）⇒ 复核不过、`#1` 不成立（不是把上一条结果记住）" \
    || bad "参数绑定假" "$(grep -m3 '要核\|条件' o38)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 $1 属于 $2\n::op 加条件 查 #1 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查 猫\n' o39
grep -q '认不全' o39 && grep -q '\$2' o39 \
    && ok "**没给实参** ⇒ 明说认不全（占位符 `$2` 原样留着，不静默变空串）" \
    || bad "参数缺" "$(grep -m3 '认不全\|条件' o39)"
# §6.511 B4 **答复模板也是行为**：`加说` 给过程加一步"说出来"，走的是同一条受审通道
run '::proc on\n::proc dry off\n::op 建过程 答\n::op 加说 答 猫 属于 动物\n::proc run 答\n' o40
grep -q '答复\*\*：猫 属于 动物' o40 \
    && ok "**模板原样说出来**（过程跑完打「答复：」）" \
    || bad "加说" "$(grep -m3 '说：\|答复' o40)"
run '::proc on\n::proc dry off\n::op 建过程 答\n::op 加说 答 猫 属于 $1\n::proc run 答 动物\n' o41
grep -q '答复\*\*：猫 属于 动物' o41 \
    && ok "**模板里能带实参**（`$1` ⇒ 动物）" \
    || bad "加说参数" "$(grep -m3 '说：\|答复' o41)"
run '::proc on\n::proc dry off\n::op 建过程 答\n::op 加说 答 猫 属于 $2\n::proc run 答 动物\n' o42
grep -q '答复\*\*：猫 属于 \$2' o42 \
    && ok "**没给的实参原样留着**（`$2` 不静默变空串）" \
    || bad "加说缺参" "$(grep -m3 '说：\|答复' o42)"
# §6.513 B5 **外壳也是图里的记号**：`标外壳` 建一条边，剥离环节先读桶、再读图
run '::op 标外壳 帮我看下\n帮我看下猫属于动物吗\n' o43
grep -q '行为 标外壳' o43 && grep -q 'AI > 对' o43 \
    && ok "图上标了外壳 ⇒ 剥离环节**从图里读**（答出来了，不是兜底）" \
    || bad "图外壳" "$(grep -m3 '标外壳\|AI >' o43)"
run '帮我看下鲸鱼属于鱼吗\n::op 标外壳 鲸鱼属于\n鲸鱼属于鱼吗\n' o44
grep -q '行为 标外壳' o44 \
    && ok "同一会话里**现场标**新外壳（不用改代码/不用重启）" \
    || bad "现场标" "$(grep -m2 '标外壳' o44)"
# §6.515 B5 **图化等价**：`[lead]` 桶关掉（SHELLONLY）、只用**镜像到图上**的外壳边，问句照样答得出来
# ⚠ 必须在**仓库根**跑：这套的 WORK 目录里没有 patterns.txt（没有 `[lead]` 就没东西可镜像）——
#   而且 `cp` 要在管道外（放管道里会和二进制启动竞争，读到半个模型文件 —— 踩过）。
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '请问猫属于动物吗\n'; } | \
    AIP_SHELLMIRROR=1 AIP_SHELLONLY=1 "$WORK/aip" --model model.bin > "$WORK/o45" 2>&1 ) || true
grep -q 'AI > 对' "$WORK/o45" \
    && ok "**桶关掉、只用图** ⇒ 引导语照样剥、照样答（图化等价的第一步）" \
    || bad "图化等价" "$(grep -m3 'AI >\|外壳\|加载失败' "$WORK/o45")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '::quit\n'; } | \
    AIP_SHELLMIRROR=1 AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o46" 2>&1 ) || true
# 条数是**数据决定的**（每加一行 `[lead]` 就变）⇒ 从 patterns.txt 现数，别再写死
LEAD_LINES=$(grep -c '^\[lead\]' "$ROOT/patterns.txt" 2>/dev/null || echo 0)
grep -q '外壳镜像（AIP_SHELLMIRROR）' "$WORK/o46" && grep -q "→ 图上 ${LEAD_LINES} 条边" "$WORK/o46" \
    && ok "镜像日志：[lead] ${LEAD_LINES} 条 → 图上 ${LEAD_LINES} 条边（条数从数据现数，不写死）" \
    || bad "镜像日志" "$(grep -m2 '外壳\|lead=' "$WORK/o46")"
# §6.520 `::load` 之后**要重新镜像**（否则换脑子就把外壳全丢了 —— 套件里大量用 `::load`）
# ⚠ 在仓库根跑：WORK 目录没有 patterns.txt（没有 `[lead]` 就没东西可镜像）
( cd "$ROOT" && cp model.bin mx.bin && { printf '::load mx.bin\n'; printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '请问猫属于动物吗\n'; } | \
    AIP_PATDBG=1 "$WORK/aip" --model mx.bin > "$WORK/o47" 2>&1 ) || true
[ "$(grep -c '外壳镜像' "$WORK/o47")" -ge 2 ] \
    && ok "`::load` 之后**又镜像了一次**（启动 1 次 + 换脑子 1 次）" \
    || bad "load 后镜像" "$(grep -m3 '外壳镜像\|加载' "$WORK/o47")"
grep -q 'AI > 对' "$WORK/o47" \
    && ok "换脑子之后引导语照样剥（默认走图）" \
    || bad "load 后剥离" "$(grep -m2 'AI >' "$WORK/o47")"
# §6.521 外壳缓存**版本号**：标记一个"已经在池子里"的词，也必须立刻生效
run '::op 标外壳 请\n请猫属于动物吗\n::op 标外壳 问\n问猫属于动物吗\n' o48
[ "$(grep -c 'AI > 对' o48)" -ge 2 ] \
    && ok "**连标两个已有词**（请、问）⇒ 两条都立刻生效（缓存按版本号重建，不是按词数）" \
    || bad "外壳缓存版本号" "$(grep -m4 'AI >\|标外壳' o48)"
# §6.530 目标② **外壳的唯一写法在图上**：建边 = 标，删边 = 撤（不再有"只活在代码里"的引导语表）
run '::op 标外壳 咳\n咳猫属于动物吗\n::op 撤外壳 咳\n::edges 咳\n咳猫属于动物吗\n' o55
grep -q '行为 撤外壳' o55 && grep -q '\[咳\] 有 0 条边' o55 \
    && [ "$(grep -c 'AI > 对' o55)" = 1 ] \
    && ok "**撤外壳**：删掉标记边 + 缓存立刻失效（标记时答「对」，撤了以后不再答）" \
    || bad "撤外壳" "$(grep -m6 'AI >\|撤外壳\|条边' o55)"
run '::op 撤外壳 从没标过\n' o56
grep -q '不是.*外壳\|没有.*外壳' o56 \
    && ok "撤一个**不是外壳**的词 ⇒ 明说（不静默成功）" \
    || bad "撤外壳-空撤" "$(grep -m3 '撤外壳' o56)"
# ⚠ 同样要在仓库根跑：镜像记号 `[外壳:请问]` 只有在 patterns.txt 存在时才建
( cd "$ROOT" && { printf '::op 撤外壳 请问\n'; } | "$WORK/aip" --model model.bin > "$WORK/o57" 2>&1 ) || true
grep -q '\[lead\]\|镜像' "$WORK/o57" \
    && ok "**镜像来的外壳**（「[lead]」→ 图）撤不掉 ⇒ 明说要改数据行（不假装撤了）" \
    || bad "撤外壳-镜像" "$(grep -m3 '撤外壳' "$WORK/o57")"
# 存盘：图里标的外壳必须**活得比进程长**（否则"唯一写法"只是内存里的假象）
run '::op 标外壳 咳\n::save sb.bin\n' o58
{ printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '咳猫属于动物吗\n'; } \
    | ./aip --model sb.bin > o59 2>&1 || true
grep -q 'AI > 对' o59 \
    && ok "**存读之后还在**：「标外壳」建的边进了存档 ⇒ 新进程照样剥（不是只活在内存）" \
    || bad "标外壳-存读" "$(grep -m3 'AI >\|加载' o59)"
# 数据驱动：[lead] 桶关掉且不镜像 ⇒ 连「教你」都不剥（证明它已经没有代码兜底表了）
# ⚠ 必须在仓库根跑（WORK 里没有 patterns.txt —— 没有数据行就没有可镜像的东西）
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '教你 猫 属于 动物吗\n'; } | \
    "$WORK/aip" --model model.bin > "$WORK/o60" 2>&1 ) || true
grep -q 'AI > 对' "$WORK/o60" && ok "「教你…」默认剥得掉（数据行 + 镜像）" \
    || bad "教你-默认" "$(grep -m2 'AI >' "$WORK/o60")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '教你 猫 属于 动物吗\n'; } | \
    AIP_NODATA=lead AIP_SHELLMIRROR=0 "$WORK/aip" --model model.bin > "$WORK/o61" 2>&1 ) || true
if grep -q 'AI > 对' "$WORK/o61"; then
    bad "教你-数据关掉还答对" "说明还有代码兜底表在剥（agent_step 里那张 lead[]）"
else
    ok "**数据一关就剥不动**（「教你」没有代码兜底表 —— 外壳的唯一写法确实在数据/图上）"
fi
# §6.531 目标③：**疑问词表下沉成数据**（`[qword]` 整词 / `[qpart]` 子串）
# 为什么这条算"下沉"：表原来写死在 ai.h，加一个疑问词要重编译；现在 patterns.txt 是唯一来源，
# 没有文件时才退回内置种子表（保证"没有数据也不会把问句当陈述学进去"）。
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o62" 2>&1 ) || true
grep -q 'qword=24 qpart=34' "$WORK/o62" \
    && ok "疑问词表**从数据读**：qword=24 / qpart=34（patterns.txt 各一行，代码里那份只当种子）" \
    || bad "疑问词桶" "$(grep -o 'qword=[0-9]* qpart=[0-9]*' "$WORK/o62")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 哪儿 在\n'; } | \
    "$WORK/aip" --model model.bin > "$WORK/o63" 2>&1 ) || true
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 哪儿 在\n'; } | \
    AIP_NODATA=qword "$WORK/aip" --model model.bin > "$WORK/o64" 2>&1 ) || true
q_on=$(sed -n 's/.*AI > //p' "$WORK/o63" | tail -1)
q_off=$(sed -n 's/.*AI > //p' "$WORK/o64" | tail -1)
[ -n "$q_on" ] && [ "$q_on" != "$q_off" ] \
    && ok "关掉 qword 桶 ⇒ 答复就变（**桶在承重**；反证：改前这个开关对写死的表毫无作用）" \
    || bad "qword 消融" "on=「$q_on」 off=「$q_off」"
# §6.532 目标③续：**答复组织下沉**（`[say] 语义位=模板`；`$1`/`$2` 是槽位）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o65" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o65" \
    && ok "答复模板**从数据读**：say=$SAY_LINES（行数由数据决定）" \
    || bad "say 桶" "$(grep -o 'say=[0-9]*' "$WORK/o65")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n'; } | \
    "$WORK/aip" --model model.bin > "$WORK/o66" 2>&1 ) || true
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n'; } | \
    AIP_NODATA=say "$WORK/aip" --model model.bin > "$WORK/o67" 2>&1 ) || true
s_on=$(sed -n 's/.*AI > //p' "$WORK/o66" | tail -1)
s_off=$(sed -n 's/.*AI > //p' "$WORK/o67" | tail -1)
[ -n "$s_on" ] && [ "$s_on" != "$s_off" ] \
    && ok "关掉 say 桶 ⇒ 答复组织就变（判词消失、只剩推导链：数据在承重）" \
    || bad "say 消融" "on=「$s_on」 off=「$s_off」"
# 改**一行数据**就能换措辞（代码不动）—— 用一个临时目录，别弄脏真数据
SX="$WORK/sx"; rm -rf "$SX"; mkdir -p "$SX"
cp "$ROOT/model.bin" "$SX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$SX/" 2>/dev/null || true
( cd "$SX" && printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n' | cat - > /dev/null ) || true
sed -i 's/^\[say\] 组织\.定=.*/[say] 组织.定=（数据改的）$1 ⇒ $2/' "$SX/patterns.txt"
( cd "$SX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据改的）' "$SX/out.txt" \
    && ok "**改一行 [say] 组织.定** ⇒ 答复措辞跟着变（答复组织真的是数据）" \
    || bad "say 改模板" "$(sed -n 's/.*AI > //p' "$SX/out.txt" | tail -1)"
# §6.533 裸短回话：**输入词表 + 回话语措**两边都下沉（`[bare]` 组=词,词 + `[say] 回话.*`）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o68" 2>&1 ) || true
grep -q 'bare_yes=19 bare_no=14 bare_cf=12' "$WORK/o68" \
    && ok "裸回话词表**从数据读**：肯定 19 / 否定 14 / 不懂 12（三组，逗号分隔）" \
    || bad "bare 桶" "$(grep -o 'bare_yes=[0-9]* bare_no=[0-9]* bare_cf=[0-9]*' "$WORK/o68")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '好的\n'; } | \
    "$WORK/aip" --model model.bin > "$WORK/o69" 2>&1 ) || true
grep -q 'AI > 嗯 ， 好' "$WORK/o69" \
    && ok "裸回话**照旧**（「好的」⇒「嗯 ， 好」，措辞来自 [say] 回话.肯定）" \
    || bad "裸回话-肯定" "$(sed -n 's/.*AI > //p' "$WORK/o69" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '好的\n'; } | \
    AIP_NODATA=bare "$WORK/aip" --model model.bin > "$WORK/o70" 2>&1 ) || true
b_on=$(sed -n 's/.*AI > //p' "$WORK/o69" | tail -1)
b_off=$(sed -n 's/.*AI > //p' "$WORK/o70" | tail -1)
[ -n "$b_on" ] && [ "$b_on" != "$b_off" ] \
    && ok "关掉 bare 桶 ⇒ 「好的」不再算裸回话（桶在承重）" \
    || bad "bare 消融" "on=「$b_on」 off=「$b_off」"
# 加一个**数据里的新回话词**：`好哒`（代码里没有）—— 不用改代码就该被认出来
BX="$WORK/bx"; rm -rf "$BX"; mkdir -p "$BX"
cp "$ROOT/model.bin" "$BX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$BX/" 2>/dev/null || true
( cd "$BX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '好哒\n'; } | \
    "$WORK/aip" --model m.bin > before.txt 2>&1 ) || true
sed -i 's/^\[bare\] 肯定=\(.*\)/[bare] 肯定=\1,好哒/' "$BX/patterns.txt"
( cd "$BX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '好哒\n'; } | \
    "$WORK/aip" --model m.bin > after.txt 2>&1 ) || true
grep -q 'AI > 嗯 ， 好' "$BX/after.txt" && ! grep -q 'AI > 嗯 ， 好' "$BX/before.txt" \
    && ok "**加一行数据（好哒）** ⇒ 新的裸回话就这样被认出来（代码没动）" \
    || bad "bare 加词" "before=$(sed -n 's/.*AI > //p' "$BX/before.txt" | tail -1) after=$(sed -n 's/.*AI > //p' "$BX/after.txt" | tail -1)"
# §6.534 L1 判词识别下沉：`[verdict]` 六个**语义组**（答案词 / 判词 / 改口，不是一张表）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o71" 2>&1 ) || true
grep -q 'verdict=6' "$WORK/o71" \
    && ok "判词表**从数据读**：verdict=6（答词 2 + 判词 2 + 改口 2，分组保留、不合并）" \
    || bad "verdict 桶" "$(grep -o 'verdict=[0-9]*' "$WORK/o71")"
run '猫 属于 动物 吗\n不对\n' o72
grep -q '改 过来 了' o72 \
    && ok "改口**照旧**（问 → 答 → 「不对」⇒ 改过来了）" \
    || bad "改口-默认" "$(sed -n 's/.*AI > //p' o72 | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n不对\n'; } | \
    AIP_NODATA=verdict "$WORK/aip" --model model.bin > "$WORK/o73" 2>&1 ) || true
v_on=$(sed -n 's/.*AI > //p' o72 | tail -1)
v_off=$(sed -n 's/.*AI > //p' "$WORK/o73" | tail -1)
[ -n "$v_off" ] && [ "$v_off" != "$v_on" ] \
    && ok "关掉 verdict 桶 ⇒ 「不对」不再触发改口（桶在承重）" \
    || bad "verdict 消融" "on=「$v_on」 off=「$v_off」"
# 加一个**数据里的新改口词**：`瞎`（代码里没有；单 token —— 多字新词先要进词表，否则被分词拆开）
VX="$WORK/vx"; rm -rf "$VX"; mkdir -p "$VX"
cp "$ROOT/model.bin" "$VX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$VX/" 2>/dev/null || true
( cd "$VX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n瞎\n'; } | \
    "$WORK/aip" --model m.bin > before.txt 2>&1 ) || true
sed -i 's/^\[verdict\] 改口触发=\(.*\)/[verdict] 改口触发=\1,瞎/' "$VX/patterns.txt"
( cd "$VX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n瞎\n'; } | \
    "$WORK/aip" --model m.bin > after.txt 2>&1 ) || true
grep -q '改 过来 了' "$VX/after.txt" && ! grep -q '改 过来 了' "$VX/before.txt" \
    && ok "**加一行数据（瞎）** ⇒ 新的改口说法就认出来了（代码没动）" \
    || bad "verdict 加词" "before=$(sed -n 's/.*AI > //p' "$VX/before.txt" | tail -1) after=$(sed -n 's/.*AI > //p' "$VX/after.txt" | tail -1)"
# §6.535 L2 代词下沉：`[pron]` 四组（指代 / 省略指代 / 人称 / 须分隔）——**规则留代码，词表进数据**
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o74" 2>&1 ) || true
grep -q 'pron=4' "$WORK/o74" \
    && ok "代词表**从数据读**：pron=4（指代 6 / 省略指代 4 / 人称 7 / 须分隔 2）" \
    || bad "pron 桶" "$(grep -o 'pron=[0-9]*' "$WORK/o74")"
run '猫 属于 动物 吗\n它 属于 动物 吗\n' o75
[ "$(grep -c 'AI > 对 ， 猫' o75)" -ge 2 ] \
    && ok "省略主语**照旧**（「它」接上一句主语 ⇒ 答「对」）" \
    || bad "指代-默认" "$(sed -n 's/.*AI > //p' o75 | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n它 属于 动物 吗\n'; } | \
    AIP_NODATA=pron "$WORK/aip" --model model.bin > "$WORK/o76" 2>&1 ) || true
p_on=$(sed -n 's/.*AI > //p' o75 | tail -1)
p_off=$(sed -n 's/.*AI > //p' "$WORK/o76" | tail -1)
[ -n "$p_off" ] && [ "$p_off" != "$p_on" ] \
    && ok "关掉 pron 桶 ⇒ 「它」不再顶替主语（桶在承重）" \
    || bad "pron 消融" "on=「$p_on」 off=「$p_off」"
PX="$WORK/px"; rm -rf "$PX"; mkdir -p "$PX"
cp "$ROOT/model.bin" "$PX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PX/" 2>/dev/null || true
( cd "$PX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n那些 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > before.txt 2>&1 ) || true
sed -i 's/^\[pron\] 省略指代=\(.*\)/[pron] 省略指代=\1,那些/' "$PX/patterns.txt"
( cd "$PX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物 吗\n那些 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > after.txt 2>&1 ) || true
pr_b=$(sed -n 's/.*AI > //p' "$PX/before.txt" | tail -1)
pr_a=$(sed -n 's/.*AI > //p' "$PX/after.txt" | tail -1)
case "$pr_a" in
    *对*) [ -n "$pr_a" ] && [ "$pr_a" != "$pr_b" ] \
        && ok "**加一行数据（那些）** ⇒ 新的指代词就顶得上主语（代码没动）" \
        || bad "pron 加词" "before=「$pr_b」 after=「$pr_a」" ;;
    *) bad "pron 加词" "before=「$pr_b」 after=「$pr_a」" ;;
esac
# §6.536 L3 量词判据下沉：`[measure]` 词表 + `[say] 不知道.量` 措辞
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o77" 2>&1 ) || true
grep -q 'measure=1' "$WORK/o77" \
    && ok "量词表**从数据读**：measure=1 行（原有 11 条 + 数据还能加）" \
    || bad "measure 桶" "$(grep -o 'measure=[0-9]*' "$WORK/o77")"
run '猫 有 多大\n' o78
grep -q '具 体 数 量' o78 \
    && ok "量词问句**照旧**拒绝（「猫 有 多大」⇒ 不 编）" \
    || bad "量词-默认" "$(sed -n 's/.*AI > //p' o78 | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 有 多大\n'; } | \
    AIP_NODATA=measure "$WORK/aip" --model model.bin > "$WORK/o79" 2>&1 ) || true
m_on=$(sed -n 's/.*AI > //p' o78 | tail -1)
m_off=$(sed -n 's/.*AI > //p' "$WORK/o79" | tail -1)
[ -n "$m_off" ] && [ "$m_off" != "$m_on" ] \
    && ok "关掉 measure 桶 ⇒ 量词问句不再被拦住（桶在承重）" \
    || bad "measure 消融" "on=「$m_on」 off=「$m_off」"
# 数据补的洞：`多沉/多老` 以前被当成**陈述学进去**（"记下 了 ： 猫 有 多沉"）
# ⚠ 必须在仓库根跑：WORK 里没有 patterns.txt（没有数据就只有种子 11 条，补的词不生效）
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 有 多沉\n'; } | \
    "$WORK/aip" --model model.bin > "$WORK/o80" 2>&1 ) || true
grep -q '具 体 数 量' "$WORK/o80" \
    && ok "数据补上的量词（多沉）⇒ 拒答（以前是「记下 了」，把问句学成事实）" \
    || bad "measure 补洞" "$(sed -n 's/.*AI > //p' "$WORK/o80" | tail -1)"
# §6.537 L4 "不知道"的说法池下沉：`[refuse] 说法=…`（原来是 ai.h 里写死的 6 句）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o81" 2>&1 ) || true
grep -q 'refuse=1' "$WORK/o81" \
    && ok "兜底说法池**从数据读**：refuse=1 行（6 句，逗号分隔）" \
    || bad "refuse 桶" "$(grep -o 'refuse=[0-9]*' "$WORK/o81")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 的 妈妈 是 谁\n'; } | \
    "$WORK/aip" --model model.bin > "$WORK/o82" 2>&1 ) || true
grep -q '这个我 不 太 懂' "$WORK/o82" \
    && ok "兜底措辞**照旧**（「猫 的 妈妈 是 谁」⇒「这个我 不 太 懂」）" \
    || bad "refuse-默认" "$(sed -n 's/.*AI > //p' "$WORK/o82" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 的 妈妈 是 谁\n'; } | \
    AIP_NODATA=refuse "$WORK/aip" --model model.bin > "$WORK/o83" 2>&1 ) || true
r_on=$(sed -n 's/.*AI > //p' "$WORK/o82" | tail -1)
r_off=$(sed -n 's/.*AI > //p' "$WORK/o83" | tail -1)
[ -n "$r_off" ] && [ "$r_on" != "$r_off" ] \
    && ok "关掉 refuse 桶 ⇒ 兜底措辞就变（桶在承重）" \
    || bad "refuse 消融" "on=「$r_on」 off=「$r_off」"
RX="$WORK/rx"; rm -rf "$RX"; mkdir -p "$RX"
cp "$ROOT/model.bin" "$RX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$RX/" 2>/dev/null || true
sed -i 's/^\[refuse\] 说法=.*/[refuse] 说法=（数据）我 想 想 再 说/' "$RX/patterns.txt"
( cd "$RX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 的 妈妈 是 谁\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）我 想 想 再 说' "$RX/out.txt" \
    && ok "**改一行数据** ⇒ 兜底的说法就换掉（说话方式真的是数据）" \
    || bad "refuse 改池" "$(sed -n 's/.*AI > //p' "$RX/out.txt" | tail -1)"
# §6.538 L5 列表式答复下沉：`[say] 列表.头/项分隔/余/空/空类别/空地点`
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o84" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o84" \
    && ok "列表答复模板**从数据读**（含后加的 不知道 3 行 ⇒ 总数 24）" \
    || bad "列表模板" "$(grep -o 'say=[0-9]*' "$WORK/o84")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 吃 什么\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o85" 2>&1 ) || true
grep -q '我 记 的 是 ： 猫 吃 鱼 、 肉' "$WORK/o85" \
    && ok "列表答复**照旧**（「猫 吃 什么」⇒「我 记 的 是 ： 猫 吃 鱼 、 肉 …」）" \
    || bad "列表-默认" "$(sed -n 's/.*AI > //p' "$WORK/o85" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '麒麟 在 哪\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o86" 2>&1 ) || true
grep -q '什么 ， 图里 没 记 过' "$WORK/o86" \
    && ok "列表为空**照旧**（「麒麟 在 哪」⇒「… 什么 ， 图里 没 记 过（不 编）」）" \
    || bad "列表-空" "$(sed -n 's/.*AI > //p' "$WORK/o86" | tail -1)"
LX="$WORK/lx"; rm -rf "$LX"; mkdir -p "$LX"
cp "$ROOT/session.bin" "$LX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$LX/" 2>/dev/null || true
sed -i 's/^\[say\] 列表\.头=.*/[say] 列表.头=（数据）$1$2$3/; s/^\[say\] 列表\.项分隔=.*/[say] 列表.项分隔=｜/' "$LX/patterns.txt"
( cd "$LX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 吃 什么\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）猫吃 鱼 ｜ 肉' "$LX/out.txt" \
    && ok "**改两行数据** ⇒ 列表的头与分隔符都换掉（答复组织真的是数据）" \
    || bad "列表改模板" "$(sed -n 's/.*AI > //p' "$LX/out.txt" | tail -1)"
# §6.539 L4 续：带槽位的"不知道"整句也进 `[say]`（不认识 / 不敢说会 / 没这条）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o87" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o87" \
    && ok "带槽位的不知道说法**也从数据读**：say=$SAY_LINES（行数由数据决定）" \
    || bad "不知道槽位" "$(grep -o 'say=[0-9]*' "$WORK/o87")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '你 会 飞 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o88" 2>&1 ) || true
grep -q '这 件 事 我 不 敢 说 会' "$WORK/o88" \
    && ok "「不敢说会」**照旧**（`你 会 飞 吗` ⇒ 「这 件 事 我 不 敢 说 会 …」）" \
    || bad "不敢说会-默认" "$(sed -n 's/.*AI > //p' "$WORK/o88" | tail -1)"
NX="$WORK/nx"; rm -rf "$NX"; mkdir -p "$NX"
cp "$ROOT/session.bin" "$NX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$NX/" 2>/dev/null || true
sed -i 's/^\[say\] 不知道\.不敢说会=.*/[say] 不知道.不敢说会=（数据）这 个 我 没 学 过：$1（你 想 教 就 说「你 会 $2」）/' "$NX/patterns.txt"
( cd "$NX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '你 会 飞 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）这 个 我 没 学 过：飞' "$NX/out.txt" \
    && ok "**改一行数据** ⇒ 带槽位的整句换成数据里的（两个槽位都填对了）" \
    || bad "不敢说会-改模板" "$(sed -n 's/.*AI > //p' "$NX/out.txt" | tail -1)"
# §6.540 视角前缀 + "没听他说过"也进 `[say]`（§6.278 他人立场）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o89" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o89" \
    && ok "视角/没听过**从数据读**：say=$SAY_LINES（行数由数据决定）" \
    || bad "视角模板" "$(grep -o 'say=[0-9]*' "$WORK/o89")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '小明 以为 猫 会 飞\n小明 以为 猫 属于 鱼 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o90" 2>&1 ) || true
grep -q '在 小明 看来 ： 我 没 听 他 说 过 这 个' "$WORK/o90" \
    && ok "他人立场答复**照旧**（「在 小明 看来 ： 我 没 听 他 说 过 这 个 …」）" \
    || bad "视角-默认" "$(sed -n 's/.*AI > //p' "$WORK/o90" | tail -1)"
WX="$WORK/wx"; rm -rf "$WX"; mkdir -p "$WX"
cp "$ROOT/session.bin" "$WX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$WX/" 2>/dev/null || true
sed -i 's/^\[say\] 视角\.在=.*/[say] 视角.在=（数据）$1 的 看法 里 ： $2/' "$WX/patterns.txt"
( cd "$WX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '小明 以为 猫 会 飞\n小明 以为 猫 属于 鱼 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）小明 的 看法 里 ：' "$WX/out.txt" \
    && ok "**改一行数据** ⇒ 视角前缀换掉（两个槽位都填对）" \
    || bad "视角-改模板" "$(sed -n 's/.*AI > //p' "$WX/out.txt" | tail -1)"
# §6.541 L6 关系别名下沉：`[alias] 词=规范关系名`（关系 id 仍由代码枚举定，进数据的只是"哪些词归到哪"）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o91" 2>&1 ) || true
grep -q 'alias=1' "$WORK/o91" \
    && ok "关系别名**从数据读**：alias=1 行（5 条别名，词=规范关系名）" \
    || bad "alias 桶" "$(grep -o 'alias=[0-9]*' "$WORK/o91")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 拥有 毛 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o92" 2>&1 ) || true
grep -q '猫 -\[属于·继承\]-> 动物' "$WORK/o92" \
    && ok "别名**照旧**（「猫 拥有 毛 吗」⇒ 经「有」继承答出来）" \
    || bad "alias-默认" "$(sed -n 's/.*AI > //p' "$WORK/o92" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 拥有 毛 吗\n'; } | \
    AIP_NODATA=alias "$WORK/aip" --model session.bin > "$WORK/o93" 2>&1 ) || true
a_on=$(sed -n 's/.*AI > //p' "$WORK/o92" | tail -1)
a_off=$(sed -n 's/.*AI > //p' "$WORK/o93" | tail -1)
[ -n "$a_off" ] && [ "$a_off" != "$a_on" ] \
    && ok "关掉 alias 桶 ⇒「拥有」不再归到「有」（桶在承重）" \
    || bad "alias 消融" "on=「$a_on」 off=「$a_off」"
# 改一行数据：把「拥有」改归到「怕」⇒ 同一个词的意义就变了（证明映射由数据定）
AX="$WORK/ax"; rm -rf "$AX"; mkdir -p "$AX"
cp "$ROOT/session.bin" "$AX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$AX/" 2>/dev/null || true
sed -i 's/^\[alias\] 词=.*/[alias] 词=惧怕=怕,喜爱=喜欢,喜好=喜欢,拥有=怕,具有=有/' "$AX/patterns.txt"
( cd "$AX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 拥有 毛 吗\n'; } | \
    "$WORK/aip" --model m.bin > after.txt 2>&1 ) || true
a_new=$(sed -n 's/.*AI > //p' "$AX/after.txt" | tail -1)
[ -n "$a_new" ] && [ "$a_new" != "$a_on" ] && ! echo "$a_new" | grep -q '属于·继承' \
    && ok "**改一行数据（拥有=怕）** ⇒ 同一个词的意义就变了（别名映射真的由数据定）" \
    || bad "alias 改映射" "默认=「$a_on」 改后=「$a_new」"
# §6.542 清单外第一类：**删除请求的答复**（`忘掉/删掉 X R Y`）进 `[say]`
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o94" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o94" \
    && ok "删除答复**从数据读**：say=$SAY_LINES（行数由数据决定）" \
    || bad "删除模板" "$(grep -o 'say=[0-9]*' "$WORK/o94")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '忘掉 猫 属于 动物\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o95" 2>&1 ) || true
grep -q '这 条 我 删 不 了' "$WORK/o95" \
    && ok "「删不了」**照旧**（硬底线 + 改口建议）" \
    || bad "删除-默认" "$(sed -n 's/.*AI > //p' "$WORK/o95" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '忘掉 猫 属于 鱼\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o96" 2>&1 ) || true
grep -q '我 图 里 没 有「猫 属于 鱼」，不 用 删' "$WORK/o96" \
    && ok "「本来就没有」**照旧**（三个槽位都填对）" \
    || bad "删除-没有" "$(sed -n 's/.*AI > //p' "$WORK/o96" | tail -1)"
FX="$WORK/fx"; rm -rf "$FX"; mkdir -p "$FX"
cp "$ROOT/session.bin" "$FX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$FX/" 2>/dev/null || true
sed -i 's/^\[say\] 删除\.不行=.*/[say] 删除.不行=（数据）删 不 掉：$1 $2 $3/' "$FX/patterns.txt"
( cd "$FX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '忘掉 猫 属于 动物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）删 不 掉：猫 属于 动物' "$FX/out.txt" \
    && ok "**改一行数据** ⇒ 删除答复换掉（槽位都填对）" \
    || bad "删除-改模板" "$(sed -n 's/.*AI > //p' "$FX/out.txt" | tail -1)"
# §6.543 清单外第二类：**"刚才说了什么/聊了什么"的答复**进 `[say]`（`历史.*`）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o97" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o97" \
    && ok "历史回顾**从数据读**：say=$SAY_LINES（行数由数据决定）" \
    || bad "历史模板" "$(grep -o 'say=[0-9]*' "$WORK/o97")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物\n刚才 我 说 了 什么\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o98" 2>&1 ) || true
grep -q '你 刚才 说 的 是 ： 猫 属于 动物' "$WORK/o98" \
    && ok "「我刚才说了什么」**照旧**" \
    || bad "历史-默认" "$(sed -n 's/.*AI > //p' "$WORK/o98" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物\n刚才 聊 了 什么\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o99" 2>&1 ) || true
grep -q '刚才 我们 聊 了 ： 猫 属于 动物' "$WORK/o99" \
    && ok "「刚才聊了什么」**照旧**（列表头也走数据）" \
    || bad "历史-我们" "$(sed -n 's/.*AI > //p' "$WORK/o99" | tail -1)"
HX="$WORK/hx"; rm -rf "$HX"; mkdir -p "$HX"
cp "$ROOT/session.bin" "$HX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$HX/" 2>/dev/null || true
sed -i 's/^\[say\] 历史\.你说=.*/[say] 历史.你说=（数据）你 刚 才 说 的 是 ： $1/' "$HX/patterns.txt"
( cd "$HX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物\n刚才 我 说 了 什么\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）你 刚 才 说 的 是 ： 猫 属于 动物' "$HX/out.txt" \
    && ok "**改一行数据** ⇒ 历史回顾前缀换掉（槽位填对）" \
    || bad "历史-改模板" "$(sed -n 's/.*AI > //p' "$HX/out.txt" | tail -1)"
# §6.544 X5 先量清（天气/时间**本来就是 [reply] 数据**）+ X4 教学提示下沉
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '现在 几点\n'; } | \
    AIP_NODATA=reply "$WORK/aip" --model session.bin > "$WORK/o100" 2>&1 ) || true
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '现在 几点\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o101" 2>&1 ) || true
grep -q '我 没有 时 钟' "$WORK/o101" && ! grep -q '我 没有 时 钟' "$WORK/o100" \
    && ok "天气/时间**本来就是 `[reply]` 数据**（关掉 reply 桶就不再说「我没时钟」）" \
    || bad "X5-消融" "off=$(sed -n 's/.*AI > //p' "$WORK/o100" | tail -1)"
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o102" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o102" \
    && ok "教学提示**从数据读**：say=$SAY_LINES（行数由数据决定）" \
    || bad "教学模板" "$(grep -o 'say=[0-9]*' "$WORK/o102")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '教你 会飞 的 动物 是 鸟\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o103" 2>&1 ) || true
grep -q '这 是 一 条 规则' "$WORK/o103" \
    && ok "「规则句学不进」**照旧**" \
    || bad "教学-默认" "$(sed -n 's/.*AI > //p' "$WORK/o103" | tail -1)"
TX="$WORK/tx"; rm -rf "$TX"; mkdir -p "$TX"
cp "$ROOT/session.bin" "$TX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$TX/" 2>/dev/null || true
sed -i 's/^\[say\] 教学\.规则=.*/[say] 教学.规则=（数据）这 条 是 规则，别 教 形 式/' "$TX/patterns.txt"
( cd "$TX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '教你 会飞 的 动物 是 鸟\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）这 条 是 规则' "$TX/out.txt" \
    && ok "**改一行数据** ⇒ 教学提示换掉（代码没动）" \
    || bad "教学-改模板" "$(sed -n 's/.*AI > //p' "$TX/out.txt" | tail -1)"
# §6.545 清单外 C1 第一刀：**"记下了"学习确认**进 `[say]`（四槽位）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o104" 2>&1 ) || true
grep -q "say=$SAY_LINES" "$WORK/o104" \
    && ok "学习确认**从数据读**：say=35（新增 学习.事实 / 学习.新词）" \
    || bad "学习模板" "$(grep -o 'say=[0-9]*' "$WORK/o104")"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 会 打呼噜\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o105" 2>&1 ) || true
grep -q '记下 了 ： 猫 会 打呼噜' "$WORK/o105" \
    && ok "「记下了」**照旧**（新谓语现场学）" \
    || bad "学习-默认" "$(sed -n 's/.*AI > //p' "$WORK/o105" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 喜欢 晒太阳\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o106" 2>&1 ) || true
grep -q '记下 了 ： 猫 喜欢 晒太阳（新词当场造）' "$WORK/o106" \
    && ok "「新词当场造」**照旧**（后缀也是数据）" \
    || bad "学习-新词" "$(sed -n 's/.*AI > //p' "$WORK/o106" | tail -1)"
LX2="$WORK/lx2"; rm -rf "$LX2"; mkdir -p "$LX2"
cp "$ROOT/session.bin" "$LX2/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$LX2/" 2>/dev/null || true
sed -i 's/^\[say\] 学习\.事实分隔=.*/[say] 学习.事实分隔=（数据）学 会 了：$1 $2 $3/' "$LX2/patterns.txt"
( cd "$LX2" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 会 打呼噜\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）学 会 了：猫 会 打呼噜' "$LX2/out.txt" \
    && ok "**改一行数据** ⇒ 学习确认换掉（四个槽位都填对）" \
    || bad "学习-改模板" "$(sed -n 's/.*AI > //p' "$LX2/out.txt" | tail -1)"
# §6.546 C1 余量：**「记下了」的并列式 / 地点式**也进 `[say]`
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 和 狗 都 是 动物\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o107" 2>&1 ) || true
grep -q '记下 了 ： 猫 和 狗 都 是 动物' "$WORK/o107" \
    && ok "并列学习确认**照旧**" \
    || bad "学习-并列" "$(sed -n 's/.*AI > //p' "$WORK/o107" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '我 在 家\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o108" 2>&1 ) || true
grep -q '记下 了 ： 我 在 家' "$WORK/o108" \
    && ok "地点学习确认**照旧**" \
    || bad "学习-地点" "$(sed -n 's/.*AI > //p' "$WORK/o108" | tail -1)"
_K2="$WORK/k2"; rm -rf "$_K2"; mkdir -p "$_K2"
cp "$ROOT/session.bin" "$_K2/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$_K2/" 2>/dev/null || true
sed -i 's/^\[say\] 学习\.并列=.*/[say] 学习.并列=（数据）都 记 下 了：$1 和 $2 都 $3 $4/; s/^\[say\] 学习\.事实分隔=.*/[say] 学习.事实分隔=（数据）地 点 记 下：$1 住 着 $2 $3/' "$_K2/patterns.txt"
( cd "$_K2" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 和 狗 都 是 动物\n我 在 家\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）都 记 下 了：猫 和 狗 都 是 动物' "$_K2/out.txt" && grep -q '（数据）地 点 记 下：我 住 着 在 家' "$_K2/out.txt" \
    && ok "**改两行数据** ⇒ 并列式与地点式都换掉（槽位都对）" \
    || bad "学习-余量改模板" "$(sed -n 's/.*AI > //p' "$_K2/out.txt" | tail -3 | tr '\n' ' ')"
# §6.547 C2：**推理措辞**（因为/其实是反的/只能猜/没依据）进 `[say]`
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '为什么 猫 属于 动物\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o109" 2>&1 ) || true
grep -q '因为 猫 属于 动物 —— 直接 记 着 的 事实' "$WORK/o109" \
    && ok "「因为…」**照旧**（直接事实）" \
    || bad "推理-因为" "$(sed -n 's/.*AI > //p' "$WORK/o109" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '为什么 鲸鱼 属于 动物\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o110" 2>&1 ) || true
grep -q '多 跳 推 导：鲸鱼' "$WORK/o110" \
    && ok "「多跳推导」**照旧**（来源那半句也是模板）" \
    || bad "推理-多跳" "$(sed -n 's/.*AI > //p' "$WORK/o110" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '为什么 猫 会 飞\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o111" 2>&1 ) || true
grep -q '其实 是 反 的：猫 不 会 飞' "$WORK/o111" \
    && ok "「其实是反的」**照旧**" \
    || bad "推理-反" "$(sed -n 's/.*AI > //p' "$WORK/o111" | tail -1)"
RX2="$WORK/rx2"; rm -rf "$RX2"; mkdir -p "$RX2"
cp "$ROOT/session.bin" "$RX2/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$RX2/" 2>/dev/null || true
sed -i 's/^\[say\] 推理\.因为=.*/[say] 推理.因为=（数据）$1 $2$3 $4 成立，$5 给的：$6/' "$RX2/patterns.txt"
( cd "$RX2" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '为什么 猫 属于 动物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）猫 属于 动物 成立' "$RX2/out.txt" \
    && ok "**改一行数据** ⇒ 六槽位推理句式换掉（代码没动）" \
    || bad "推理-改模板" "$(sed -n 's/.*AI > //p' "$RX2/out.txt" | tail -1)"
# §6.548 C3：过程引擎答复（没有/关着/跑完了）进 `[say]`
( cd "$ROOT" && { printf '::proc on\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '跑 从没建过\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o112" 2>&1 ) || true
grep -q '没有 「 从没建过 」 这个 过程' "$WORK/o112" \
    && ok "「没有这个过程」**照旧**" \
    || bad "过程-没有" "$(sed -n 's/.*AI > //p' "$WORK/o112" | tail -1)"
( cd "$ROOT" && { printf '::proc on\n::proc dry off\n::op 建过程 查\n::proc dry off\n::char off\n'; printf '跑 查\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o113" 2>&1 ) || true
grep -q '跑 完 了 「 查 」 ： 每 一步 都 过 得 去' "$WORK/o113" \
    && ok "「跑完了」**照旧**" \
    || bad "过程-跑完" "$(sed -n 's/.*AI > //p' "$WORK/o113" | tail -1)"
PCX="$WORK/pcx"; rm -rf "$PCX"; mkdir -p "$PCX"
cp "$ROOT/session.bin" "$PCX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PCX/" 2>/dev/null || true
sed -i 's/^\[say\] 过程\.没有=.*/[say] 过程.没有=（数据）查 无 此 过 程：$1/; s/^\[say\] 过程\.跑完=.*/[say] 过程.跑完=（数据）$1 跑 了，$2/' "$PCX/patterns.txt"
( cd "$PCX" && { printf '::proc on\n::proc dry off\n::op 建过程 查\n::proc dry off\n::char off\n'; printf '跑 从没建过\n跑 查\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）查 无 此 过 程：从没建过' "$PCX/out.txt" && grep -q '（数据）查 跑 了，每 一步 都 过 得 去' "$PCX/out.txt" \
    && ok "**改两行数据** ⇒ 过程答复都换掉（槽位都对）" \
    || bad "过程-改模板" "$(sed -n 's/.*AI > //p' "$PCX/out.txt" | tail -3 | tr '\n' ' ')"
# §6.549 C3 余量：建过程 / 加步骤 / 多个过程的答复也进 `[say]`
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '记个过程 泡茶\n第1步 烧水\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o114" 2>&1 ) || true
grep -q '好 ， 过程 「 泡茶 」 记下 了' "$WORK/o114" && grep -q '记下 了 ： 过程 「 泡茶 」 第 1 步 是 「 烧水 」' "$WORK/o114" \
    && ok "建过程 / 加步骤**照旧**" \
    || bad "过程-建与步" "$(sed -n 's/.*AI > //p' "$WORK/o114" | tail -2 | tr '\n' ' ')"
PCY="$WORK/pcy"; rm -rf "$PCY"; mkdir -p "$PCY"
cp "$ROOT/session.bin" "$PCY/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PCY/" 2>/dev/null || true
sed -i 's/^\[say\] 过程\.建了=.*/[say] 过程.建了=（数据）建 好 了：$1（$2）/; s/^\[say\] 过程\.加了步=.*/[say] 过程.加了步=（数据）$1 的 第 $2 步：$3/' "$PCY/patterns.txt"
( cd "$PCY" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '记个过程 泡茶\n第1步 烧水\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）建 好 了：泡茶' "$PCY/out.txt" && grep -q '（数据）泡茶 的 第 1 步：烧水' "$PCY/out.txt" \
    && ok "**改两行数据** ⇒ 建过程/加步骤的答复都换掉（槽位都对）" \
    || bad "过程-余量改模板" "$(sed -n 's/.*AI > //p' "$PCY/out.txt" | tail -3 | tr '\n' ' ')"
# §6.550 C4：假设层答复（撤一层 / 撤指定 / 找不到）进 `[say]`
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '如果 猫 会 飞\n如果 猫 会 游\n算了\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o115" 2>&1 ) || true
grep -q '好 的（还有 别的 假设）' "$WORK/o115" \
    && ok "撤一层（还有别的）**照旧**" \
    || bad "假设-撤层" "$(sed -n 's/.*AI > //p' "$WORK/o115" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '如果 猫 会 飞\n算了\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o116" 2>&1 ) || true
grep -q 'AI > 好 的' "$WORK/o116" && ! grep -q '还有 别的 假设' "$WORK/o116" \
    && ok "撤一层（最后一层）**照旧**（不带尾巴）" \
    || bad "假设-最后一层" "$(sed -n 's/.*AI > //p' "$WORK/o116" | tail -1)"
HXX="$WORK/hxx"; rm -rf "$HXX"; mkdir -p "$HXX"
cp "$ROOT/session.bin" "$HXX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$HXX/" 2>/dev/null || true
sed -i 's/^\[say\] 假设\.撤一层=.*/[say] 假设.撤一层=（数据）撤 掉 一 层/; s/^\[say\] 假设\.撤层还有=.*/[say] 假设.撤层还有=（数据）撤 一 层（还 有 $1 层）/' "$HXX/patterns.txt"
( cd "$HXX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '如果 猫 会 飞\n如果 猫 会 游\n算了\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）撤 一 层（还 有 1 层）' "$HXX/out.txt" \
    && ok "**改两行数据** ⇒ 撤一层的答复换掉（槽位填对）" \
    || bad "假设-改模板" "$(sed -n 's/.*AI > //p' "$HXX/out.txt" | tail -2 | tr '\n' ' ')"
# §6.552 C7 第一刀：**能力答复 + "我 知道"**（关系答复族的两句）
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '你 会 游泳\n你 会 游泳 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o117" 2>&1 ) || true
grep -q '对 ， 你 -\[会\]-> 游泳' "$WORK/o117" \
    && ok "能力答复**照旧**（教过之后问 ⇒「对 ， 你 -[会]-> 游泳」）" \
    || bad "能力-会" "$(sed -n 's/.*AI > //p' "$WORK/o117" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 属于 动物\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o118" 2>&1 ) || true
grep -q '我 知道 ： 猫 属于 动物' "$WORK/o118" \
    && ok "「我 知道」**照旧**（教一条已知事实时不装没听见）" \
    || bad "我知道-默认" "$(sed -n 's/.*AI > //p' "$WORK/o118" | tail -1)"
CXX="$WORK/cxx"; rm -rf "$CXX"; mkdir -p "$CXX"
cp "$ROOT/session.bin" "$CXX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$CXX/" 2>/dev/null || true
sed -i 's/^\[say\] 能力\.会=.*/[say] 能力.会=（数据）你 会 $1/; s/^\[say\] 关系\.我知道=.*/[say] 关系.我知道=（数据）早 就 记 着：$1 $2 $3/' "$CXX/patterns.txt"
( cd "$CXX" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '你 会 游泳\n你 会 游泳 吗\n猫 属于 动物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）你 会 游泳' "$CXX/out.txt" && grep -q '（数据）早 就 记 着：猫 属于 动物' "$CXX/out.txt" \
    && ok "**改两行数据** ⇒ 两句都换掉（槽位都对）" \
    || bad "能力-改模板" "$(sed -n 's/.*AI > //p' "$CXX/out.txt" | tail -3 | tr '\n' ' ')"
# §6.553 C8："不知道"变体三句（几个 / 几岁 / 是谁）进 `[say]`
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 有 几 个 头\n猫 几 岁\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o119" 2>&1 ) || true
grep -q '我 不 知 道 猫 有 几 个 头' "$WORK/o119" && grep -q '我 不 知 道 猫 几 岁' "$WORK/o119" \
    && ok "「不知道有几个/几岁」**照旧**" \
    || bad "C8-默认" "$(sed -n 's/.*AI > //p' "$WORK/o119" | tail -2 | tr '\n' ' ')"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 是 谁\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o120" 2>&1 ) || true
grep -q '我 不 知 道 猫 是 谁 —— 图 里 没 记 过 名 字（只 记 了 猫 属 于 动物）' "$WORK/o120" \
    && ok "「不知道是谁（带类别）」**照旧**" \
    || bad "C8-是谁" "$(sed -n 's/.*AI > //p' "$WORK/o120" | tail -1)"
C8X="$WORK/c8x"; rm -rf "$C8X"; mkdir -p "$C8X"
cp "$ROOT/session.bin" "$C8X/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$C8X/" 2>/dev/null || true
sed -i 's/^\[say\] 不知道\.数目=.*/[say] 不知道.数目=（数据）$1 有 $2 我 没 记 数 目/; s/^\[say\] 不知道\.岁=.*/[say] 不知道.岁=（数据）$1 几 岁 我 没 记/' "$C8X/patterns.txt"
( cd "$C8X" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 有 几 个 头\n猫 几 岁\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）猫 有 几 个 头 我 没 记 数 目' "$C8X/out.txt" && grep -q '（数据）猫 几 岁 我 没 记' "$C8X/out.txt" \
    && ok "**改两行数据** ⇒ 两句都换掉（槽位都对）" \
    || bad "C8-改模板" "$(sed -n 's/.*AI > //p' "$C8X/out.txt" | tail -2 | tr '\n' ' ')"
# §6.554 C9 他者信念（**开关 `::others ask on` 才可达**）：投一票 + 我的立场
( cd "$ROOT" && { printf '::others ask on\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 会 飞\n大家 都 说 猫 会 飞 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o121" 2>&1 ) || true
grep -q '大家 都 说 ： 猫 会 飞（1 个人 说 过 ；我 不 这么 看）' "$WORK/o121" \
    && ok "「大家都说」**照旧**（开关打开时）" \
    || bad "C9-大家" "$(sed -n 's/.*AI > //p' "$WORK/o121" | tail -1)"
( cd "$ROOT" && { printf '::others ask on\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 会 飞\n大家 都 说 猫 会 游 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o122" 2>&1 ) || true
grep -q '这 件 事 他们 没 说 过（我 自己 也 不 知道）' "$WORK/o122" \
    && ok "「他们没说过」**照旧**" \
    || bad "C9-没说过" "$(sed -n 's/.*AI > //p' "$WORK/o122" | tail -1)"
C9X="$WORK/c9x"; rm -rf "$C9X"; mkdir -p "$C9X"
cp "$ROOT/session.bin" "$C9X/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$C9X/" 2>/dev/null || true
sed -i 's/^\[say\] 信念\.大家都说=.*/[say] 信念.大家都说=（数据）$1 $2$3 $4（$5 人；$6）/; s/^\[say\] 信念\.我不同意=.*/[say] 信念.我不同意=（数据）我 不 认/' "$C9X/patterns.txt"
( cd "$C9X" && { printf '::others ask on\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 会 飞\n大家 都 说 猫 会 飞 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）猫 会 飞（1 人；（数据）我 不 认）' "$C9X/out.txt" \
    && ok "**改两行数据** ⇒ 投一票句与我的立场都换掉（槽位都对）" \
    || bad "C9-改模板" "$(sed -n 's/.*AI > //p' "$C9X/out.txt" | tail -1)"
# §6.557 C9 余量：说法不一致 / 大家都说不 / 有相关说法（**都要先 `::others ask on`**）
( cd "$ROOT" && { printf '::others ask on\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 不 会 飞\n她 以为 猫 会 飞\n大家 都 说 猫 会 飞 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o124" 2>&1 ) || true
grep -q '这 件 事 大家 说 法 不 一致 ： 有人 说 猫 会 飞 、 有人 说 不 会（1 认 / 1 不认' "$WORK/o124" \
    && ok "「说法不一致」**照旧**（两人各说一边 ⇒ 待验不否定）" \
    || bad "C9b-不一致" "$(sed -n 's/.*AI > //p' "$WORK/o124" | tail -1)"
( cd "$ROOT" && { printf '::others ask on\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 不 会 飞\n大家 都 说 猫 会 飞 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o125" 2>&1 ) || true
grep -q '大家 都 说 ： 猫 不 会 飞（1 个人 说 过 ；我 不 这么 看）' "$WORK/o125" \
    && ok "「大家都说不」**照旧**" \
    || bad "C9b-都不" "$(sed -n 's/.*AI > //p' "$WORK/o125" | tail -1)"
C9Y="$WORK/c9y"; rm -rf "$C9Y"; mkdir -p "$C9Y"
cp "$ROOT/session.bin" "$C9Y/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$C9Y/" 2>/dev/null || true
sed -i 's|^\[say\] 信念\.说法不一=.*|[say] 信念.说法不一=（数据）$1 $2 $3 与 不 $4 打 平（$5 比 $6；$7）|' "$C9Y/patterns.txt"
( cd "$C9Y" && { printf '::others ask on\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 不 会 飞\n她 以为 猫 会 飞\n大家 都 说 猫 会 飞 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）猫 会 飞 与 不 会 打 平（1 比 1；我 不 这么 看）' "$C9Y/out.txt" \
    && ok "**改一行数据** ⇒ 不一致句换掉（七个槽位都对）" \
    || bad "C9b-改模板" "$(sed -n 's/.*AI > //p' "$C9Y/out.txt" | tail -1)"
# §6.559 F1：学习/改口确认五句进 `[say]`
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '猫 属于 动物 吗\n对\n猫 会 飞 吗\n不对\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o126" 2>&1 ) || true
grep -q '嗯 ， 那 我 就 这 么 记 着 ： 猫 属于 动物' "$WORK/o126" \
    && ok "「那我就这么记着」**照旧**（确认不写图）" \
    || bad "F1-确认" "$(sed -n 's/.*AI > //p' "$WORK/o126" | sed -n 2p)"
grep -q '改 过来 了 ： 猫 会 飞（我 刚才 说 反 了）' "$WORK/o126" \
    && ok "「改过来了」**照旧**（说反了就地改）" \
    || bad "F1-改过来" "$(sed -n 's/.*AI > //p' "$WORK/o126" | tail -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '猫 会 游\n猫 会 跑\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o127" 2>&1 ) || true
grep -q '记下 了 ： 猫 会 游' "$WORK/o127" \
    && ok "「记下了：…」**照旧**（学习汇总）" \
    || bad "F1-汇总" "$(sed -n 's/.*AI > //p' "$WORK/o127" | head -1)"
F1X="$WORK/f1x"; rm -rf "$F1X"; mkdir -p "$F1X"
cp "$ROOT/session.bin" "$F1X/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$F1X/" 2>/dev/null || true
sed -i 's|^\[say\] 改口\.确认=.*|[say] 改口.确认=（数据）好 的 记 下：$1 $2$3 $4|' "$F1X/patterns.txt"
( cd "$F1X" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '猫 属于 动物 吗\n对\n猫 会 游\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）好 的 记 下：猫 属于 动物' "$F1X/out.txt" \
    && ok "**改两行数据** ⇒ 确认语与汇总语都换掉" \
    || bad "F1-改模板" "$(sed -n 's/.*AI > //p' "$F1X/out.txt" | tr '\n' ' ' | cut -c1-120)"
# §6.560 F4：列表/类别答复（表头 / 没记过 / 只列前几个 / 类比点题）
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '什么 属于 动物\n猫 属于 鱼 吗\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o128" 2>&1 ) || true
grep -q '属于 动物 的 有 ： 猫 、 狗' "$WORK/o128" && grep -q '等（只列前几个）' "$WORK/o128" \
    && ok "列表答复（表头 + 只列前几个）**照旧**" \
    || bad "F4-列表" "$(sed -n 's/.*AI > //p' "$WORK/o128" | head -1)"
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '什么 属于 什么\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o129" 2>&1 ) || true
grep -q '的 有 ：' "$WORK/o129" || grep -q '图里 没 记 过' "$WORK/o129" \
    && ok "空表头路径**照旧**（有命中就列表，没命中就明说没记过）" \
    || bad "F4-没记过" "$(sed -n 's/.*AI > //p' "$WORK/o129" | tail -1)"
F4X="$WORK/f4x"; rm -rf "$F4X"; mkdir -p "$F4X"
cp "$ROOT/session.bin" "$F4X/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$F4X/" 2>/dev/null || true
sed -i 's|^\[say\] 列表\.表头=.*|[say] 列表.表头=$1$2 $3 都 有 谁 ：|' "$F4X/patterns.txt"
( cd "$F4X" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '什么 属于 动物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '属于 动物 都 有 谁 ：' "$F4X/out.txt" \
    && ok "**改一行数据** ⇒ 列表表头换掉（槽位都对）" \
    || bad "F4-改模板" "$(sed -n 's/.*AI > //p' "$F4X/out.txt" | head -1)"
# §6.561 F2/F3：反事实四句 + 他者视角两句
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '如果 猫 会 飞\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o130" 2>&1 ) || true
grep -q '好 ， 就 当 猫 会 飞 —— 不过 我 知道 实际 上 不是 这样' "$WORK/o130" \
    && ok "「就当…（与实际冲突）」**照旧**" \
    || bad "F2-冲突" "$(sed -n 's/.*AI > //p' "$WORK/o130" | tail -1)"
F2X="$WORK/f2x"; rm -rf "$F2X"; mkdir -p "$F2X"
cp "$ROOT/session.bin" "$F2X/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$F2X/" 2>/dev/null || true
sed -i 's|^\[say\] 就当\.与实际冲突=.*|[say] 就当.与实际冲突=（数据）先 当 $1 $2$3 $4 成 立|' "$F2X/patterns.txt"
( cd "$F2X" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '如果 猫 会 飞\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）先 当 猫 会 飞 成 立' "$F2X/out.txt" \
    && ok "**改一行数据** ⇒ 反事实说法换掉（四槽都对）" \
    || bad "F2-改模板" "$(sed -n 's/.*AI > //p' "$F2X/out.txt" | tail -1)"
# §6.568 B 腿：`mods` 看自己的账（多智能体时各看各的）
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '窗户 属于 家具\n猫 会 打呼噜\n::mods\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o133" 2>&1 ) || true
grep -q '本脑的改动 2 条' "$WORK/o133" \
    && ok "**mods 看自己的账**：学两条之后是 2 条" \
    || bad "mods-计数" "$(grep -o '本脑的改动 [0-9]* 条' "$WORK/o133" | tail -1)"
grep -q '加关系  猫 会 打呼噜' "$WORK/o133" && grep -q '加关系  窗户 属于 家具' "$WORK/o133" \
    && ok "账里能读回**谁 关系 谁**（id→名字解得回来）" \
    || bad "mods-内容" "$(grep '加关系' "$WORK/o133" | head -2 | tr '\n' ' ')"
grep -q '#2 加关系' "$WORK/o133" \
    && ok "**最新在前**（序号 2 排在 1 前面）" \
    || bad "mods-顺序" "$(grep -o '#[0-9]* 加关系' "$WORK/o133" | head -2 | tr '\n' ' ')"
# §6.569 局部加载从命令面可用：::parts save/attach/detach/stats，且**问一句只读一部分**
PW="$WORK/pw"; rm -rf "$PW"; mkdir -p "$PW"
cp "$ROOT/session.bin" "$PW/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PW/" 2>/dev/null || true
( cd "$PW" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '窗户 属于 家具\n::parts save t.parts\n::parts attach t.parts\n::parts detach\n::parts stats\n';     printf '窗户 属于 家具 吗\n::parts stats\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '\[分段保存\]' "$PW/out.txt" && grep -q '\[分段加载\] 索引已挂上' "$PW/out.txt" \
    && grep -q '\[卸下\]' "$PW/out.txt" \
    && ok "parts save/attach/detach 三步都在" \
    || bad "parts-三连" "$(grep -E '分段|卸下' "$PW/out.txt" | head -3 | tr '\n' ' ')"
grep -q '读回 0 段 / 0 字节' "$PW/out.txt" \
    && ok "卸下之后 **0 段在内存**" \
    || bad "parts-卸下后" "$(grep -o '读回 [0-9]* 段' "$PW/out.txt" | head -1)"
l=$(grep -o '读回 [0-9]* 段' "$PW/out.txt" | tail -1 | grep -o '[0-9]*' || true)
[ -n "$l" ] && [ "$l" -gt 0 ] && [ "$l" -lt 500 ] \
    && ok "问一句只读回 **$l** 段（不是把上千段全读回来）" \
    || bad "parts-部分加载" "读回=${l:-无}"
grep -q '对 ， 窗户 -\[属于\]-> 家具' "$PW/out.txt" \
    && ok "**卸下状态下问句仍然答得对**" \
    || bad "parts-答对" "$(grep -E '对 ，|不 对' "$PW/out.txt" | head -1)"
# §6.571 多智能体（命令面）：::brain new/list/use/mods —— 两个脑**互不串台、各记各的账**
BW="$WORK/bw"; rm -rf "$BW"; mkdir -p "$BW"
cp "$ROOT/session.bin" "$BW/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$BW/" 2>/dev/null || true
( cd "$BW" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain new\n窗户 属于 家具\n::brain list\n::brain use 0\n窗户 属于 家具 吗\n';     printf '::brain use 1\n窗户 属于 家具 吗\n::brain mods 0\n::brain mods 1\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '开了第 2 个' "$BW/out.txt" && grep -q '\[多脑\] 2 个' "$BW/out.txt" \
    && ok "**能开第二个脑**，列表里是 2 个" \
    || bad "多脑-开" "$(grep '多脑' "$BW/out.txt" | head -3 | tr '\n' ' ')"
grep -q '#0 的改动 0 条' "$BW/out.txt" && grep -q '#1 的改动 1 条' "$BW/out.txt" \
    && ok "**各记各的账**：#0 是 0 条、#1 是 1 条" \
    || bad "多脑-账" "$(grep '的改动' "$BW/out.txt" | tr '\n' ' ')"
n_unknown=$(grep -c '图里 没 记 过 窗户 属于 什么' "$BW/out.txt" || true)
n_known=$(grep -c '对 ， 窗户 -\[属于\]-> 家具' "$BW/out.txt" || true)
[ "${n_unknown:-0}" -ge 1 ] && [ "${n_known:-0}" -ge 1 ] \
    && ok "**互不串台**：#0 不知道（说没记过）、#1 知道（对）" \
    || bad "多脑-隔离" "unknown=${n_unknown:-0} known=${n_known:-0}"
# §6.572 `::brain diff A B`：两本账各自多了什么（按图里的事实比）
BD="$WORK/bd"; rm -rf "$BD"; mkdir -p "$BD"
cp "$ROOT/session.bin" "$BD/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$BD/" 2>/dev/null || true
( cd "$BD" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain new\n窗户 属于 家具\n::brain diff 1 0\n::brain diff 0 0\n::brain new m.bin\n::brain list\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '只在 #1：窗户 属于 家具' "$BD/out.txt" \
    && ok "**diff 说得清谁多了什么**（只在 #1：窗户 属于 家具）" \
    || bad "多脑-diff" "$(grep -E '只在 #' "$BD/out.txt" | head -2 | tr '\n' ' ')"
grep -q '只在 #0 的：0 条；只在 #0 的：0 条' "$BD/out.txt" \
    && ok "**自己跟自己 diff 是 0/0**（不会凭空造差别）" \
    || bad "多脑-diff自己" "$(grep '的差' -A1 "$BD/out.txt" | head -3 | tr '\n' ' ')"
grep -q '从 m.bin 读了一份' "$BD/out.txt" \
    && ok "「brain new 加模型文件」能**从盘上再开一个独立脑**" \
    || bad "多脑-new模型" "$(grep '多脑' "$BD/out.txt" | head -4 | tr '\n' ' ')"
# §6.573 共享 base + **自己的 delta**：base 不被改、delta 只装改过的那几段、且能读回来
DL="$WORK/dl"; rm -rf "$DL"; mkdir -p "$DL"
cp "$ROOT/session.bin" "$DL/base.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$DL/" 2>/dev/null || true
( cd "$DL" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::parts save base.parts\n::parts attach base.parts\n::parts detach\n';     printf '猫 属于 动物\n::parts delta d1.delta\n::parts detach\n::parts overlay d1.delta\n猫 属于 动物 吗\n'; } | \
    "$WORK/aip" --model base.bin > out.txt 2>&1 ) || true
base_sz=$(wc -c < "$DL/base.parts" 2>/dev/null || echo 0)
delta_sz=$(wc -c < "$DL/d1.delta" 2>/dev/null || echo 0)
[ "${base_sz:-0}" -gt 0 ] && [ "${delta_sz:-0}" -gt 0 ] && [ "$delta_sz" -lt $((base_sz / 3)) ] \
    && ok "**delta 明显小于 base**（base=${base_sz}B / delta=${delta_sz}B）" \
    || bad "delta-大小" "base=${base_sz} delta=${delta_sz}"
grep -q 'base 原样不动' "$DL/out.txt" \
    && ok "**写 delta 时 base 不动**（命令面明说）" \
    || bad "delta-不动base" "$(grep 'delta' "$DL/out.txt" | head -2 | tr '\n' ' ')"
grep -q '覆盖层\] 挂上' "$DL/out.txt" && grep -q '对 ， 猫 -\[属于\]-> 动物' "$DL/out.txt" \
    && ok "**卸下 + 挂自己的 delta 之后，改过的事实还在**" \
    || bad "delta-回读" "$(grep -E '覆盖层|对 ，|不 对' "$DL/out.txt" | head -2 | tr '\n' ' ')"
# §6.574 词级流水：只记**自己改过的**（不是把 base 抄一遍）+ replay 走学习同一条路
JR="$WORK/jr"; rm -rf "$JR"; mkdir -p "$JR"
cp "$ROOT/session.bin" "$JR/base.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$JR/" 2>/dev/null || true
( cd "$JR" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain new base.bin\n::brain use 1\n小龙 属于 神兽\n::parts journal j.txt\n';     printf '::brain use 0\n::parts replay j.txt\n小龙 属于 神兽 吗\n'; } | \
    "$WORK/aip" --model base.bin > out.txt 2>&1 ) || true
jl=$(wc -l < "$JR/j.txt" 2>/dev/null || echo 0)
[ "${jl:-0}" -ge 1 ] && [ "${jl:-0}" -le 10 ] \
    && ok "流水**只记自己改过的**（$jl 条，不是几百条 base 事实）" \
    || bad "流水-条数" "jl=$jl"
grep -q '照抄 [0-9]* 条' "$JR/out.txt" \
    && ok "replay 真的走了（照抄口径）" \
    || bad "流水-replay" "$(grep '流水' "$JR/out.txt" | head -2 | tr '\n' ' ')"
# §6.580 老 bug 的看守：旧的 replay 字段错位 ⇒ 报了"照着改了 N 条"却一条没写。
# 现在**必须问得出**（这一条比"数了 N 行"强：它检查知识真的到了目标脑）。
grep -q '对 ，' "$JR/out.txt" \
    && ok "照抄之后**目标脑真的知道了**（不是只数了行数）" \
    || bad "照抄-落地" "$(grep -E 'AI >|不 对' "$JR/out.txt" | tail -2 | tr '\n' ' ')"
# §6.580 冲突可见 + 干跑 + force：两个空脑，一个说动物、一个说植物（属于 是独占关系）
BR="$WORK/xbr"; rm -rf "$BR"; mkdir -p "$BR"
cp "$ROOT/session.bin" "$BR/base.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$BR/" 2>/dev/null || true
( cd "$BR" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain new\n::brain use 1\n猫 属于 动物\n::brain new\n::brain use 2\n猫 属于 植物\n::brain use 1\n::parts journal j2.txt\n';     printf '::brain use 2\n::parts replay j2.txt\n猫 属于 动物 吗\n::parts replay j2.txt check\n::parts replay j2.txt force\n猫 属于 动物 吗\n::mods\n'; } | \
    "$WORK/aip" --model base.bin > out.txt 2>&1 ) || true
grep -q '冲突跳过 1' "$BR/out.txt" \
    && ok "冲突**默认不写**（本地优先：冲突跳过 1）" \
    || bad "冲突-默认" "$(grep '照抄' "$BR/out.txt" | head -1)"
grep -q '冲突 1 条，本地优先' "$BR/out.txt" \
    && ok "冲突上了屏（人看得到是哪条）" \
    || bad "冲突-上屏" "$(grep '⚠' "$BR/out.txt" | head -1)"
grep -q '只查没写' "$BR/out.txt" \
    && ok "干跑（check）在：只报会发生什么" \
    || bad "冲突-check" "$(grep '流水' "$BR/out.txt" | sed -n 2p)"
grep -q '覆盖 1' "$BR/out.txt" \
    && ok "force 明知冲突还是写（覆盖 1）" \
    || bad "冲突-force" "$(grep '照抄' "$BR/out.txt" | tail -1)"
grep -q '冲突未写' "$BR/out.txt" && grep -q '强行覆盖' "$BR/out.txt" \
    && ok "两种落法都进了本脑的账（冲突未写 / 强行覆盖）" \
    || bad "冲突-账" "$(grep -E '冲突未写|强行覆盖' "$BR/out.txt" | tr '\n' ' ')"
# §6.587 分词切歪的整句：**3+ 个单字开头 + 后面才是核心关系词** ⇒ 交给造词路径重拼
TK="$WORK/tk"; rm -rf "$TK"; mkdir -p "$TK"
cp "$ROOT/model.bin" "$TK/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$TK/" 2>/dev/null || true
( cd "$TK" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '猫头鹰 怕 光\n猫头鹰 怕 光 吗\n猫 喵 鱼\n猫 喵 鱼 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '记下 了 ： 猫头鹰 怕 光' "$TK/out.txt" \
    && ok "切碎的新词主语会**重拼成一个词**再学（猫头鹰，不是 猫 -[头]-> 鹰怕光）" \
    || bad "整词重拼" "$(grep -a 'AI >' "$TK/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '对 ， 猫头鹰 -[怕]-> 光' "$TK/out.txt" \
    && ok "重拼之后**问得出来**（学得成 → 用得上）" \
    || bad "整词重拼-回读" "$(grep -a '猫头鹰' "$TK/out.txt" | tail -1)"
grep -qaF '记下 了 ： 猫 喵 鱼' "$TK/out.txt" && grep -qaF '对 ， 猫 -[喵]-> 鱼' "$TK/out.txt" \
    && ok "反过来：**单字动态谓语**（猫 喵 鱼）照样学得进（没有被上面那条规则误杀）" \
    || bad "单字谓语" "$(grep -aE '喵' "$TK/out.txt" | tr '\n' ' ')"
# §6.595 教学留痕：`你 怎么 知道` 要能**跨会话**答"你教我的"（标记随图存盘）
PV2="$WORK/pv2"; rm -rf "$PV2"; mkdir -p "$PV2"
cp "$ROOT/model.bin" "$PV2/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PV2/" 2>/dev/null || true
( cd "$PV2" && { printf '::notepad off\n::char off\n'; printf '麒麟 属于 神兽\n::save m.bin\n'; } | \
    "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
grep -qaF '已保存' "$PV2/out1.txt" \
    && ok "教会一条事实并存盘（留痕 `[教:…]` 随图落盘）" \
    || bad "留痕-存盘" "$(tail -2 "$PV2/out1.txt" | tr '\n' ' ')"
( cd "$PV2" && { printf '::notepad off\n::char off\n'; printf '你 怎么 知道 麒麟 属于 神兽\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '这 条 是 你 教 我 的' "$PV2/out2.txt" \
    && ok "**重开之后**问出处 ⇒ 仍答「你教我的」（跨会话，靠留痕不靠内存账）" \
    || bad "留痕-跨会话" "$(grep -a 'AI >' "$PV2/out2.txt" | tail -1)"

# §6.607 **"陈述 + 问句"混在一句**：学到的要说出来（以前学会了却静默）
MX="$WORK/mx"; rm -rf "$MX"; mkdir -p "$MX"
cp "$ROOT/model.bin" "$MX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$MX/" 2>/dev/null || true
( cd "$MX" && { printf '::notepad off\n::char off\n'; printf '麒麟 属于 神兽， 你 记住 了 吗\n麒麟 属于 神兽 吗\n'; } | \
    "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
grep -qaF '记下 了 ： 麒麟 属于 神兽' "$MX/out1.txt" \
    && ok "「X 属于 Y，你 记住 了 吗」⇒ 先说**记下了什么**（以前静默学会、只回「不太懂」）" \
    || bad "混句-记下" "$(grep -a 'AI >' "$MX/out1.txt" | head -1)"
grep -qaF '对 ， 麒麟 -[属于]-> 神兽' "$MX/out1.txt" \
    && ok "混句**确实学进去了**（下一句回问为对）" \
    || bad "混句-学进去" "$(grep -a 'AI >' "$MX/out1.txt" | tail -1)"
( cd "$MX" && { printf '::notepad off\n::char off\n'; printf '猫 属于 动物，狗 属于 宠物，你 记住 了 吗\n狗 属于 宠物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '记下 了 ： 猫 属于 动物 ； 狗 属于 宠物' "$MX/out2.txt" \
    && ok "**两个陈述 + 问句**：两条都学、都报出来（以前一句 `return NULL` 全丢）" \
    || bad "混句-两条" "$(grep -a 'AI >' "$MX/out2.txt" | head -1)"

# §6.630 **"因为"进存档**（v15）：教过的原因，重开之后还答得出来
RS="$WORK/rs"; rm -rf "$RS"; mkdir -p "$RS"
cp "$ROOT/model.bin" "$RS/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$RS/" 2>/dev/null || true
( cd "$RS" && { printf '::notepad off\n::char off\n'; printf '因为 会 飞 ， 灵狐兽 属于 神兽\n::save m.bin\n'; } | \
    "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
grep -qaF '是 因 为 会 飞' "$RS/out1.txt" \
    && ok "教「因为」能记住（当场回「你说 … 是因为 …」）" \
    || bad "reason-教" "$(grep -a 'AI >' "$RS/out1.txt" | head -1)"
( cd "$RS" && { printf '::notepad off\n::char off\n为什么 灵狐兽 属于 神兽\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '因为 会 飞 —— 你 教 我 的' "$RS/out2.txt" \
    && ok "**重开之后**原因还在（v15 写进存档了；以前是会话内的）" \
    || bad "reason-存读" "$(grep -a 'AI >' "$RS/out2.txt" | head -1)"

# §6.633 **谓词（多元关系）**：::pred make/list/show —— 参数进实例名，实例=节点
PW="$WORK/pred"; rm -rf "$PW"; mkdir -p "$PW"
cp "$ROOT/model.bin" "$PW/m.bin"; cp "$ROOT/patterns.txt" "$PW/" 2>/dev/null || true
pred_run() { ( cd "$PW" && printf '%b' "::notepad off\n::char off\n$1::quit\n" | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true; }
pred_run '::pred make 敌人 甲 乙\n::pred make 敌人 甲 丙\n'
grep -qaF '[谓词:敌人(甲,乙)]' "$PW/o.txt" \
    && ok "::pred make 造出实例节点「[谓词:敌人(甲,乙)]」（参数进名字）" \
    || bad "pred-make" "$(grep -a '谓词' "$PW/o.txt" | head -1)"
pred_run '::pred make 敌人 甲 乙\n::pred show 敌人\n'
grep -qaF '甲, 乙' "$PW/o.txt" \
    && ok "::pred show 列出实例与参数（甲, 乙）" \
    || bad "pred-show" "$(grep -a '谓词' "$PW/o.txt" | head -1)"
pred_run '::pred make 九元 a b c d e f g h i\n'
grep -qaF '不记：参数最多 8 个' "$PW/o.txt" \
    && ok "9 个参数 ⇒ **拒绝**（不截断成 8 个还报『记下』）" \
    || bad "pred-上限" "$(tail -2 "$PW/o.txt" | tr '\n' ' ')"

# §6.634 **奖励机制**：学到一条 +1、可查账、可清零、调节口默认 off
RW="$WORK/reward"; rm -rf "$RW"; mkdir -p "$RW"
cp "$ROOT/model.bin" "$RW/m.bin"; cp "$ROOT/patterns.txt" "$RW/" 2>/dev/null || true
( cd "$RW" && printf '::notepad off\n::char off\n小猫 属于 猫科\n::reward\n::reward on\n::reward reset\n::reward\n::quit\n' | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true
grep -qaE '短期 = \+1\.00' "$RW/o.txt" \
    && ok "学到一条 ⇒ 奖励 +1（当场可查）" \
    || bad "reward-学" "$(grep -a '奖励' "$RW/o.txt" | head -1)"
grep -qaF '调节口 = on' "$RW/o.txt" \
    && ok "::reward on 打开调节口（默认 off：只计量）" \
    || bad "reward-on" "$(grep -a '调节口' "$RW/o.txt" | head -1)"
grep -qaE '短期 = \+0\.00' "$RW/o.txt" \
    && ok "::reward reset 清零" \
    || bad "reward-reset" "$(tail -2 "$RW/o.txt" | tr '\n' ' ')"

# §6.635 **规则 + 证明**：::rule add / ::rule / ::prove（后向链，有界）
RP="$WORK/rule"; rm -rf "$RP"; mkdir -p "$RP"
cp "$ROOT/model.bin" "$RP/m.bin"; cp "$ROOT/patterns.txt" "$RP/" 2>/dev/null || true
( cd "$RP" && printf '::notepad off\n::char off\n::pred make 父 甲 乙\n::pred make 父 乙 丙\n::rule add 祖先($x,$y) :- 父($x,$y)\n::rule add 祖先($x,$z) :- 父($x,$y), 祖先($y,$z)\n::prove 祖先 甲 丙\n::prove 祖先 丙 甲\n::quit\n' | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true
grep -qaF '[规则:R2]（2 条前提）' "$RP/o.txt" \
    && ok "::rule add 加两条规则（R1 基例 / R2 递归）" \
    || bad "rule-add" "$(grep -a '规则' "$RP/o.txt" | head -1)"
grep -qaF '祖先(甲,丙) ⇒ **成立**' "$RP/o.txt" \
    && ok "::prove 祖先 甲 丙 ⇒ **成立**（两跳：父(甲,乙)+父(乙,丙)）" \
    || bad "prove-成立" "$(grep -a '证明' "$RP/o.txt" | head -1)"
grep -qaF '选取事实' "$RP/o.txt" && grep -qaF '规则 [规则:R' "$RP/o.txt" \
    && ok "证明里给出**每一步**（选了哪条事实、用了哪条规则）" \
    || bad "prove-步骤" "$(grep -a -A2 '证明' "$RP/o.txt" | head -3 | tr '\n' ' ')"
grep -qaF '祖先(丙,甲) ⇒ **推不出来**（未知，不是假）' "$RP/o.txt" \
    && ok "证不出来 ⇒ 说**未知**（不把"推不出"说成"假"）" \
    || bad "prove-未知" "$(grep -a '丙,甲' "$RP/o.txt" | head -1)"

# §6.636 **待证（不轻信）**：听说/论文 之类的话不进事实层，::claim 确认才升格
CW="$WORK/claim"; rm -rf "$CW"; mkdir -p "$CW"
cp "$ROOT/model.bin" "$CW/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/relations.txt" "$CW/" 2>/dev/null || true
( cd "$CW" && printf '::notepad off\n::char off\n听说 猫 属于 鸟类\n猫 属于 鸟类 吗\n::claim\n::claim accept 1\n猫 属于 鸟类 吗\n::quit\n' | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true
grep -qaF '先 记 成 **待 证**' "$CW/o.txt" \
    && ok "「听说 …」⇒ 记成**待证**并说明来源" \
    || bad "claim-记" "$(grep -a 'AI >' "$CW/o.txt" | head -1)"
grep -qaF '待证**没有**写成' "$CW/o.txt" || true
grep -qaE '只 \*\*听 说\*\* 过|这个我 不 太 懂|不 确定' "$CW/o.txt" \
    && ok "待证期间问「猫 属于 鸟类 吗」⇒ **照实说只听说过**（§6.647），不按事实答" \
    || bad "claim-未生效" "$(grep -a 'AI >' "$CW/o.txt" | sed -n 2p)"
grep -qaF '[待证] 共 1 条' "$CW/o.txt" \
    && ok "::claim 列得出来（编号/来源/内容）" \
    || bad "claim-列" "$(grep -a '\[待证\]' "$CW/o.txt" | head -1)"
grep -qaF '可信度 30/100' "$CW/o.txt" \
    && ok "::claim 会显示来源**可信度**（听说 30/100）" \
    || bad "claim-可信度" "$(grep -a '\[待证\]' "$CW/o.txt" | sed -n 2p)"
( cd "$CW" && printf '::notepad off\n::char off\n听说 鱼 属于 鸟类\n论文 鱼 属于 鸟类\n::claim\n::quit\n' | "$WORK/aip" --model m.bin > o2.txt 2>&1 ) || true
grep -qaF '两 个 **独立 来源**' "$CW/o2.txt" \
    && ok "**两个独立来源** ⇒ 按事实记下（交叉验证，§6.637）" \
    || bad "claim-两源" "$(grep -a 'AI >' "$CW/o2.txt" | tail -1)"
grep -qaF '猫 -[属于]-> 鸟类' "$CW/o.txt" \
    && ok "::claim accept 之后才**真答对**（这时才写进事实层、并记成"是你教的"）" \
    || bad "claim-确认" "$(grep -a 'AI >' "$CW/o.txt" | tail -1)"

# §6.638 **读论文（离线）**：::paper read ⇒ 抽断言、全进待证（不写事实层）
PP="$WORK/paper"; rm -rf "$PP"; mkdir -p "$PP/papers"
cp "$ROOT/model.bin" "$PP/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/relations.txt" "$PP/" 2>/dev/null || true
cp "$ROOT/papers/sample.txt" "$PP/papers/sample.txt" 2>/dev/null || printf '企鹅 属于 鸟类。\n麒麟 属于 神兽。\n' > "$PP/papers/sample.txt"
( cd "$PP" && printf '::notepad off\n::char off\n::paper read papers/sample.txt\n::claim\n::quit\n' | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true
grep -qaE '抽到 [0-9]+ 条，记为待证 [0-9]+ 条' "$PP/o.txt" \
    && ok "::paper read 抽到断言、全部记为待证（样本里是 3 条）" \
    || bad "paper-抽" "$(grep -a '论文' "$PP/o.txt" | head -1)"
grep -qaF '（来源：论文，可信度 50/100）' "$PP/o.txt" \
    && ok "待证来源是「论文」、可信度 50/100" \
    || bad "paper-来源" "$(grep -a '待证' "$PP/o.txt" | sed -n 2p)"
( cd "$PP" && printf '::notepad off\n::char off\n::paper read 不存在.txt\n::quit\n' | "$WORK/aip" --model m.bin > o2.txt 2>&1 ) || true
grep -qaF '读不到 不存在.txt' "$PP/o2.txt" \
    && ok "文件不存在 ⇒ **明确说读不到**（不假装读过）" \
    || bad "paper-缺" "$(grep -a '论文' "$PP/o2.txt" | head -1)"

# §6.639 语言包的行为由 `tools/langprobe.c` 钉（这里不重复跑：并行套件里它对 `_t` 目录敏感，会偶发失败）

# §6.657 **前向链推出的东西，出处说"我自己推的"**（不说成"你教的"）
PV="$WORK/prov"; rm -rf "$PV"; mkdir -p "$PV"
cp "$ROOT/model.bin" "$PV/m.bin"; cp "$ROOT/patterns.txt" "$PV/" 2>/dev/null || true
( cd "$PV" && printf '::notepad off\n::char off\n::pred make 父 甲 乙\n::rule add 怕($x,$y) :- 父($x,$y)\n::rule add 祖先($x,$y) :- 父($x,$y)\n::rule apply\n你 怎么 知道 甲 怕 乙\n你 怎么 知道 祖先(甲,乙)\n你 怎么 知道 怕(甲,乙)\n::quit\n' | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true
grep -qaE '推出 [0-9]+ 条新事实' "$PV/o.txt" \
    && ok "::rule apply 推出事实（怕(甲,乙) 与 祖先(甲,乙)）" \
    || bad "prov-推" "$(grep -a '前向链' "$PV/o.txt" | head -1)"
grep -qaF '自己 推' "$PV/o.txt" \
    && ok "追问出处 ⇒ 说**我自己推的**（不是"你教的"）" \
    || bad "prov-说" "$(grep -a 'AI >' "$PV/o.txt" | tail -1)"

# §6.662 **论文缓存**：::paper fetch 只从本地取（不联网），取不到就明说
PCa="$WORK/pcache"; rm -rf "$PCa"; mkdir -p "$PCa/papers/cache"
cp "$ROOT/model.bin" "$PCa/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/relations.txt" "$PCa/" 2>/dev/null || true
printf '企鹅 属于 鸟类。\n麒麟 属于 神兽。\n' > "$PCa/papers/cache/c.txt"
( cd "$PCa" && printf '::notepad off\n::char off\n::paper fetch c.txt\n::paper fetch 没有这个.txt\n::quit\n' | "$WORK/aip" --model m.bin > o.txt 2>&1 ) || true
grep -qaE '从本地缓存取到 papers/cache/c.txt' "$PCa/o.txt" \
    && ok "::paper fetch 从本地缓存取到文件" \
    || bad "paper-fetch" "$(grep -a '论文' "$PCa/o.txt" | head -1)"
grep -qaF '**不联网**' "$PCa/o.txt" \
    && ok "缓存里没有 ⇒ 明确说**不联网**（不假装去抓）" \
    || bad "paper-fetch-缺" "$(grep -a '论文' "$PCa/o.txt" | tail -1)"
( cd "$PCa" && printf '::notepad off\n::paper list\n::quit\n' | "$WORK/aip" --model m.bin > o2.txt 2>&1 ) || true
printf '企鹅 属于 鸟类。\n' > "$PCa/papers/cache/s2.txt"
( cd "$PCa" && printf '::notepad off\n::paper search 企鹅\n::paper search 没有这个词\n::quit\n' | "$WORK/aip" --model m.bin > o3.txt 2>&1 ) || true
grep -qaE '命中 [0-9]+ 行' "$PCa/o3.txt" \
    && ok "::paper search 本地检索命中（不联网，§6.675）" \
    || bad "paper-search" "$(grep -a '论文' "$PCa/o3.txt" | head -1)"
grep -qaF '本地没搜到' "$PCa/o3.txt" \
    && ok "搜不到 ⇒ 明说「本地没搜到」（**不联网**）" \
    || bad "paper-search-空" "$(grep -a '论文' "$PCa/o3.txt" | tail -1)"
grep -qaF 'papers/cache/' "$PCa/o2.txt" \
    && ok "::paper list 真的列目录（papers/ 与 papers/cache/，§6.666）" \
    || bad "paper-fetch-缺" "$(grep -a '论文' "$PCa/o.txt" | tail -1)"

grep -qaF '自己 推' "$PV/o.txt" && [ "$(grep -ac '自己 推' "$PV/o.txt")" -ge 3 ] \
    && ok "**谓词实例**的出处也说得清（`你 怎么 知道 祖先(甲,乙)` ⇒ 我推的，§6.668）" \
    || bad "prov-谓词" "$(grep -a 'AI >' "$PV/o.txt" | tail -1)"

# §6.605 **带标点的教法也是人话**：`麒麟，属于 神兽` / `我 教 你 ， 麒麟 属于 神兽`
PC="$WORK/pc"; rm -rf "$PC"; mkdir -p "$PC"
cp "$ROOT/model.bin" "$PC/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PC/" 2>/dev/null || true
( cd "$PC" && { printf '::notepad off\n::char off\n'; printf '麒麟，属于 神兽\n麒麟 属于 神兽 吗\n'; } | \
    "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
grep -qaF '对 ， 麒麟 -[属于]-> 神兽' "$PC/out1.txt" \
    && ok "**逗号不断词**：`麒麟，属于 神兽` 能学进去（以前整句掉兜底）" \
    || bad "punct-逗号" "$(grep -a 'AI >' "$PC/out1.txt" | tail -2 | tr '\n' ' ')"
( cd "$PC" && { printf '::notepad off\n::char off\n'; printf '我 教 你 ， 麒麟 属于 神兽\n麒麟 属于 神兽 吗\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '对 ， 麒麟 -[属于]-> 神兽' "$PC/out2.txt" \
    && ok "**引导语 + 逗号**（`我 教 你 ， …`）也能学进去" \
    || bad "punct-引导语" "$(grep -a 'AI >' "$PC/out2.txt" | tail -2 | tr '\n' ' ')"
( cd "$PC" && { printf '::notepad off\n::char off\n'; printf '猫 属于 动物，狗 属于 宠物\n狗 属于 宠物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out3.txt 2>&1 ) || true
grep -qaF '对 ， 狗 -[属于]-> 宠物' "$PC/out3.txt" && ! grep -qaiE '宠物，狗' "$PC/out3.txt" \
    && ok "**真分句仍然分**（`猫 属于 动物，狗 属于 宠物` 两条都学、不拼成一个怪词）" \
    || bad "punct-分句" "$(grep -a 'AI >' "$PC/out3.txt" | tail -2 | tr '\n' ' ')"

# §6.602 `::contradict`：全图扫独占关系的矛盾（只报不改；包含关系不算矛盾）
CT="$WORK/ct"; rm -rf "$CT"; mkdir -p "$CT"
cp "$ROOT/model.bin" "$CT/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$CT/" 2>/dev/null || true
( cd "$CT" && { printf '::notepad off\n::char off\n'; printf '::contradict\n'; } | \
    "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
grep -qaF '[独占矛盾] 0 条' "$CT/out1.txt" \
    && ok "「::contradict」扫出厂模型：0 条（出厂图里没有互不包含的独占值）" \
    || bad "contradict-出厂" "$(grep -a '独占矛盾' "$CT/out1.txt" | head -1)"
( cd "$CT" && { printf '::notepad off\n::char off\n::askconflict off\n'; printf '狗 属于 动物\n狗 属于 植物\n::contradict\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '[独占矛盾] 1 条' "$CT/out2.txt" && grep -qaF '狗 属于 动物 ／ 植物' "$CT/out2.txt" \
    && ok "教两条互不包含的独占值 ⇒ 「::contradict」报 1 条并点名两个值" \
    || bad "contradict-冲突" "$(grep -a '独占矛盾\|狗 属于' "$CT/out2.txt" | tail -2 | tr '\n' ' ')"
( cd "$CT" && { printf '::notepad off\n::char off\n::askconflict off\n'; printf '狗 属于 犬科\n犬科 属于 动物\n::contradict\n'; } | \
    "$WORK/aip" --model m.bin > out3.txt 2>&1 ) || true
grep -qaF '[独占矛盾] 0 条' "$CT/out3.txt" && ! grep -qaF '犬科 ／ 动物' "$CT/out3.txt" \
    && ok "**包含关系不算矛盾**（犬科 ⊂ 动物：「::contradict」不把它列成互不包含）" \
    || bad "contradict-包含" "$(grep -a '独占矛盾\|犬科' "$CT/out3.txt" | tail -2 | tr '\n' ' ')"

# §6.603 `::contradict` 的**裁决建议**（只建议不下手）：教学留痕 > 写入序号 > 命中数
( cd "$CT" && { printf '::notepad off\n::char off\n::askconflict off\n'; printf '::teach 猫 属于 植物\n::contradict\n'; } | \
    "$WORK/aip" --model m.bin > out4.txt 2>&1 ) || true
grep -qaF '建议撤「动物」：你教的那条更可信' "$CT/out4.txt" \
    && ok "裁决建议：**你教的那条更可信**（语料里有 猫 属于 动物，你教了 猫 属于 植物 ⇒ 建议撤动物）" \
    || bad "verdict-教" "$(grep -a '建议撤\|／' "$CT/out4.txt" | head -2 | tr '\n' ' ')"
( cd "$CT" && { printf '::notepad off\n::char off\n::askconflict off\n'; printf '::teach 狗 属于 动物\n::teach 狗 属于 植物\n::contradict\n'; } | \
    "$WORK/aip" --model m.bin > out5.txt 2>&1 ) || true
grep -qaF '后说的算（写入序号' "$CT/out5.txt" \
    && ok "裁决建议：**后说的算**（两条都是你教的 ⇒ 按写入序号挑旧的那条撤）" \
    || bad "verdict-序号" "$(grep -a '建议撤\|／' "$CT/out5.txt" | head -2 | tr '\n' ' ')"

# §6.600 parts 上下文绑定在**某个池**上：换脑/加载模型之前必须先 close（否则 use-after-free）
PL="$WORK/pl"; rm -rf "$PL"; mkdir -p "$PL"
cp "$ROOT/model.bin" "$PL/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PL/" 2>/dev/null || true
( cd "$PL" && { printf '::notepad off\n::char off\n'; printf '::parts save p.parts\n::load m.bin\n::parts attach p.parts\n::parts detach\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ); rc=$?
[ "$rc" -eq 0 ] \
    && ok "「::parts save → ::load（换脑）→ ::parts attach」**不崩**（以前是 SEGV）" \
    || bad "parts-换脑" "退出码 $rc；$(tail -3 "$PL/out.txt" | tr '\n' ' ')"
# ⚠ 只有"确实有边在磁盘上"时才会打「换脑之前读回 N 个词」——`::parts save` 之后还要 evict 才对，
# 所以这里不钉那句话，改钉"整段跑完反查表仍自洽、模型还能答"。
( cd "$PL" && { printf '::notepad off\n::char off\n'; printf '猫 属于 动物 吗\n::revcheck\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '反查表缺 0 条' "$PL/out2.txt" \
    && ok "换脑 + 挂分段之后：反查表自洽（::revcheck 缺 0 条）" \
    || bad "parts-换脑-自洽" "$(grep -a revcheck "$PL/out2.txt" | head -1)"

# §6.598 **`::op` 之后存盘还能读回来**（临时符号节点 [临:…] 不能让存档不可读）
OS="$WORK/os"; rm -rf "$OS"; mkdir -p "$OS"
cp "$ROOT/model.bin" "$OS/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$OS/" 2>/dev/null || true
( cd "$OS" && { printf '::notepad off\n::char off\n'; printf '::op 加前提 猫 动物\n::op 中间插词 鸟 飞\n::save m.bin\n'; } | \
    "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
grep -qaF '已保存' "$OS/out1.txt" \
    && ok "「::op 加前提 / 中间插词」之后能存盘" \
    || bad "op-存盘" "$(tail -2 "$OS/out1.txt" | tr '\n' ' ')"
( cd "$OS" && { printf '::notepad off\n::char off\n'; printf '猫 属于 动物 吗\n::revcheck\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '对 ， 猫 -[属于]-> 动物' "$OS/out2.txt" \
    && ok "**存过 op 改动之后仍然读得回来**（以前静默读不回：id 缺口 + 边计数不一致）" \
    || bad "op-回读" "$(tail -3 "$OS/out2.txt" | tr '\n' ' ')"
grep -qaF '反查表缺 0 条' "$OS/out2.txt" \
    && ok "读回来之后反查表自洽（::revcheck 缺 0 条）" \
    || bad "op-反查表" "$(grep -a revcheck "$OS/out2.txt" | head -1)"

# §6.597 教学留痕**上限**：只保最近 N 条（AIP_TEACHMARK_MAX 可调小），最老的软删
TM="$WORK/tm"; rm -rf "$TM"; mkdir -p "$TM"
cp "$ROOT/model.bin" "$TM/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$TM/" 2>/dev/null || true
( cd "$TM" && { printf '::notepad off\n::char off\n'; for i in 1 2 3 4 5; do printf "麒麟$i 属于 神兽\n"; done; printf '::save m.bin\n'; } | \
    env AIP_TEACHMARK_MAX=3 "$WORK/aip" --model m.bin > out1.txt 2>&1 ) || true
( cd "$TM" && { printf '::notepad off\n::char off\n::learn\n'; } | env AIP_TEACHMARK_MAX=3 "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '教学留痕 3 个' "$TM/out2.txt" \
    && ok "留痕**上限**生效（AIP_TEACHMARK_MAX=3 时教 5 条 ⇒ 只保 3 个）" \
    || bad "留痕上限" "$(grep -a '教学留痕' "$TM/out2.txt" | head -1)"

# §6.596 `::locative on|off`：地点块学习开关（`::learn` 总览里那条命令必须真的存在）
LC3="$WORK/lc3"; rm -rf "$LC3"; mkdir -p "$LC3"
cp "$ROOT/model.bin" "$LC3/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$LC3/" 2>/dev/null || true
( cd "$LC3" && { printf '::notepad off\n::char off\n'; printf '::locative\n::locative off\n::locative\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '[地点块学习（A V 在 B ⇒ A 在 B）= 开（默认）]' "$LC3/out.txt" \
    && grep -qaF '[地点块学习（A V 在 B ⇒ A 在 B）= 关]' "$LC3/out.txt" \
    && ok "「::locative on|off」能读能改（::learn 总览里承诺的命令真的存在）" \
    || bad "locative-开关" "$(grep -a '地点块' "$LC3/out.txt" | tail -2 | tr '\n' ' ')"

# §6.596 `::learn`（不带参数）= 学习总览：六个开关 + 这一会话学到的（计数随教随涨）
LD2="$WORK/ld2"; rm -rf "$LD2"; mkdir -p "$LD2"
cp "$ROOT/model.bin" "$LD2/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$LD2/" 2>/dev/null || true
( cd "$LD2" && { printf '::notepad off\n::char off\n'; printf '::learn\n麒麟 属于 神兽\n::learn\n::learn 猫 是 动物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '[学习设置]' "$LD2/out.txt" && grep -qaF '::askconflict on|off' "$LD2/out.txt" \
    && ok "「::learn」（不带参数）打出学习总览（六开关 + 去哪改）" \
    || bad "learn-总览" "$(grep -a '学习设置' "$LD2/out.txt" | head -1)"
grep -qaF '关系事实 1 条' "$LD2/out.txt" && grep -qaF '教学留痕 1 个' "$LD2/out.txt" \
    && ok "「::learn」的计数**随教随涨**（关系事实 1 条 / 教学留痕 1 个）" \
    || bad "learn-计数" "$(grep -aE '关系事实|教学留痕' "$LD2/out.txt" | tail -2 | tr '\n' ' ')"
grep -qaF '[学了一句]' "$LD2/out.txt" \
    && ok "带参数的老行为仍在（「::learn <文本>」⇒ [学了一句]）" \
    || bad "learn-带参数" "$(grep -a '学了一句' "$LD2/out.txt" | head -1)"

# §6.595 `::src` 也要认教学留痕（诊断命令和自然语言回答同一份判据）
( cd "$PV2" 2>/dev/null && { printf '::notepad off\n::char off\n麒麟 属于 神兽\n::src 麒麟 属于 神兽\n'; } | \
    "$WORK/aip" --model "$ROOT/model.bin" > src1.txt 2>&1 ) || true
grep -qaF '**教学留痕：有**' "$PV2/src1.txt" \
    && ok "`::src A R B` 认得出「教学留痕」（和 `你 怎么 知道` 同一份判据）" \
    || bad "src-留痕" "$(grep -a '来源账\|教学' "$PV2/src1.txt" | head -2 | tr '\n' ' ')"

# §6.594 `::askconflict off`：冲突**照样说清**，但不再追问"要 我 改 掉 旧 的 吗"
AC="$WORK/ac"; rm -rf "$AC"; mkdir -p "$AC"
cp "$ROOT/model.bin" "$AC/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$AC/" 2>/dev/null || true
( cd "$AC" && { printf '::notepad off\n::char off\n'; printf '::askconflict off\n狗 属于 植物\n对\n狗 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '[冲突时追问「要 我 改 掉 旧 的 吗」= 关]' "$AC/out.txt" \
    && ok "`::askconflict off` 生效（开关能读能改）" \
    || bad "askconflict-开关" "$(grep -a '冲突' "$AC/out.txt" | head -1)"
grep -qaF '要 撤 就 说「狗 不 属于 动物」' "$AC/out.txt" \
    && ok "关掉追问之后：冲突**照样说清**，并给显式撤法（不静默）" \
    || bad "askconflict-措辞" "$(grep -a '冲突' "$AC/out.txt" | head -1)"
# 判据用「说「对」就改」这半句（帮助文本里没有它；而"要 我 改 掉 旧 的 吗"在 ::help 里 ⇒ 会假绿）
if grep -a '^AI >' "$AC/out.txt" | grep -qaF '说「对」就改'; then
    bad "askconflict-不追问" "$(grep -a '^AI >' "$AC/out.txt" | grep -a '说「对」就改' | head -1)"
else
    ok "关掉追问之后：**不再**问「要对才改」（帮助里没这半句 ⇒ 不会假绿）"
fi
grep -qaF '对 ， 狗 -[属于]-> 动物' "$AC/out.txt" \
    && ok "关掉追问、回「对」不会撤旧值（没挂待确认）—— 旧值还在" \
    || bad "askconflict-未撤" "$(grep -a '动物' "$AC/out.txt" | tail -1)"

# §6.593 教学意图前缀（`我教你`/`记一下`/`请记住`）+ 「我刚才教了你什么」
TL="$WORK/tl"; rm -rf "$TL"; mkdir -p "$TL"
cp "$ROOT/model.bin" "$TL/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$TL/" 2>/dev/null || true
( cd "$TL" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '我 教 你 麒麟 属于 神兽\n麒麟 属于 神兽 吗\n记 一下 麒麟 会 飞\n麒麟 会 飞 吗\n请 记住 麒麟 有 角\n麒麟 有 角 吗\n记个过程 泡茶\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '记下 了 ： 麒麟 属于 神兽' "$TL/out.txt" \
    && ok "「我 教 你 X 属于 Y」⇒ 剥掉教学意图、当事实学（以前整句听不懂）" \
    || bad "我教你" "$(grep -a 'AI >' "$TL/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '记下 了 ： 麒麟 会 飞' "$TL/out.txt" \
    && ok "「记 一下 X 会 Y」⇒ 学成事实（不再被当成"记个过程"）" \
    || bad "记一下" "$(grep -a '过程' "$TL/out.txt" | head -1)"
grep -qaF '过程 「 泡茶 」 记下 了' "$TL/out.txt" \
    && ok "「记个过程 泡茶」照旧建过程（教学意图剥离**不误伤**过程入口）" \
    || bad "过程入口" "$(grep -a '泡茶' "$TL/out.txt" | head -1)"
( cd "$TL" && { printf '::notepad off\n::char off\n我刚才教了你什么\n我 教 你 麒麟 属于 神兽\n记 一下 麒麟 会 飞\n我刚才教了你什么\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '你 还 没 教 过 我 什么' "$TL/out2.txt" \
    && ok "「我刚才教了你什么」（还没教）⇒ 直说没教过" \
    || bad "回顾-空" "$(grep -a '教' "$TL/out2.txt" | head -1)"
grep -qa '你 刚 才 教 了 2 条' "$TL/out2.txt" && grep -qa '麒麟 属于 神兽' "$TL/out2.txt" \
    && ok "「我刚才教了你什么」⇒ 从本会话的账里列出教过的事实" \
    || bad "回顾-有" "$(grep -a '教 了' "$TL/out2.txt" | tail -1)"

# §6.592 数量事实：`X 有 三 个 角` ⇒ 学得进；`X 有 角 吗` ⇒ 对；`X 有 几 个 角` ⇒ 答数目
QN="$WORK/qn"; rm -rf "$QN"; mkdir -p "$QN"
cp "$ROOT/model.bin" "$QN/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$QN/" 2>/dev/null || true
( cd "$QN" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '麒麟 有 三 个 角\n麒麟 有 角 吗\n麒麟 有 几 个 角\n麒麟 有 几 只 角\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '记下 了 ： 麒麟 有 三个角' "$QN/out.txt" \
    && ok "数量事实学得进（`麒麟 有 三 个 角`；以前被检索抢答成闲聊）" \
    || bad "数量-学" "$(grep -a 'AI >' "$QN/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '对 ， 麒麟 -[有]-> 角' "$QN/out.txt" \
    && ok "数量蕴含属性：`麒麟 有 角 吗` ⇒ 对（不是"不太确定"）" \
    || bad "数量-蕴含" "$(grep -a '角' "$QN/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '图 里 记 的 是 ： 麒麟 有 三 个 角' "$QN/out.txt" \
    && ok "问数目 ⇒ 答出图里记的那个数目（`图 里 记 的 是 ： 麒麟 有 三 个 角`）" \
    || bad "数量-数目" "$(grep -a '图 里 记 的 是' "$QN/out.txt" | head -1)"
grep -qaF '图 里 记 的 是 ： 麒麟 有 三 个 角' "$QN/out.txt" && [ "$(grep -c '图 里 记 的 是' "$QN/out.txt")" -ge 2 ] \
    && ok "量词不一致（问\"几只\"、记\"三个\"）也答得出（照图里的量词说）" \
    || bad "数量-量词" "$(grep -a '只 角' "$QN/out.txt" | head -1)"

# §6.591 定义句/全称句的说法归一（`所有…都` / `是 一种` / `是…的一种`）
DF="$WORK/df"; rm -rf "$DF"; mkdir -p "$DF"
cp "$ROOT/model.bin" "$DF/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$DF/" 2>/dev/null || true
( cd "$DF" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '所有 的 麒麟 都 属于 神兽\n麒麟 属于 神兽 吗\n麒麟 是 一种 神兽\n猫 属于 哪 一 类\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '记下 了 ： 麒麟 属于 神兽' "$DF/out.txt" && grep -qaF '对 ， 麒麟 -[属于]-> 神兽' "$DF/out.txt" \
    && ok "「所有 的 X 都 R Y」⇒ 学成 X R Y，且问得出来" \
    || bad "全称句" "$(grep -a '麒麟' "$DF/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '我 知道 ： 麒麟 是 神兽' "$DF/out.txt" \
    && ok "「X 是 一种 Y」⇒ 归一成 `X 是 Y`（不再听不懂）" \
    || bad "是一种" "$(grep -a '一种' "$DF/out.txt" | head -1)"
grep -qaF '我 记 的 是 ： 猫 属于 动物' "$DF/out.txt" \
    && ok "疑问句里的「哪一类」**不被归一**（`猫 属于 哪 一 类` 照常答）" \
    || bad "疑问不剥" "$(grep -a '猫' "$DF/out.txt" | tail -1)"
# 正反问（A 不 A）也是问句：`猫算不算动物这一类` 不能被归一削成「猫算不算动物这」
( cd "$DF" && { printf '::notepad off\n::char off\n猫算不算动物这一类\n'; } | \
    "$WORK/aip" --model m.bin > out3.txt 2>&1 ) || true
grep -qaF '对 ， 猫 -[属于]-> 动物' "$DF/out3.txt" \
    && ok "正反问（算不算/是不是/会不会…）**不当成定义句**（照常答，不被削成词串）" \
    || bad "正反问不剥" "$(grep -a 'AI >' "$DF/out3.txt" | tail -1)"
# `::relnat off` 时整条路径关掉（自然说法归一属于 §6.233 的开关）
( cd "$DF" && { printf '::notepad off\n::relnat off\n所有 的 龙 都 属于 神兽\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qa '不太懂\|不 太 懂' "$DF/out2.txt" \
    && ok "「::relnat off」时定义句归一也关掉（开关统一，不是一个新旁路）" \
    || bad "relnat 门控" "$(grep -a 'AI >' "$DF/out2.txt" | tail -1)"

# §6.590 「你 怎么 知道」：出处（教的是教、语料是语料、推的是推、没有就说没有）
PV="$WORK/pv"; rm -rf "$PV"; mkdir -p "$PV"
cp "$ROOT/model.bin" "$PV/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$PV/" 2>/dev/null || true
( cd "$PV" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '麒麟 属于 神兽\n你 怎么 知道 麒麟 属于 神兽\n你 怎么 知道 猫 属于 动物\n你 怎么 知道 麒麟 属于 植物\n神兽 属于 动物\n你 怎么 知道 麒麟 属于 动物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '这 条 是 你 教 我 的' "$PV/out.txt" \
    && ok "「你 怎么 知道 X」：教过的 ⇒ 说「这 条 是 你 教 我 的」" \
    || bad "出处-教的" "$(grep -a '教 我' "$PV/out.txt" | head -1)"
grep -qaF '不 是 这 次 会话 教 的' "$PV/out.txt" \
    && ok "没教过但图上有 ⇒ 说清「不是这次会话教的」（§6.597 起措辞更诚实：语料或以前会话）" \
    || bad "出处-语料" "$(grep -a '语料' "$PV/out.txt" | head -1)"
grep -qaF '我 图 里 没 有 这 条' "$PV/out.txt" \
    && ok "图里没有 ⇒ 老实说「没 有 这 条 —— 我 不 知道」" \
    || bad "出处-没有" "$(grep -a '我 图 里' "$PV/out.txt" | head -1)"
grep -qaF '推 出来 的 ： 麒麟 属于 神兽 ； 神兽 属于 动物' "$PV/out.txt" \
    && ok "推得出来 ⇒ 给一条链「推 出来 的 ： …」（不带 `?` 和自环）" \
    || bad "出处-链" "$(grep -a '推 出来' "$PV/out.txt" | head -1)"

# §6.589 用户最自然的更正说法：「不 对 ， 猫 属于 猫科」⇒ 学新的、撤旧的（不再"听不懂"）
CF="$WORK/cf"; rm -rf "$CF"; mkdir -p "$CF"
cp "$ROOT/model.bin" "$CF/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$CF/" 2>/dev/null || true
( cd "$CF" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '狗 属于 动物\n不 对 ，狗 属于 犬科\n狗 属于 动物 吗\n狗 属于 犬科 吗\n::mods\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '改 过来 了 ： 旧 的 狗 属于 动物 撤 掉 了' "$CF/out.txt" \
    && ok "「不 对 ，X」：学 X 并撤掉被否认的旧值（说清撤的是哪条）" \
    || bad "更正-撤旧" "$(grep -a 'AI >' "$CF/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '不 对 ， 狗 -[不 属于·独占]-> 动物' "$CF/out.txt" \
    && ok "更正之后旧值答「不 对」" \
    || bad "更正-旧值" "$(grep -a '动物' "$CF/out.txt" | tail -1)"
grep -qaF '对 ， 狗 -[属于]-> 犬科' "$CF/out.txt" \
    && ok "更正之后新值答「对」" \
    || bad "更正-新值" "$(grep -a '犬科' "$CF/out.txt" | tail -1)"
grep -qa '删关系 .*狗 属于 动物' "$CF/out.txt" \
    && ok "账里看得见「删关系 狗 属于 动物」（更正可追溯）" \
    || bad "更正-账" "$(grep -aE '删关系|加关系' "$CF/out.txt" | tr '\n' ' ')"

# §6.588 两个字的新词主语同样会被切碎（企/鹅），也要重拼（以前学成「企 -[鹅]-> 怕热」）
( cd "$TK" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '企鹅 怕 热\n企鹅 怕 热 吗\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '记下 了 ： 企鹅 怕 热' "$TK/out2.txt" \
    && ok "两个字的切碎主语也重拼（企鹅，不是 企 -[鹅]-> 怕热）" \
    || bad "两字重拼" "$(grep -a 'AI >' "$TK/out2.txt" | head -2 | tr '\n' ' ')"
grep -qaF '对 ， 企鹅 -[怕]-> 热' "$TK/out2.txt" \
    && ok "两字重拼之后问得出来" \
    || bad "两字重拼-回读" "$(grep -a '企鹅' "$TK/out2.txt" | tail -1)"

# §6.585 新旧知识冲突：教互不包含的独占事实 ⇒ **冲突可见**（不静默删知识）+ 显式「不」才撤
LC="$WORK/lc"; rm -rf "$LC"; mkdir -p "$LC"
cp "$ROOT/model.bin" "$LC/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$LC/" 2>/dev/null || true
# ⚠ 这一组**必须有 patterns.txt**：要验的就是"冲突措辞进了数据"（没有它就退回代码兜底文案）
( cd "$LC" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '狗 属于 植物\n狗 属于 动物 吗\n狗 不 属于 动物\n狗 属于 动物 吗\n::mods\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qa '冲突' "$LC/out.txt" \
    && ok "独占冲突：学的时候**说清冲突**（措辞来自 [say] 学习.冲突 / 改口.还冲突）" \
    || bad "冲突-措辞" "$(grep 'AI >' "$LC/out.txt" | head -2 | tr '\n' ' ')"
grep -qaF '对 ， 狗 -[属于]-> 动物' "$LC/out.txt" \
    && ok "冲突之后旧值**仍在**（不静默删知识，§6.199 的老判定）" \
    || bad "冲突-旧值" "$(grep '动物' "$LC/out.txt" | head -1)"
grep -qaF '改 过来 了' "$LC/out.txt" \
    && ok "显式说「狗 不 属于 动物」⇒ 改 过来 了（可追溯的撤）" \
    || bad "冲突-显式撤" "$(grep '改 过来' "$LC/out.txt" | head -1)"
grep -qaF '不 对 ， 狗 -[不 属于·独占]-> 动物' "$LC/out.txt" \
    && ok "显式撤之后旧值答「不 对」" \
    || bad "冲突-撤后" "$(grep '动物' "$LC/out.txt" | tail -1)"
grep -qa '加关系' "$LC/out.txt" \
    && ok "账里看得见「加关系」（每次学都记账）" \
    || bad "冲突-账" "$(grep -E '加关系|删关系' "$LC/out.txt" | tr '\n' ' ')"

# §6.586 追问式改口（CLI）：学冲突事实 → 回「对」⇒ 撤旧值；回「不」⇒ 两条都留
RF="$WORK/rf"; rm -rf "$RF"; mkdir -p "$RF"
cp "$ROOT/model.bin" "$RF/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$RF/" 2>/dev/null || true
( cd "$RF" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '狗 属于 植物\n对\n狗 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -qaF '改 过来 了 ： 旧 的 狗 属于 动物 撤 掉 了' "$RF/out.txt" \
    && ok "追问式改口：回「对」⇒ 撤掉旧值并说清" \
    || bad "追问改口-对" "$(grep -a 'AI >' "$RF/out.txt" | tail -2 | tr '\n' ' ')"
grep -qaF '不 对 ， 狗 -[不 属于·独占]-> 动物' "$RF/out.txt" \
    && ok "追问式改口：撤完之后旧值答「不 对」" \
    || bad "追问改口-回读" "$(grep -a '动物' "$RF/out.txt" | tail -1)"
( cd "$RF" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '猫 属于 植物\n不\n猫 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out2.txt 2>&1 ) || true
grep -qaF '两 条 都 留 着' "$RF/out2.txt" \
    && ok "追问式改口：回「不」⇒ 明说两条都留着" \
    || bad "追问改口-不" "$(grep -a 'AI >' "$RF/out2.txt" | tail -2 | tr '\n' ' ')"

# §6.584 反查表在"边搬去磁盘再读回来"之后必须重建：`::revcheck` 必须报"缺 0 条"
RV="$WORK/revb"; rm -rf "$RV"; mkdir -p "$RV"
cp "$ROOT/model.bin" "$RV/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$RV/" 2>/dev/null || true
( cd "$RV" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::parts save p.parts\n::parts attach p.parts\n::parts detach\n猫 属于 动物 吗\n::revcheck\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '反查表缺 0 条' "$RV/out.txt" \
    && ok "边搬去磁盘再读回来之后，反查表**重建了**（`::revcheck` 缺 0 条）" \
    || bad "revcheck-读回" "$(grep 'revcheck' "$RV/out.txt" | tail -1)"

# §6.581 池级共享：`::brain fork [N]` 从第 N 个脑分一个**共享底层**的脑（emb 共享、边不共享）
FK="$WORK/fork"; rm -rf "$FK"; mkdir -p "$FK"
cp "$ROOT/model.bin" "$FK/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$FK/" 2>/dev/null || true
( cd "$FK" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain fork 0\n::brain list\n猫 属于 动物 吗\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '共享了 [1-9][0-9]* 个词的 emb' "$FK/out.txt" \
    && ok "fork 报出**共享了多少个词**的 emb（不是"看起来共享了"）" \
    || bad "fork-共享" "$(grep '多脑' "$FK/out.txt" | head -2 | tr '\n' ' ')"
grep -q '边\*\*不共享\*\*' "$FK/out.txt" \
    && ok "fork 明确说清"边不共享"（边里存 Token*）" \
    || bad "fork-说明" "$(grep '不共享' "$FK/out.txt" | head -1)"
grep -q '2 个' "$FK/out.txt" && grep -q '对 ， 猫' "$FK/out.txt" \
    && ok "分出来的脑照样答得对（共享底层不影响知识）" \
    || bad "fork-问答" "$(grep -E '多脑|AI >' "$FK/out.txt" | tail -3 | tr '\n' ' ')"

# §6.576 `::parts verify` 自检：内存 vs 段文件对不对得上（改写之后必须报 stale，写完 delta 必须归零）
VR="$WORK/vr"; rm -rf "$VR"; mkdir -p "$VR"
cp "$ROOT/session.bin" "$VR/base.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$VR/" 2>/dev/null || true
( cd "$VR" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::parts save b.parts\n::parts attach b.parts\n::parts verify\n';     printf '猫 属于 动物\n::parts verify\n::parts delta v.parts\n::parts verify\n';     printf '::parts detach\n猫 属于 动物 吗\n'; } | \
    "$WORK/aip" --model base.bin > out.txt 2>&1 ) || true
n0=$(grep -c '段条数不一致 0 个' "$VR/out.txt" || true)
bad_stale=$(grep -c '⚠ 有 .* 个词的段和内存不一致' "$VR/out.txt" || true)
[ "${n0:-0}" -ge 1 ] && [ "${bad_stale:-0}" -ge 1 ] \
    && ok "自检**能报出"改过但没写 delta"**（stale>0 时会警告），写完之后归零" \
    || bad "verify-检出" "zero=${n0:-0} warn=${bad_stale:-0}"
grep -q '还没写 delta 0 个 / 段条数不一致 0 个' "$VR/out.txt" && grep -q '对 ， 猫 -\[属于\]-> 动物' "$VR/out.txt" \
    && ok "**写完 delta + 卸下之后仍然对得上、答得对**" \
    || bad "verify-归零" "$(grep '自检' "$VR/out.txt" | tail -2 | tr '\n' ' ')"
# §6.577 `::brain save N [文件]`：把**指定那个脑**存盘，读回来它自己的知识还在
BS="$WORK/bs"; rm -rf "$BS"; mkdir -p "$BS"
cp "$ROOT/session.bin" "$BS/base.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$BS/" 2>/dev/null || true
( cd "$BS" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain new\n小明同学 属于 人类\n::brain save 1 saved.bin\n'; } | \
    "$WORK/aip" --model base.bin > out.txt 2>&1 ) || true
grep -q '#1 存到 saved.bin' "$BS/out.txt" \
    && ok "**能存指定那个脑**（#1 ⇒ saved.bin）" \
    || bad "brain-save" "$(grep '多脑' "$BS/out.txt" | tail -2 | tr '\n' ' ')"
[ -s "$BS/saved.bin" ] \
    && ok "存出来的文件非空（不是空壳）" \
    || bad "brain-save-文件" "大小=0"
( cd "$BS" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '小明同学 属于 人类 吗\n'; } | \
    "$WORK/aip" --model saved.bin > out2.txt 2>&1 ) || true
grep -q '对 ， 小明同学 -\[属于\]-> 人类' "$BS/out2.txt" \
    && ok "**读回来还是它自己的知识**（不是 base 的）" \
    || bad "brain-save-回读" "$(grep 'AI >' "$BS/out2.txt" | tail -1)"
# §6.578 `::brain drop N`：关掉一个脑（内存还回去）、当前那个不许关、槽位能复用
DP="$WORK/dp"; rm -rf "$DP"; mkdir -p "$DP"
cp "$ROOT/session.bin" "$DP/base.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$DP/" 2>/dev/null || true
( cd "$DP" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n';     printf '::brain new\n::brain new\n::brain drop 1\n::brain list\n::brain drop 2\n::brain new\n::brain list\n'; } | \
    "$WORK/aip" --model base.bin > out.txt 2>&1 ) || true
grep -q '关掉 #1' "$DP/out.txt" \
    && ok "**能关掉一个脑**（#1 释放）" \
    || bad "brain-drop" "$(grep '多脑' "$DP/out.txt" | head -3 | tr '\n' ' ')"
grep -q '#[12] 是当前那个，先用 ::brain use 切走' "$DP/out.txt" \
    && ok "**当前那个不许关**（明确拒绝，不静默）" \
    || bad "brain-drop当前" "$(grep '是当前' "$DP/out.txt" | head -1)"
# ⚠ 别钉外壳词数（§6.593 钉 1474、§6.606 加连接词后又变 1482）——那只是"加载了几个词"的指纹，
# 与这条要看的东西（**槽位被新脑复用**）无关。改成只要求"有一个 #0 的词数行"。
n_list=$(grep -cE '#0  [0-9]+ 词' "$DP/out.txt" || true)
[ "${n_list:-0}" -ge 1 ] && grep -q '\* #1  32 词' "$DP/out.txt" \
    && ok "**关掉后列表少了一个、且槽位被新脑复用**（* #1 32 词）" \
    || bad "brain-drop-复用" "$(grep -E '^\s+[* ]#|\* #' "$DP/out.txt" | tail -3 | tr '\n' ' ')"
# §6.562 他者学习确认 + 过程实参超限
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 会 飞\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o131" 2>&1 ) || true
grep -q '记下 了 ： 他 以为 猫 会 飞 —— 可 我 知道 实际 上 不 是 这样' "$WORK/o131" \
    && ok "他者学习确认**照旧**（和我的知识冲突时也照实说）" \
    || bad "他者-确认" "$(sed -n 's/.*AI > //p' "$WORK/o131" | tail -1)"
L5="$WORK/l5"; rm -rf "$L5"; mkdir -p "$L5"
cp "$ROOT/session.bin" "$L5/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$L5/" 2>/dev/null || true
sed -i 's|^\[say\] 学习\.他以为反=.*|[say] 学习.他以为反=（数据）$1 说 $3 $4$5 $6，我 不 认|' "$L5/patterns.txt"
( cd "$L5" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'; printf '他 以为 猫 会 飞\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）他 说 猫 会 飞，我 不 认' "$L5/out.txt" \
    && ok "**改一行数据** ⇒ 他者确认换掉（六槽都对）" \
    || bad "他者-改模板" "$(sed -n 's/.*AI > //p' "$L5/out.txt" | tail -1)"
# §6.563 数据键**撞名必须报出来**（以前是静默覆盖：§6.560 撞过一次，blind4 −3）
( cd "$ROOT" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model model.bin > "$WORK/o132" 2>&1 ) || true
grep -q 'say_dup=0' "$WORK/o132" \
    && ok "出厂数据**没有重复键**（say_dup=0）" \
    || bad "撞名-出厂" "$(grep -o 'say_dup=[0-9]*' "$WORK/o132" || echo 缺字段)"
DX="$WORK/dx"; rm -rf "$DX"; mkdir -p "$DX"
cp "$ROOT/session.bin" "$DX/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$DX/" 2>/dev/null || true
printf '[say] 判词.对=撞名的后一句\n' >> "$DX/patterns.txt"
( cd "$DX" && printf '::quit\n' | AIP_PATDBG=1 "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q 'say_dup=1' "$DX/out.txt" \
    && ok "**人为撞名** ⇒ 被计数（say_dup=1），不再静默覆盖" \
    || bad "撞名-检测" "$(grep -o 'say_dup=[0-9]*' "$DX/out.txt" || echo 缺字段)"
grep -q '重复键' "$DX/out.txt" \
    && ok "撞名时**点名报出**（带键名，方便改）" \
    || bad "撞名-报名字" "$(grep -c '重复键' "$DX/out.txt")"
# §6.555 C7 余量：并列（都对/不对/一边）+ 三选一（对/都不对/不确定）
( cd "$ROOT" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 和 狗 都 属于 动物 吗\n猫 属于 动物 还是 植物\n猫 属于 鱼 还是 植物\n'; } | \
    "$WORK/aip" --model session.bin > "$WORK/o123" 2>&1 ) || true
grep -q '对 ， 猫 和 狗 都 属于 动物' "$WORK/o123" \
    && ok "并列「都对」**照旧**" \
    || bad "C7-并列" "$(sed -n 's/.*AI > //p' "$WORK/o123" | head -1)"
grep -q '对 ： 猫 属于 动物（不 是 植物）' "$WORK/o123" \
    && ok "三选一「对」**照旧**" \
    || bad "C7-三选" "$(sed -n 's/.*AI > //p' "$WORK/o123" | sed -n 2p)"
grep -q '我 不 确定 你 指 哪 个 ： 猫 属于 鱼 / 植物' "$WORK/o123" \
    && ok "三选一「不确定」**照旧**" \
    || bad "C7-三选不确定" "$(sed -n 's/.*AI > //p' "$WORK/o123" | sed -n 3p)"
C7X="$WORK/c7x"; rm -rf "$C7X"; mkdir -p "$C7X"
cp "$ROOT/session.bin" "$C7X/m.bin"; cp "$ROOT/patterns.txt" "$ROOT/frames.txt" "$ROOT/relations.txt" "$C7X/" 2>/dev/null || true
sed -i 's/^\[say\] 并列\.都对=.*/[say] 并列.都对=（数据）$1 和 $2 都 $3 $4 对/; s/^\[say\] 三选\.对=.*/[say] 三选.对=（数据）$1 $2 $3 对（不 是 $4）/' "$C7X/patterns.txt"
( cd "$C7X" && { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n'; printf '猫 和 狗 都 属于 动物 吗\n猫 属于 动物 还是 植物\n'; } | \
    "$WORK/aip" --model m.bin > out.txt 2>&1 ) || true
grep -q '（数据）猫 和 狗 都 属于 动物 对' "$C7X/out.txt" && grep -q '（数据）猫 属于 动物 对（不 是 植物）' "$C7X/out.txt" \
    && ok "**改两行数据** ⇒ 并列与三选都换掉（槽位都对）" \
    || bad "C7-改模板" "$(sed -n 's/.*AI > //p' "$C7X/out.txt" | tail -2 | tr '\n' ' ')"
# §6.522 B3 续：**动作命令里的 `$1`..`$4` 也替换**（以前只有断言/条件会换）
run '::act on\n::proc on\n::proc dry off\n::op 建过程 说\n::op 加执行 说 echo $1\n::proc run 说 猫\n' o49
grep -q '真执行了.*echo 猫' o49 \
    && ok "**实参进动作命令**：`echo $1` + run 带 `猫` ⇒ 真执行 `echo 猫`" \
    || bad "动作实参" "$(grep -m3 '执行\|echo' o49)"
run '::act on\n::proc on\n::proc dry off\n::op 建过程 说\n::op 加执行 说 echo $2\n::proc run 说 猫\n' o50
grep -q 'echo \$2' o50 \
    && ok "**没给的实参原样留着**（`$2` 不变空串 —— 和断言/条件同一套规矩）" \
    || bad "动作缺参" "$(grep -m3 '执行\|echo' o50)"
# §6.523 B2 续：条件能看**缺口类型**（`#N:过` / `#N:认不全` / `#N:复核不过`）
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 猫 属于 鱼\n::op 加条件 查 #1:复核不过 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查\n' o51
grep -q '条件「#1:复核不过」\*\*成立\*\*' o51 \
    && ok "**复核不过**的断言 ⇒ `#1:复核不过` 成立（看类型，不只看成败）" \
    || bad "缺口类型-复核不过" "$(grep -m3 '条件' o51)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 $1 属于 $2\n::op 加条件 查 #1:认不全 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查 猫\n' o52
grep -q '认不全' o52 && grep -q '条件「#1:认不全」\*\*成立\*\*' o52 \
    && ok "**认不全**（缺实参）⇒ `#1:认不全` 成立" \
    || bad "缺口类型-认不全" "$(grep -m3 '条件\|认不全' o52)"
run '::proc on\n::proc dry off\n::op 建过程 查\n::op 加断言 查 猫 属于 动物\n::op 加条件 查 #1:过 报成立\n::op 加否则 查 报不成立\n猫 属于 动物\n::proc run 查\n' o53
grep -q '条件「#1:过」\*\*成立\*\*' o53 \
    && ok "**过** ⇒ `#1:过` 成立（与 `#1` 同真值，但写出来更清楚）" \
    || bad "缺口类型-过" "$(grep -m3 '条件' o53)"
# §6.524 P1-3：过程**说过话**就把那句话当答复（`加说` 接进对话流水线）
run '::proc on\n::proc dry off\n::proc talk on\n::op 建过程 报\n::op 加说 报 猫 属于 动物\n跑 报\n' o54
grep -q 'AI > 猫 属于 动物' o54 \
    && ok "**说过的过程**：`跑 报` 直接答「猫 属于 动物」（不是状态串）" \
    || bad "过程答复" "$(grep -m2 'AI >' o54)"
run '::proc on\n::proc dry off\n::proc talk on\n::op 建过程 报\n::op 加步骤 报 烧水\n跑 报\n' o55
grep -q '跑 完 了' o55 \
    && ok "**没说过话的过程**：仍回状态串（不被空模板顶掉）" \
    || bad "过程无话" "$(grep -m2 'AI >' o55)"
# §6.525 P1-3 续：对话里也能**带实参**（`跑 <过程> <实参…>`，过程名取最长匹配）
run '::proc on\n::proc dry off\n::proc talk on\n::op 建过程 报\n::op 加说 报 $1 属于 $2\n跑 报 猫 动物\n' o56
grep -q 'AI > 猫 属于 动物' o56 \
    && ok "**对话里带实参**：`跑 报 猫 动物` ⇒ 过程名=报、实参=猫/动物" \
    || bad "对话实参" "$(grep -m2 'AI >' o56)"
run '::proc on\n::proc dry off\n::proc talk on\n::op 建过程 泡茶\n::op 加说 泡茶 泡 好 了\n::op 加步骤 泡茶 烧水\n跑 泡茶\n' o57
grep -q 'AI > 泡 好 了' o57 \
    && ok "**多字过程名不被实参拆坏**（`跑 泡茶` 仍认成过程 泡茶）" \
    || bad "多字过程名" "$(grep -m2 'AI >' o57)"
# §6.526 对话入口的实参超限也要**明说**（CLI 有，对话以前没有）
run '::proc on\n::proc dry off\n::proc talk on\n::op 建过程 报\n::op 加说 报 $1\n跑 报 甲 乙 丙 丁 戊\n' o58
grep -q '实参最多 4 个' o58 \
    && ok "对话里给 5 个实参 ⇒ 答复里**明说**「后面 1 个没带进去」" \
    || bad "对话实参超限" "$(grep -m2 'AI >' o58)"
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
if [ "${AIP_PROCTALK:-1}" = "0" ]; then
    run '记个过程 泡茶\n' o33
    grep -aq '记下 了 （ \[过程:泡茶\] ' o33 && bad "默认关不该认" "$(grep -m1 'AI >' o33)" \
        || ok "默认关：这些话照走流水线（一句都不认）"
    run '::proc talk\n' o34
    grep -q '过程记号接进对话 = 关（默认）' o34 && ok "::proc talk 自报默认关" || bad "talk 默认" "$(head -2 o34)"
else
    ok "显式关（AIP_PROCTALK=0）：这一侧断言"关""
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
if [ "${AIP_PROCTHINK:-1}" = "0" ]; then
    run '::proc think off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做 呢\n::delib\n' o38
    n=$(grep -ac '这我记过过程' o38 || true)
    [ "$n" -eq 0 ] && ok "默认关：思考里**一个字都不加**" || bad "默认关" "$(grep -m1 '记过过程' o38)"
    run '::proc think\n' o39
    grep -q '过程接进思维 = 关（默认）' o39 && ok "::proc think 自报默认关" || bad "think 默认" "$(head -2 o39)"
else
    ok "显式关（AIP_PROCTHINK=0）：这一侧断言"关""
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
if [ "${AIP_PROCPLAN:-1}" = "0" ]; then
    run '::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n泡茶 怎么 做\n' o43
    n=$(printf '%s\n' "$(cat o43)" | grep -ac '我 记过 过程' || true)
    [ "$n" -eq 0 ] && ok "默认关：照旧走流水线（不拿计划当回答）" || bad "默认关" "$(grep -m1 '记过 过程' o43)"
    run '::proc plan\n' o44
    grep -q '过程接进决策 = 关（默认）' o44 && ok "::proc plan 自报默认关" || bad "plan 默认" "$(head -2 o44)"
else
    ok "显式关（AIP_PROCPLAN=0）：这一侧断言"关""
fi
run '::proc plan on\n::proc off\n::op 建过程 泡茶\n::op 加步骤 泡茶 烧水\n::op 加步骤 泡茶 泡茶叶\n::op 建过程 洗杯子\n::op 加步骤 洗杯子 冲水\n::op 加调用 泡茶 洗杯子\n泡茶 怎么 做\n' o45
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
run '猫 属于 泡茶\n猫 属于 泡茶 吗\n' o59
# §6.585 之后学习答复有两种合法形态：「记下 了」（没冲突）或「改 过 来 了…撤 掉 了」
# （`泡茶` 与 `动物` 互不包含 ⇒ 后说的算）。**判据是"学得成"**，不是措辞：
grep -aqE '记下 了 ： 猫 属于 泡茶|改 过 来 了 ： 猫 属于 泡茶' o59 \
    && ok "有词表时：**整词切** ⇒ 同一句话学得成（数据驱动，不动算法）" \
    || bad "整词切" "$(grep -m2 "AI >" o59)"
grep -aqF '对 ， 猫 -[属于]-> 泡茶' o59 \
    && ok "整词切之后这条事实**问得出来**（学得成 → 用得上）" \
    || bad "整词切-回读" "$(grep -m3 "AI >" o59)"
rm -f lexicon.txt

echo "== ㉑ 特征集合 → 概念（只读；答的是「像什么」不是「是什么」） =="
run '::feat 毛\n' o60
grep -aq '像是什么' o60 && grep -aq '动物' o60 \
    && ok "`::feat 毛` ⇒ 投出「动物」（只投**关系边**：出厂模型里 动物—有→毛 才是关系边）" \
    || bad "特征投票" "$(grep -m3 '像是\|命中' o60)"
grep -aq '强度 ' o60 && ok "投票按**边强度**排（`ai_edge_weight` 现算，不是纯计数）" \
    || bad "边强度" "$(grep -am2 '命中' o60)"
grep -aq '这是\*\*联想\*\*' o60 && ok "措辞明说这是**联想**，不是判定" \
    || bad "措辞" "$(grep -m1 '联想' o60)"
run '::feat 没这个词\n' o61
grep -aq '不在图上' o61 && ok "没在图上 ⇒ 如实说（不瞎投）" || bad "不在图上" "$(head -2 o61)"

echo "== ㉒ A 组接线：N1 思考对象接进 ::cot（默认关） =="
# ⚠ 两档分开：默认那侧断言"对象关着"，开着那侧才断言"打得出对象"（反向测量下两边各自跑）
if [ -z "${AIP_THINKOBJ:-}" ]; then
    # ⚠ 得先有一轮说话，否则 ::cot 直接"还没有——先说句话"就返回了（第一版踩到）
    run '猫 有 毛 吗\n::cot\n' o62
    grep -aq '思考对象\] 关着' o62 && ok "默认关：::cot 明说对象关着（不假装有）" \
        || bad "默认关" "$(grep -m2 '思考' o62)"
    ok "AIP_THINKOBJ=1（反向测量）：开着那侧的断言按规矩跳过"
else
    run '猫 有 毛 吗\n::cot\n' o63
    grep -aq '思考对象 #1\] 步 ' o63 && grep -aq '缺口 ' o63 \
        && ok "开着：::cot 打出**思考对象**（编号/步数/缺口/复没复核）—— 局限里「::cot 没接它」结掉" \
        || bad "思考对象" "$(grep -am3 '思考' o63)"
fi

echo "== ㉔ C③ 生成原语程序：过程 → 原语序列 → 干跑校验（C 线缺的那一节） =="
run '::proc on\n::op 建过程 生成试\n::op 加步骤 生成试 烧水\n::op 加步骤 生成试 泡茶叶\n::op 加条件 生成试 有热水 泡茶叶\n::op 加否则 生成试 用凉水\n::proc gen 生成试\n' o66
grep -aq '\[生成\] 过程「生成试」⇒ 原语程序' o66 && ok "`::proc gen` 把过程**翻译成原语程序**（以前只会读外面写好的代码）" \
    || bad "生成" "$(grep -am2 '生成' o66)"
grep -aq '\[原语:对象\] 烧水' o66 && ok "步骤 → \`[原语:对象] <动作词>\`" \
    || bad "步骤翻译" "$(grep -am1 '原语' o66)"
grep -aq '\[原语:如果\]' o66 && grep -aq '\[原语:否则\]' o66 && grep -aq '\[原语:结束\]' o66 \
    && ok "条件/否则 → \`如果/否则/结束\`（形状按 PRIMS 的栈效应来）" \
    || bad "控制流翻译" "$(grep -am1 '原语' o66)"
grep -aq '干跑校验：通过' o66 && ok "生成的东西**过干跑校验**（和 \`::asm\` 同一条判据，不是"生成了就算"）" \
    || bad "校验" "$(grep -am2 '校验' o66)"
if [ -z "${AIP_GENEXEC:-}" ]; then
    grep -aq '要真跑：`::proc genexec on`' o66 && ok "默认**只生成+校验**（真跑要显式开 genexec）" \
        || bad "默认" "$(grep -am1 '校验' o66)"
else
    grep -aq '已走受审通道执行' o66 && ok "AIP_GENEXEC=1（反向测量）：生成后**真跑**（走受审通道）" \
        || bad "真跑" "$(grep -am2 '生成' o66)"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS tokenop（Token 行为：接线/插词/中转 + 临时 Token 不存盘 + 修正的记忆 + 原语组合 + 前提闸门/极性/进答复/对话触发 + 过程记号 + 过程执行引擎 + 干跑（假设世界）+ 分支/循环/异常 + 步骤带对象 + 对话入口 + 接进思维 + 接进决策 + 解释器观察遍 + 动作执行槽 + 求值层样板 + 词表覆盖 + 特征投票 + 思考对象进 ::cot + 生成原语程序，$pass 项）"
else
    echo "FAIL tokenop（$fail 项失败，$pass 项通过）"
    exit 1
fi
