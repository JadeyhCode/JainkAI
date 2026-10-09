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

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS thinking（思维循环：议程 / 分步 / 反方向 / 跨轮 / 矛盾 / 元认知 / 未解问题持久化，$pass 项）"
else
    echo "FAIL thinking（$fail 项失败，$pass 项通过）"
    exit 1
fi
