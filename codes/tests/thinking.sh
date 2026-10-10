#!/bin/sh
# tests/thinking.sh — 思维循环（::delib / ::pending / ::think*）的端到端回归
#
# 单元级的东西（议程去重、验证器复核、矛盾、跨轮、UTF-8 截断）在
# tools/delibtest.c 里；这里盯的是**在真模型、真对话里**它表现成什么样：
#
#   ① 每轮真的在想：::delib 有目标、有分步、有依据、有停止原因
#   ② 想得对：直接事实 / 规则推导（继承）/ **反方向才是答案**（缺前提）
#   ③ 诚实：没知识就写明「推不出来」，并且把问题挂起来（跨轮接着想）
#   ④ 思维改脑走的是受审通道：::mod 里能看到「加Token [自问:…] [思维:自问]」
#   ⑤ 跨轮想通：教会它 → ::pending 归零 → 打开 ::thinkout 才在回复里补一句
#   ⑥ 矛盾：教一条和缺前提推理相反的事 → 下一轮 ::delib 报「正反都能推出来」
#   ⑦ 开关与预算：::thinkrun / ::thinklearn / ::thinkbudget 都真的生效
#   ⑧ 铁律：**推导不动图**（::mod 里 新边/打关系 都是 0）
#   ⑨ N1「思考对象」（[思考:N]，默认关）：一轮思考产出**结构化**对象（目标 + 步骤 + 复核），
#      ::delib 那一节打的是对象本身；缺口=0 才算站得住；**只进内存**（不进图、不存盘）
#   ⑩ N4「空闲思考」（默认关）：没人说话的那一轮，自己从 [自问:…] 里挑一个问题来想；
#      **只读**（不提议意图、不沉淀、不动图），睡眠/巩固/衰减的账一点不动；节拍 ::idle every N
#   ⑪ N3「回溯 + 在线修订」（默认关）：走到一半发现**前提不成立** → 退回分支起点、换一条
#      （同一个问题改问反面）再试；痕上写明为什么退、失败那一支留着、**步数预算不退**
#
# 用法：sh tests/thinking.sh [模型文件]     默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/think"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
# 每条断言都从**干净的 model.bin** 起跑，免得前一句学到的知识污染后一句
run() { cp model.bin m.bin; printf '%b' "$1" | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== 1. 每轮都在想：::delib 有目标/分步/依据/停止原因 =="
run '猫 有 毛 吗\n::delib\n::stats\n' o1
grep -q '\[思维\] 想过 1 轮' o1 && ok "想过 1 轮（默认每轮都想）" || bad "每轮想" "$(grep '\[思维\]' o1)"
grep -q '立目标（关系问题）' o1 && ok "有目标" || bad "目标" "$(grep -m3 '\.' o1)"
grep -q '交给求解器' o1 && ok "有分步（查→推）" || bad "分步" "$(grep '推\|查' o1)"
grep -q '继承' o1 && ok "依据链上有规则名（继承）" || bad "依据" "$(grep '依据' o1)"
grep -q '停止原因：有结论' o1 && ok "写明了停止原因" || bad "停因" "$(grep '停止原因' o1)"
grep -q '思维：想过' o1 && ok "::stats 里有思维摘要行" || bad "stats" "$(grep '思维' o1)"

echo "== 2. 想得对：直接事实 / 规则推导 / 反方向才是答案 =="
run '猫 会 飞 吗\n::delib\n' o2
grep -q '结论=不成立' o2 && ok "直接事实（猫 不会 飞）：结论=不成立" || bad "直接事实" "$(grep '这一轮' o2)"
grep -q '直接否定' o2 && ok "只查到直接否定时不立刻下结论（规则或假设可能推翻）" || bad "查的克制" "$(grep '查 ' o2)"
grep -q '反方向才是答案' o2 && ok "交给求解器之后，反方向给出真值" || bad "反方向" "$(grep '反方向' o2)"
grep -q '依据强度：观测事实' o2 && ok "结论强度标成「观测事实」" || bad "强度" "$(grep '依据强度' o2)"

run '猫 会 唱歌 吗\n::delib\n' o3
grep -q '反方向才是答案' o3 && ok "问「会」推不出来 → 试反方向" || bad "反方向" "$(grep '反方向\|推' o3)"
grep -q '缺前提' o3 && ok "依据是「缺前提」那条规则（R9）" || bad "缺前提" "$(grep '依据' o3)"
grep -q '结论=不成立' o3 && ok "结论=不成立（猫 不会 唱歌）" || bad "结论" "$(grep '这一轮' o3)"

echo "== 3. 诚实 + 挂问题 + 思维改脑走受审通道 =="
run '猫 会 跳舞 吗\n::delib\n::pending\n::mod\n' o4
grep -qE '停止原因：推不出来|停止原因：原地打转' o4 && ok "没知识就写明推不下去" || bad "停因" "$(grep '停止原因' o4)"
grep -q '还挂着 1 个' o4 && ok "把想不通的问题挂起来（跨轮接着想）" || bad "挂问题" "$(grep -A3 '未解问题' o4)"
grep -q '加Token \[自问:猫_会_跳舞_吗\].*思维:自问' o4 \
    && ok "自问节点经**意图队列**落地（审计里看得见来源）" || bad "受审通道" "$(grep -A4 '最近应用' o4)"

echo "== 4. 铁律：推出来的可以沉淀，但**不许当成观测** =="
# 这一条随「推理沉淀」改了口径（原来是「一个字都不许写进图」）：
# 现在允许把想通的记成一条**缓存**（role=ASSOC_ROLE_DERIVED），
# 但**不许**当成观测到的关系写进去 —— 那会把观测降级，安全内核也会拒。
run '猫 有 毛 吗\n::mod\n::rel 猫\n' o5
grep -q '沉淀 1' o5 && ok "想通的被沉淀成缓存（走意图队列）" || bad "沉淀" "$(grep '应用 ' o5)"
grep -q '打关系 0' o5 && ok "没有把它当成**观测到的关系**写进去" || bad "铁律" "$(grep '应用 ' o5)"
grep -q '猫 --\[有\]--> 毛（推出来的，可撤）' o5 \
    && ok "边上标着「推出来的，可撤」（来源没被抹掉）" || bad "来源标记" "$(grep '毛' o5)"

echo "== 5. 跨轮想通：教会它 → 归零 → 下一轮补一句 =="
run '猫 会 跳舞 吗\n猫 会 跳舞\n::pending\n::delib\n' o6
grep -q '记下 了' o6 && ok "教会它了（关系层现场学）" || bad "教学" "$(grep 'AI >' o6)"
grep -q '没有挂着的' o6 && ok "旧问题被想通了（挂 → 0）" || bad "想通" "$(grep '未解问题' o6)"
grep -q '跨轮想通' o6 && ok "痕上记着「跨轮想通」" || bad "跨轮痕" "$(grep '跨轮' o6)"
grep -qE '想通 1' o6 && ok "想通计数在走" || bad "想通计数" "$(grep '未解问题' o6)"

# 接管输出是个开关：显式关掉时**不**补那句，打开时才补
run '::thinkout off\n猫 会 跳舞 吗\n猫 会 跳舞\n你好\n' o7
if grep -q '现在 我 知道 了' o7; then
    bad "::thinkout off 时不多说" "$(grep '现在 我 知道 了' o7)"
else
    ok "::thinkout off 时不在回复里多说"
fi
run '::thinkout on\n猫 会 跳舞 吗\n猫 会 跳舞\n你好\n' o8
grep -q '现在 我 知道 了' o8 && ok "::thinkout on 之后补一句「现在我知道了」" \
                               || bad "接管输出" "$(grep 'AI >' o8 | tail -2)"

echo "== 6. 矛盾：教一条和缺前提推理相反的事 =="
run '猫 会 唱歌\n猫 会 唱歌 吗\n::delib\n::mod\n' o9
grep -q '正反都能推出来' o9 && ok "正反都成立 → 停因=矛盾" || bad "矛盾" "$(grep '停止原因' o9)"
grep -q '互相矛盾的依据' o9 && ok "结论位置写的是「有互相矛盾的依据」（不下断言）" || bad "矛盾结论" "$(grep '结论' o9)"
grep -qE "知识自相矛盾=[1-9]" o9 && ok "把矛盾报给元认知（提醒计数）" || bad "元认知" "$(grep "元认知提醒" o9)"

echo "== 7. 开关与预算 =="
run '::thinkrun off\n猫 有 毛 吗\n::delib\n' o10
grep -q '思维循环关着' o10 && ok "::thinkrun off 之后不再想" || bad "thinkrun off" "$(grep '收束' o10)"
run '::thinkbudget 1\n猫 会 唱歌 吗\n::delib\n' o11
grep -q '步数预算用完' o11 && ok "::thinkbudget 1 → 停因=步数预算用完" || bad "预算" "$(grep '停止原因' o11)"
run '::thinklearn off\n猫 会 跳舞 吗\n::mod\n::pending\n' o12
grep -q '新Token 0' o12 && ok "::thinklearn off → 不落自问节点（改不了脑）" || bad "thinklearn off" "$(grep '应用 ' o12)"
grep -q '还挂着 1 个' o12 && ok "但问题照样挂（想还是照想）" || bad "照想" "$(grep '未解问题' o12)"

echo "== 8. 未解问题持久化：关掉再打开还记得没想通的事（**不动存档格式**） =="
# 靠的是随图存盘的 [自问:…] 标记：装载时扫一遍，把问题重新挂回议程。
run '猫 会 跳舞 吗
::pending
::save pslot
' o13
grep -q '还挂着 1 个' o13 && ok "先挂一个想不通的问题（[自问:…] 经审计落地）" \
    || bad "挂问题" "$(grep '未解问题' o13 | head -2)"
[ -s pslot ] && ok "存盘成功" || bad "存盘" "$(tail -2 o13)"
mv pslot m.bin
printf '::pending\n::quit\n' | ./aip --model m.bin > o14 2>&1 || true
grep -q '还挂着 1 个' o14 && ok "重载后问题还在（不用升格式、不用重训）" \
    || bad "重载恢复" "$(grep '未解问题' o14 | head -3)"
grep -q -- '- 猫 会 跳舞 吗' o14 && ok "恢复出来的问句**完整**（自问名字的预算从 ~38 提到 ~86 字节）" \
    || bad "问句被截断" "$(grep -A1 '还挂着' o14 | head -2)"
grep -qE '装载恢复 [1-9]' o14 && ok "计数里如实报「装载恢复 N」" \
    || bad "恢复计数" "$(grep '累计' o14 | head -2)"

echo "== 9. N1 思考对象（[思考:N]，默认关）：目标/结构化步骤/复核/只进内存 =="
# 默认关那一侧：与从前逐字节相同（没有对象这一节）。
# ⚠ 开关反向测量（AIP_THINKOBJ=1，见 tests/switches_ab.sh）时这几条**自动跳过**。
run '猫 有 毛 吗\n::delib\n::thinkobj\n::thinkobj verify\n' o15
if [ -z "${AIP_THINKOBJ:-}" ]; then
    if grep -q '—— 思考对象' o15; then bad "默认关时不该有对象节" "$(grep -m1 -- '—— 思考对象' o15)"
    else ok "默认关：::delib 里没有「思考对象」这一节（与从前逐字节相同）"; fi
    grep -q '\[思考对象\] 关着' o15 && ok "::thinkobj 自报默认关" \
        || bad "thinkobj 状态" "$(grep -m2 '思考对象' o15)"
    grep -q '\[没得复核\]' o15 && ok "没对象时 ::thinkobj verify 明说没得复核" \
        || bad "空对象复核" "$(grep -m1 '复核' o15)"
else
    ok "AIP_THINKOBJ=1（反向测量）：默认值断言按规矩跳过"
fi

# 开着那一侧：结论=成立（继承链）的那一轮，对象必须在、复核缺口=0、步骤是**结构化**的
run '::thinkobj on\n猫 有 毛 吗\n::delib\n::thinkobj\n::thinkobj verify\n::edges [思考:1]\n' o16
grep -q '—— 思考对象（`::thinkobj` 看的就是它）——' o16 \
    && ok "::delib 多出「思考对象」一节（打的是对象，不是另一份日志）" \
    || bad "::delib 的对象节" "$(grep -m3 '思考对象' o16)"
grep -qE '\[思考对象 #[0-9]+\].*结构化步骤 [0-9]+ 条.*缺口 0' o16 \
    && ok "对象在：有编号、有结构化步骤数、**复核缺口 0**（der_verify 独立重推过）" \
    || bad "对象头" "$(grep -m1 '思考对象 #' o16)"
grep -q '目标：猫 有 毛' o16 && ok "对象里有**目标**（这一轮要弄清的事）" \
    || bad "对象目标" "$(grep -m3 '目标' o16)"
grep -q '规则 ✓' o16 && grep -q '事实 ✓' o16 && grep -q '结论' o16 \
    && ok "步骤是结构化的（规则/事实/结论三类，每步带验证标记）—— 用的是 derive.h 那套" \
    || bad "结构化步骤" "$(grep -A6 '推导' o16 | head -8)"
grep -q '（继承）' o16 && ok "步骤里带得出规则名（继承）—— 与 ::delib 的依据链对得上" \
    || bad "规则名" "$(grep -m2 '继承' o16)"
grep -q '\[当场复核\].*缺口 0（站得住）' o16 \
    && ok "::thinkobj verify 当场把对象里的链**又独立重推了一遍**：缺口 0" \
    || bad "当场复核" "$(grep -m1 '当场复核' o16)"
grep -q '没有这个 Token: \[思考:1\]' o16 \
    && ok "对象**没进图**（铁律：只有观测进图 —— ::edges 找不到它这个 Token）" \
    || bad "对象进图了" "$(grep -m2 '思考:1' o16)"

# 没接出链的那一轮也要照实留一条**缺口**（不许把空对象说成"想通了"）
run '::thinkobj on\n猫 会 跳舞 吗\n::thinkobj\n' o17
grep -qE '\[思考对象 #[0-9]+\].*缺口 [1-9]' o17 \
    && ok "想不出来的那一轮：对象在，但**缺口>0**（照实说，不当结论用）" \
    || bad "诚实缺口" "$(grep -m1 '思考对象 #' o17)"
grep -q '没接出一条站得住的链' o17 && ok "缺口那一步写明了原因" \
    || bad "缺口原因" "$(grep -A4 '推导' o17 | head -6)"

# 只进内存：存盘再读回来，对象**不在**（存档格式没动 —— N1 一个字都不进文件）
run '::thinkobj on\n猫 有 毛 吗\n::save n1slot\n' o18
[ -s n1slot ] && ok "存盘成功（存档格式没动）" || bad "存盘" "$(tail -2 o18)"
mv n1slot m.bin
printf '::thinkobj\n::quit\n' | AIP_THINKOBJ=1 ./aip --model m.bin > o19 2>&1 || true
grep -q '还没有' o19 && ok "重载后对象**不在**（只进内存：跨重启不复用）" \
    || bad "对象跨重启" "$(grep -m2 '思考对象' o19)"

echo "== 10. N4 空闲思考（默认关）：没人说话时自己挑一个没想通的问题来想（只读） =="
# 默认关那一侧：连着几轮没人说话也不空闲想（与今天逐字节相同）。
# ⚠ 开关反向测量（AIP_IDLE=1）时这几条**自动跳过**。
# 注意顺序：先问一个想不通的 → **三轮没人说话** → 再看状态（这样两侧断言看的是同一时刻）
run '猫 会 跳舞 吗\n\n\n\n::idle\n::delib\n' o20
if [ -z "${AIP_IDLE:-}" ]; then
    grep -q '空闲思考 = 关（默认）' o20 && ok "::idle 自报默认关" \
        || bad "idle 默认" "$(grep -m2 '空闲思考' o20)"
    grep -q '空闲想过 0 轮' o20 && ok "默认关：连着几轮没人说话也不空闲想（0 轮）" \
        || bad "默认不空闲想" "$(grep -m1 '空闲想过' o20)"
    if grep -qF '没人说话，我自己挑了一个没想通的问题来想：' o20; then
        bad "默认关时不该有空闲痕" "$(grep -m1 '没人说话' o20)"
    else ok "默认关：痕里没有「空闲思考」那一条"; fi
else
    grep -q '空闲想过 3 轮' o20 && grep -qF '没人说话，我自己挑了一个没想通的问题来想：' o20 \
        && ok "AIP_IDLE=1（反向测量）：启动即开，三轮没人说话想了三次（默认值断言按规矩跳过）" \
        || bad "反向测量" "$(grep -m2 '空闲想过\|没人说话' o20)"
fi

# 开着那一侧：挂一个想不通的问题 → 三轮没人说话 → 它自己挑来想（而且**只读**）
run '猫 会 跳舞 吗\n::pending\n::stats\n::idle on\n\n\n\n::stats\n::delib\n::idle\n\n::idle off\n\n::idle\n' o21
grep -q '空闲思考 = 开' o21 && ok "::idle on 打开了" || bad "idle on" "$(grep -m1 '空闲思考 =' o21)"
grep -q '还挂着 1 个' o21 && ok "前置：有一个没想通的问题挂着" || bad "前置" "$(grep -m2 '未解问题' o21)"
grep -q '空闲思考：自己挑的' o21 && ok "痕里能看到「它自己挑了一个没想通的问题在想」" \
    || bad "空闲痕" "$(grep -m3 '空闲\|挑' o21)"
grep -q '没人说话，我自己挑了一个没想通的问题来想：猫 会 跳舞 吗' o21 \
    && ok "痕里写明了挑的是哪个问题（就是挂着的那条）" \
    || bad "挑的是哪个" "$(grep -A2 '空闲思考' o21 | head -4)"
grep -q '空闲想过 3 轮' o21 && ok "三轮没人说话 = 空闲想了 3 轮" \
    || bad "空闲轮数" "$(grep -m1 '空闲想过' o21)"
grep -q '最近挑的是：猫 会 跳舞 吗' o21 && ok "::idle 报得出最近挑的问题" \
    || bad "最近挑的" "$(grep -m1 '最近挑的' o21)"
# **只读**：把两份 ::stats 里**该一动不动**的那几行抽出来逐字节比
# （图/边/已学/轮次/睡眠/轨迹/写入账/内核计数）—— 思考自己的"想过 N 轮"当然会涨，
# 那本来就是"在想"，不算账被动了。
awk '/\[统计\] Token=/{n++} { if (n==2) print > "st2.txt"; else if (n==3) print > "st3.txt" }' o21
inv() { grep -E '\[统计\] Token=|已学=|惊讶=|信用=|睡眠=|轨迹=|帧生成=|思维链步数=|关系知识：|假设层：|世界假设：|他者=|想象=|诊断=|微内核：|写入账|判定：|拒绝理由' "$1"; }
inv st2.txt > st2i.txt
inv st3.txt > st3i.txt
if [ -s st2i.txt ] && diff -q st2i.txt st3i.txt > /dev/null 2>&1; then
    ok "只读：三轮空闲思考前后，图/边/已学/睡眠/轨迹/写入账/内核计数**一条不差**"
else
    bad "只读（账变了）" "$(diff st2i.txt st3i.txt 2>&1 | head -8)"
fi
# 回复一个字都不变：这一小节一共 6 次应答，其中 5 次是空行那五轮的「……」，另一次是问句的正常回答
n_ai=$(grep -cF 'AI > ' o21 || true)
n_dots=$(grep -cF 'AI > ……' o21 || true)
[ "$n_ai" -eq 6 ] && [ "$n_dots" -eq 5 ] \
    && ok "回复不变：五轮没人说话都只回「……」（输出侧没被碰，另一次是问句的正常回答）" \
    || bad "回复变了" "应答 $n_ai 次 / 其中「……」$n_dots 次（期望 6 / 5）"
grep -q '空闲想过 3 轮' o21 && grep -q '空闲思考 = 关' o21 \
    && ok "::idle off 之后就停了（还是 3 轮）" || bad "idle off" "$(grep -m2 '空闲想过\|空闲思考 =' o21)"

# 节拍：::idle every 3 —— tick 每轮 +1，五轮没人说话只该想两次
run '猫 会 跳舞 吗\n::idle every 3\n\n\n\n\n\n::idle\n' o22
grep -q '空闲思考 = 开（每 3 个 tick 最多一次）' o22 && ok "::idle every 3 设上了" \
    || bad "every 3" "$(grep -m1 '空闲思考 =' o22)"
grep -q '空闲想过 2 轮' o22 && ok "节拍生效：五轮没人说话只想了 2 次（每 3 个 tick 一次）" \
    || bad "节拍" "$(grep -m1 '空闲想过' o22)"

echo "== 11. N3 回溯 + 在线修订（默认关）：前提不成立就退回去换一条 =="
# 场景：给「猫」挂一条前提链接（`猫 —[前提]→ 饿 → 鱼`），再问一个猫身上**推不出来**的问题 ——
# 循环会走到「问前提」那一步并判定"前提没成立"，这就是 N3 的触发条件。
# ⚠ 开关反向测量（AIP_THINKREDO=1）时"默认关"那几条**自动跳过**。
run '::op 加前提 猫 鱼 饿\n猫 会 跳舞 吗\n::delib\n::thinkredo\n' o23
if [ -z "${AIP_THINKREDO:-}" ]; then
    grep -q '前提没成立' o23 && ok "走到「问前提」那一步：判定前提没成立（触发条件成立）" \
        || bad "触发条件" "$(grep -a -m3 '前提' o23)"
    if grep -qa '改问「' o23; then bad "默认关不该回退" "$(grep -a -m1 '改问' o23)"
    else ok "默认关：**不回退**（继续硬往下想 —— 与今天逐字节相同）"; fi
    grep -q '回溯 + 在线修订 = 关（默认）' o23 && ok "::thinkredo 自报默认关" \
        || bad "thinkredo 默认" "$(grep -m2 '回溯 + 在线修订' o23)"
else
    ok "AIP_THINKREDO=1（反向测量）：默认值断言按规矩跳过"
fi

# 开着那一侧
run '::op 加前提 猫 鱼 饿\n::thinkredo on\n猫 会 跳舞 吗\n::delib\n::thinkredo\n' o24
grep -qa '回退：前提不成立' o24 && ok "痕上多了一条「回退：前提不成立」（**为什么退**写明了）" \
    || bad "回退那一步" "$(grep -a -m3 '回退' o24)"
grep -qa '退回分支起点换一条：改问「猫 不 会 跳舞」' o24 \
    && ok "换的是**另一条**：同一个问题**改问反面**（猫 不 会 跳舞）" \
    || bad "换哪一条" "$(grep -a -m2 '改问' o24)"
grep -qa '问前提' o24 && grep -qa '前提没成立' o24 \
    && ok "失败那一支的痕**留着**（不是删掉当没发生）" \
    || bad "失败分支的痕" "$(grep -a -m3 '问前提' o24)"
# 退回去之后**真的又试了一遍**：回退那一步之后还出现新的求解尝试
if awk '/退回分支起点换一条/{f=1;next} f&&/交给求解器/{found=1} END{exit found?0:1}' o24; then
    ok "退回去之后**真的换了那条再试**（回退之后还有一次求解）"
else
    bad "退回去之后没有再试" "$(grep -a -m4 '回退\|求解器' o24)"
fi
grep -qE '回退 [1-9] 次' o24 && ok "::delib 的元认知行报了回退次数" \
    || bad "回退计数" "$(grep -m1 '元认知' o24)"
grep -qE '退过 1 次' o24 && ok "::thinkredo 报了「退过 1 次」" \
    || bad "thinkredo 计数" "$(grep -m1 '退过' o24)"
# 预算**不退**：把预算压小，回溯不许把步数还回去（也就不会无限退下去）
run '::op 加前提 猫 鱼 饿\n::thinkredo on\n::thinkbudget 4\n猫 会 跳舞 吗\n::delib\n' o25
# 只数**痕里**那一行（`::thinkredo` 的说明文字里也有"退回分支起点换一条"这几个字 —— 踩过）
n_back=$(grep -ac -- '—— 退回分支起点换一条：改问' o25 || true)
[ "$n_back" -le 2 ] && ok "一轮最多退 2 次（上限真在管：这次退了 $n_back 次）" \
    || bad "回溯上限" "退了 $n_back 次"
grep -qE '停止原因：(原地打转|步数预算用完)' o25 && ok "小预算下照样收得住（回溯没绕过预算）" \
    || bad "预算" "$(grep -m1 '停止原因' o25)"

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS thinking（思维循环：议程 / 分步 / 反方向 / 跨轮 / 矛盾 / 元认知 / 未解问题持久化 / 思考对象 / 空闲思考 / 回溯，$pass 项）"
else
    echo "FAIL thinking（$fail 项失败，$pass 项通过）"
    exit 1
fi
