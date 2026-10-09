#!/bin/sh
# tests/selfmod.sh — 微内核：自修改通道 / 安全内核 / 补丁边界（CLI 级）
#
# 单元级的东西（队列去重、预算、每条拒绝理由、检测器、元认知冻结）
# 在 tools/microtest.c 里；这里盯的是**端到端**：
#
#   ::mod / ::safe / ::patch 三个入口真的接在链路上，
#   「提议 → 审查 → 安全点应用 → 记账 → 审计」这一条走通了，
#   自修改**产生的结构随图存盘**（格式没动，不用重训），
#   补丁能热插拔、能停用、坏了不影响内核。
#
# 一条要紧的说明：`::mod try …` 是**手动调试入口**，不是自修改。
# 按定义自修改必须是系统运行中自己提的意图；手动试只是驱动同一套机制，
# 好让「这条边会被放行还是被哪条底线拦住」变成可验证的事。
#
# 用法：sh tests/selfmod.sh [模型文件]     默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/selfmod"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
# 顺带用 -Werror 看住新头文件（micro.h）的零警告
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
run() { printf '%b' "$1" | ./aip > "$2" 2>&1 || true; }
runm() { printf '%b' "$1" | ./aip --model "$2" > "$3" 2>&1 || true; }

echo "== 1. 默认状态：自修改开、不剪枝、一个补丁都没挂 =="
run '::mod\n::patch\n::stats\n' o1
grep -q '自修改通道=开' o1 && ok "自修改通道默认开" || bad "自修改默认开" "$(grep '自修改通道' o1)"
grep -q '休眠剪枝=关' o1 && ok "休眠剪枝默认关（删错比不删更糟）" || bad "剪枝默认" "$(grep '休眠剪枝' o1)"
grep -q '没有挂载任何补丁' o1 && ok "默认没有补丁：内核不依赖补丁" || bad "补丁默认" "$(grep '\[补丁\]' o1)"
grep -q '微内核：自修改=开' o1 && ok "::stats 里有微内核摘要行" || bad "stats 摘要" "$(grep '微内核' o1)"

echo "== 2. 提议 → 审查 → 安全点应用（手动入口走同一条链） =="
run '::mod try 加边 鱼 雪\n::mod\n' o2
grep -q '手动意图已应用.*鱼→雪（新）' o2 && ok "加边：应用并标明是新建" || bad "加边" "$(grep '手动意图' o2)"
grep -q '新边 1' o2 && ok "记账：新边 1 条" || bad "记账" "$(grep '应用 ' o2)"
run '::mod try 加边 鱼 雪\n::mod try 加边 鱼 雪\n::mod\n::edges 鱼\n' o3
grep -q '已有，只加权' o3 && ok "同一条再加：幂等（不重复建边）" || bad "幂等" "$(grep '手动意图' o3)"
grep -q '新边 1' o3 && ok "幂等之后仍是 1 条（没有变成 2）" || bad "幂等记账" "$(grep '应用 ' o3)"
grep -q '^  → 雪 ' o3 && ok "::edges 里能看到这条边" || bad "edges" "$(grep -m3 '→' o3)"
run '::mod try 加边 鱼 鱼\n' o5
grep -q '被拒.*不许自环' o5 && ok "自环被拒" || bad "自环" "$(grep '手动意图' o5)"
run '::mod try 加Token [原语:偷偷改] 鱼\n' o6
grep -q '被拒.*不许改保留 Token' o6 && ok "拿保留名造 Token 被拒" || bad "保留名" "$(grep '手动意图' o6)"
run '::mod try\n' o7
grep -q '用法' o7 && ok "参数不全时给用法（不静默）" || bad "用法" "$(cat o7)"

echo "== 3. 硬底线：关系边（知识）不许被删 =="
run '::mod try 打关系 猫 属于 动物\n::mod try 删边 猫 动物\n::safe\n' o8
grep -q '手动意图已应用.*打关系' o8 && ok "打关系放行" || bad "打关系" "$(grep '手动意图' o8)"
grep -q '被拒.*不许删关系边' o8 && ok "删关系边被拒（知识不是共现）" || bad "删关系边" "$(grep '手动意图' o8)"
grep -q '不许删关系边=[1-9]' o8 && ok "拒绝理由进了审计分类" || bad "审计分类" "$(grep '拒绝理由' o8)"

echo "== 4. 总开关 / 安全内核开关 / 冻结 =="
run '::mod off\n::mod try 加边 猫 雪\n::mod on\n::mod try 加边 猫 雪\n' o9
grep -q '被拒.*自修改总开关关着' o9 && ok "::mod off 之后一律拒绝" || bad "总开关" "$(grep '手动意图' o9)"
grep -q '手动意图已应用.*猫→雪' o9 && ok "::mod on 之后恢复" || bad "总开关恢复" "$(grep '手动意图' o9)"

run '::safe newtoken off\n::mod try 加Token [新概念:测试] 鱼\n::safe newtoken on\n::mod try 加Token [新概念:测试] 鱼\n::edges [新概念:测试]\n' o10
grep -q '被拒.*这一类修改被关着' o10 && ok "::safe newtoken off：造 Token 被拒" || bad "newtoken off" "$(grep '手动意图' o10)"
grep -q '手动意图已应用.*加Token' o10 && ok "放开后能造（并且立刻接回网络）" || bad "newtoken on" "$(grep '手动意图' o10)"
grep -q '^  → 鱼 ' o10 && ok "新 Token 挂在触发它的上下文上（不是孤岛）" || bad "新 Token 连接" "$(grep -m3 '→' o10)"

run '::safe freeze on\n::mod try 加边 狗 雪\n::safe reset\n::mod try 加边 狗 雪\n' o12
grep -q '被拒.*已冻结' o12 && ok "冻结时一律拒绝" || bad "冻结" "$(grep '手动意图' o12)"
grep -q '手动意图已应用.*狗→雪' o12 && ok "::safe reset 解冻后恢复（不是死锁）" || bad "解冻" "$(grep '手动意图' o12)"

run '::mod try 加边 鱼 雪\n::safe remove off\n::mod try 删边 鱼 雪\n::safe remove on\n::mod try 删边 鱼 雪\n' o13
grep -q '被拒.*这一类修改被关着' o13 && ok "::safe remove off：删边被拒" || bad "remove off" "$(grep '手动意图' o13)"
grep -q '手动意图已应用.*删 鱼→雪' o13 && ok "放开后能删普通共现边" || bad "remove on" "$(grep '手动意图' o13)"

echo "== 5. 自修改产生的结构随图存盘（格式没动，不用重训） =="
run '::mod try 加边 鱼 雪\n::save slot1\n' o14
[ -s slot1 ] && ok "存盘成功" || bad "存盘" "$(tail -2 o14)"
runm '::edges 鱼\n::quit\n' slot1 o15
grep -q '雪' o15 && ok "加载后这条边还在（自修改进了图，就随图存）" || bad "存读" "$(cat o15)"

echo "== 6. 补丁边界：热插拔 / 停用 / 坏补丁不拖累内核 =="
run '::patch mount 自省\n完全没见过的词汇组合啊\n你好\n::patch\n' o16
grep -q '已挂载补丁 自省' o16 && ok "挂载内建补丁" || bad "挂载" "$(grep '补丁' o16)"
grep -q '自省.*类型=感知 状态=启用' o16 && ok "补丁在跑（状态=启用）" || bad "补丁状态" "$(grep '自省' o16)"
grep -qE '输出=[1-9]' o16 && ok "感知输出进了内核（把内核状态变成了 Token）" || bad "补丁输出" "$(grep '自省' o16)"
run '::patch mount 自省\n::patch off 自省\n你好\n::patch\n' o17
grep -q '已停用' o17 && ok "::patch off 之后停用" || bad "停用" "$(grep '自省' o17)"
grep -qE '输出=0\(最近0\)' o17 && ok "停用后不再感知（输出 0）" || bad "停用输出" "$(grep '自省' o17)"
run '::patch mount 自省\n::patch unmount 自省\n::patch\n' o18
grep -q '没有挂载任何补丁' o18 && ok "卸载后回到「没有补丁」" || bad "卸载" "$(grep '\[补丁\]' o18)"
run '::patch mount 不存在的补丁\n' o19
grep -q '没有这个补丁' o19 && ok "挂一个不存在的补丁：说清楚，不崩" || bad "缺补丁" "$(cat o19)"

echo "== 7. 输出侧补丁「记事本」：可编辑的答复 + 草稿区 + 自己决定什么时候发送 =="
# ① 挂上它**不改行为**：没编辑时输出与不挂时逐字节相同
run '猫\n' o20
a_note=$(grep -o 'AI > .*' o20 | head -1)
run '::notepad off\n::patch mount 记事本\n猫\n' o21
a_patch=$(grep -o 'AI > .*' o21 | head -1)
if [ -n "$a_note" ] && [ "$a_note" = "$a_patch" ]; then
    ok "挂上「记事本」（auto 模式）：回答与不挂时**逐字节相同**"
else
    bad "auto 模式不该改行为" "无补丁=[$a_note] 有补丁=[$a_patch]"
fi
# ② 草稿区：写 → 读 → 提交（草稿不许影响发言）
run '::notepad off\n::patch mount 记事本\n::patch op 记事本 写 猫\n::patch op 记事本 插入 1 会 跑\n::patch op 记事本 读\n::patch op 记事本 改 会 在\n::patch op 记事本 删 1 1\n::patch op 记事本 读\n::patch op 记事本 提交\n::patch op 记事本 读\n' o22
grep -q '草稿\[10字节\]=猫会 跑' o22 && ok "草稿区：写 + 按字符位置插入" || bad "草稿写/插" "$(grep '草稿\[' o22 | head -3)"
grep -q '草稿\[7字节\]=猫 跑' o22 && ok "草稿区：改 + 按字符位置删（不劈开汉字）" || bad "草稿改/删" "$(grep '草稿\[' o22 | head -3)"
grep -q '待发送\[7字节\]=猫 跑' o22 && ok "提交：草稿进待发送区" || bad "提交" "$(grep '待发送' o22 | head -3)"
# ③ 手动模式：AI 控制什么时候发送（不发送 = 这一轮不说话）
run '::notepad off\n::patch mount 记事本\n::patch op 记事本 模式 manual\n猫\n' o23
grep -qE '你 > AI > *$' o23 && ok "manual：没发送 → 这一轮**不说话**（流水线那句被扣在待发送区）" \
    || bad "manual 该沉默" "$(grep 'AI >' o23 | tail -2)"
run '::notepad off\n::patch mount 记事本\n::patch op 记事本 模式 manual\n猫\n::patch op 记事本 改发言 看 跑\n::patch op 记事本 发送\n猫 有 毛 吗\n' o24
grep -qE 'AI > 猫 跑 来 跑' o24 && ok "编辑 + 发送：说出去的是**它自己改过的那句**（猫 看 来 跑 → 猫 跑 来 跑）" \
    || bad "编辑后的文本没发出来" "$(grep 'AI >' o24 | tail -3)"
# ④ 受审通道：AI 自己（或 CLI）调用补丁操作同样过安全内核
run '::notepad off\n::patch mount 记事本\n::op 调用补丁 记事本 写 你好\n::mod\n::patch op 记事本 读\n' o25
grep -q '应用 [1-9]' o25 && ok "「调用补丁」走审查→记账→应用→审计（和自修改同一条路）" \
    || bad "受审通道" "$(grep -E '行为|应用' o25 | head -3)"
grep -q '草稿\[6字节\]=你好' o25 && ok "补丁的草稿真的被写了（外设只能碰自己的缓冲）" || bad "草稿" "$(grep '草稿' o25 | head -2)"
# ⑤ 故障隔离：不认的操作连错 3 次 → 补丁自己停用，内核照跑
run '::notepad off\n::patch mount 记事本\n::patch op 记事本 没有这个操作\n::patch op 记事本 没有这个操作\n::patch op 记事本 没有这个操作\n::patch\n你好\n' o26
grep -q '不认这个操作' o26 && ok "不认的操作：说清楚（不崩）" || bad "坏操作" "$(grep '补丁' o26 | head -3)"
grep -q '已停用' o26 && ok "连续 3 次出错 → 补丁自己停用" || bad "故障隔离" "$(grep '记事本' o26 | head -3)"
grep -q 'AI >' o26 && ok "补丁停用后内核照跑（照常回答）" || bad "内核" "$(grep 'AI >' o26 | tail -1)"

echo "== 8. D4 条件依赖：前提从反复出现的谈话里**自己长出来** =="
# 行为断言都**显式写开关**（不依赖环境默认），这样反向测量 AIP_D4=1 也能整包跑绿；
# 默认值单列一条断言（被环境变量强制时跳过）——规矩同 ::premiseout 那次。
run '::mod cond off\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n::premise 猫 鱼\n' o27
grep -q '没有前提' o27 && ok "关掉时：反复说也不会自己长前提（行为不变）" \
    || bad "关掉该不长" "$(grep '前提' o27 | head -2)"
if [ "${AIP_D4:-}" = "1" ] || [ "${AIP_D4:-}" = "0" ]; then
    ok "反向测量（AIP_D4=${AIP_D4}）：跳过默认值断言"
else
    run '::mod\n' o27b
    grep -q '条件依赖=关' o27b && ok "D4 **默认关**（会在对话里改结构，先测量）" \
        || bad "D4 默认值" "$(grep '检测器' o27b | head -2)"
fi
# 打开：同一批话 → 检测器量出「水几乎总在场」→ 提议并应用前提链接
run '::mod cond on\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n::premise 猫 鱼\n::mod\n' o28
grep -q '前提：水' o28 && ok "打开后：三次同轮共现 → **自己长出** 猫 —[前提]→ 水 → 鱼" \
    || bad "D4 没长出来" "$(grep -E '前提|条件依赖' o28 | head -4)"
grep -q '条件依赖=开(提[1-9]/成[1-9])' o28 && ok "::mod 里如实报 提/成 计数" \
    || bad "D4 计数" "$(grep '检测器' o28 | head -2)"
run '::mod cond on\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼 水\n猫 鱼\n猫 鱼\n猫 鱼\n::premise 猫 鱼\n' o29
grep -q '没有前提' o29 && ok "一半轮次没有「水」→ 比例不够 → **不当前提**（分母独立）" \
    || bad "比例判据" "$(grep '前提' o29 | head -2)"

echo "== 8.5 草稿持久化：存成 [草稿:…] 标记节点（不改存档格式） =="
# 存草稿 → 下一轮的安全点落地（提议只在安全点应用）→ 卸载重挂，草稿被扫回来
run '::notepad off\n::patch mount 记事本\n::patch op 记事本 写 小猫\n::patch op 记事本 存草稿\n你好\n::mod\n::patch unmount 记事本\n::patch mount 记事本\n::patch op 记事本 读\n' o34
grep -q '草稿:小猫' o34 && ok "草稿经安全内核落成 **[草稿:小猫]** 标记节点（普通 token → 随图存盘）" \
    || bad "草稿标记" "$(grep -E '草稿|应用 |加Token' o34 | head -4)"
grep -q '草稿\[6字节\]=小猫' o34 && ok "重新挂载后草稿**被扫回来**（重启也同理，格式没动）" \
    || bad "草稿恢复" "$(grep '草稿\[' o34 | tail -2)"

echo "== 9. AI 自己用记事本：写草稿 / 决定什么时候发送（默认关） =="
if [ "${AIP_NOTEPAD:-}" = "on" ] || [ "${AIP_NOTEPAD:-}" = "hold" ] || [ "${AIP_NOTEPAD:-}" = "off" ]; then
    ok "反向测量（AIP_NOTEPAD=${AIP_NOTEPAD}）：跳过默认值断言"
else
    run '::notepad\n' o30
    grep -q '记事本 = 关（默认）' o30 && ok "**默认关**：不显式打开就一个字都不插手" \
        || bad "默认值" "$(grep '记事本 =' o30 | head -2)"
fi
# 有结论 + on：说出来的是**它自己写的那句**（不是流水线那句，链式渲染不见了）
run '::notepad off\n::patch mount 记事本\n::notepad on\n猫 会 唱歌 吗\n' o31
grep -q 'AI > 猫 不 会 唱歌' o31 && ok "on：AI 把结论写进草稿并发送 → 说的是它自己写的那句" \
    || bad "on 模式" "$(grep 'AI >' o31 | tail -2)"
grep -q '依据：' o31 && ok "on：**依据（链）也带回来了**（规则名/链式渲染不再丢）" \
    || bad "on 模式的依据" "$(grep 'AI >' o31 | tail -2)"
grep -q -- '-\[会' o31 && ok "链上看得见走的是哪条规则" \
    || bad "规则名" "$(grep 'AI >' o31 | tail -2)"
# 没结论 + hold：按住不说（"控制什么时候发送"）
run '::notepad off\n::patch mount 记事本\n::notepad hold\n猫 会 跳舞 吗\n::delib\n' o32
grep -qE '你 > AI > *$' o32 && ok "hold：没想清楚就**不说话**（宁可不说，也不硬答）" \
    || bad "hold 该沉默" "$(grep 'AI >' o32 | tail -2)"
grep -q '按住不说' o32 && ok "::delib 里看得见这一步（痕上有「按住不说」）" \
    || bad "hold 的痕" "$(grep -E '按住|登记' o32 | head -3)"
# 关掉：回到流水线（照常说）
run '::notepad off\n::patch mount 记事本\n::notepad off\n猫 会 跳舞 吗\n' o33
grep -q 'AI > 猫' o33 && ok "off：回到流水线（照常回答，一个字不插手）" \
    || bad "off 该照常" "$(grep 'AI >' o33 | tail -2)"

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS selfmod（微内核：自修改 / 安全内核 / 补丁边界 / 记事本 / D4 / AI 自己用记事本，$pass 项）"
else
    echo "FAIL selfmod（$fail 项失败，$pass 项通过）"
    exit 1
fi
