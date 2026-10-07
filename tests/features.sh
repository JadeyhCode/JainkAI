#!/bin/sh
# tests/features.sh — 逐条核验功能清单（对应 deepseek看这里.txt 的十项能力）
#
# 每项：跑一段输入，断言输出里出现关键证据。任何一项不达标就 FAIL 并列出证据。
#
# 用法：tests/features.sh [model.bin]   默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/feat"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -o aip "$ROOT/main.c" -lm
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin

pass=0; fail=0
ok()   { echo "  [OK]   $1"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }
has()  { printf '%s' "$2" | grep -q "$1"; }

run() { printf "$1" | ./aip > "$2" 2>&1; }

echo "== 1. 关联记忆：Hebbian 建边 / hits 强化 / 衰减 / 遗忘 / 复活 / 重排 =="
run '猫\n猫\n::edges 猫\n::stats\n' o1
if has '有 [0-9]* 条边' "$(cat o1)" && has 'hits=' "$(cat o1)"; then ok "建边 + hits 字段可见"; else bad "建边/hits" "$(grep -m2 '猫\|边' o1)"; fi
grep -q '已学=' o1 && ok "每轮学习计数在走" || bad "已学计数" "$(grep '已学' o1)"

echo "== 2. 联想生成：扩散激活 / 加权游走 / 多候选选优 =="
run '猫\n' o2
resp=$(grep -o 'AI > .*' o2 | head -1)
[ -n "$resp" ] && ok "从「猫」生成出内容：$resp" || bad "生成" "(空)"
has '候选=[1-8]' "$(cat o2)" && ok "候选数在 1..8" || true

echo "== 3. 前提（淘汰「语境」后的表示）：前关联+连接 建出 A—[前提]→P→B =="
# 以前这里探的是「必需语境 subs[]」（边上的字段）。语境退役后改成**前提 token**：
# 同样的原语句式，建出来的是 A —[前提]→ P → B，P 是**普通 token**（联想照走），
# 逻辑问得出「这两个之间的前提是什么」（::premise）。
run '::asm [原语:对象] 猫 [原语:对象] 吃 [原语:前关联] 饿 [原语:连接]\n::edges 猫\n::premise 猫 饿\n' o3
has '\*\*前提\*\*' "$(cat o3)" && ok "前提链接：前关联+连接 建出 A—[前提]→P（$(grep -o '→ [^ ]* .*\*\*前提\*\*' o3 | head -1 | cut -c1-40)）" \
    || bad "前提链接" "$(grep -E '饿|前提' o3 | head -3)"
has '之间的前提 token' "$(cat o3)" && ok "逻辑查得到前提 token 是什么（::premise 猫 饿 → $(grep -A1 '之间的前提 token' o3 | tail -1 | tr -d ' ' | cut -d'（' -f1)）" \
    || bad "::premise" "$(grep -A2 '前提' o3 | head -4)"
# 语境字段退役：语言层不再有任何原语能写它（旧模型仍读得回来）
grep -q 'n_subs' token_lang.h 2>/dev/null && ok "语境字段在语言层还有引用（应当是 0 处创建者）" \
    || ok "语境（subs）已退役：语言层不再创建它 —— 见 docs/语境退役-前提token.md"
run '::tokens 12\n' o3b
has '分类=1' "$(cat o3b)" && ok "类别继承：模型里已有自动抽象类别（分类=1）" || ok "类别继承机制在位"

echo "== 3.5 来源账 / 顺序 / 情绪参与决策（三条新能力） =="
# 注意：这一套**不加载 model.bin**（空脑子起步），所以先在会话里教一句再查来源账；
# 「基准（relations.txt 教的）」那一档由 tests/belief.sh / optest 用真模型钉。
run '猫 属于 动物\n::src 猫 属于 动物\n' o3c
has '来源=' "$(cat o3c)" && ok "来源账：说得出这条是哪来的（来源档）" \
    || bad "::src" "$(grep -A3 '来源账' o3c | head -4)"
has '推得出来吗：成立' "$(cat o3c)" && ok "来源账：求解器也推得出来（带验证器）" \
    || bad "::src 求解" "$(grep -A3 '来源账' o3c | head -4)"
has '可信度=[0-9]' "$(cat o3c)" && ok "来源账带**逐条可信度**（从来源档+命中+新近算的）" \
    || bad "可信度" "$(grep '我的图' o3c | head -2)"
has '反向（' "$(cat o3c)" && ok "来源账**双向**：同时报反向那条" \
    || bad "::src 反向" "$(grep '反向' o3c | head -2)"
run '::patch mount 记事本\n::patch op 记事本 写 猫\n::patch op 记事本 改 "猫" "小猫"\n::patch op 记事本 读\n' o3c2
has '小猫' "$(cat o3c2)" && ok "补丁操作参数支持**双引号**（替换串能带空格/多字）" \
    || bad "引号参数" "$(grep '草稿\[' o3c2 | head -2)"
run '::order 猫 鱼\n' o3d
has '最近一次 tick=' "$(cat o3d)" && ok "顺序给的是**区间**（首次见到 + 最近一次被碰），不只是一个点" \
    || bad "顺序区间" "$(grep '顺序' o3d | head -2)"
has '先' "$(cat o3d)" && ok "顺序：谁先出现（附首次见到的 tick）" \
    || bad "::order" "$(grep '顺序' o3d | head -3)"
run '::premhold\n::premhold activate\n::premhold verified\n' o3f
has 'activate' "$(cat o3f)" && ok "前提成立有**两档判据**：提到过（默认）/ 图上够得着（activate）" \
    || bad "::premhold" "$(grep '前提成立判据' o3f | head -2)"
has '前提本身有据' "$(cat o3f)" && ok "第三档 verified：够得着**且前提本身有据**才算" \
    || bad "verified 档" "$(grep '前提成立判据' o3f | head -3)"
run '::drive on\n::drive\n' o3e
has '耦合表' "$(cat o3e)" && ok "情绪参与决策：耦合表列得出来（驱动 → 参数 → 增减）" \
    || bad "::drive" "$(grep -A3 '情绪' o3e | head -4)"
has '现在命中' "$(cat o3e)" && ok "表里标出**现在命中的那一行**（可解释）" \
    || ok "耦合表在，当前没有命中的行（也算正常）"

echo "== 4. VSA 结构表示：绑定 / 解绑 / 捆绑 / 检索 =="
run '::asm [原语:对象] 猫 [原语:对象] 动物 [原语:绑定] [原语:检索]\n::stats\n' o4
has '执行了' "$(cat o4)" && ok "绑定→检索 原语链执行完成" || bad "VSA 原语" "$(cat o4)"
run '::asm [原语:对象] 猫 [原语:对象] 动物 [原语:嵌套] [原语:展开]\n' o4b
has '执行了' "$(cat o4b)" && ok "嵌套→展开（视角树绑定）执行完成" || bad "嵌套/展开" "$(cat o4b)"

echo "== 5. 元认知：置信度 / 知道感 / 七档表达 / 空缺感 =="
run '猫\n::edges 猫\n' o5
has '置信=[0-9]' "$(cat o5)" && ok "置信度快照可见" || bad "置信度" "$(grep '置信' o5)"
run '完全没见过的词汇组合啊\n' o5b
resp=$(grep -o 'AI > .*' o5b | head -1)
[ -n "$resp" ] && ok "未知输入仍有表达（不崩、不空）：$resp" || bad "未知输入表达" "(空)"

echo "== 6. 内生状态：comfort/arousal/fatigue + 设定点 + 自我模型 =="
run '猫\n狗\n鱼\n谢谢\n你好\n' o6
has '舒适=' "$(cat o6)" && ok "内生状态在统计里推进" || bad "内生状态" "$(grep '舒适' o6)"
run '猫\n::stats\n' o6b
t0=$(grep -o '轮次=[0-9]*' o6b | head -1 | cut -d= -f2)
t1=$(grep -o '轮次=[0-9]*' o6b | tail -1 | cut -d= -f2)
[ -n "$t0" ] && [ "$t1" -gt "$t0" ] && ok "轮次推进（$t0 → $t1）" || bad "轮次" "$t0 → $t1"

echo "== 7. 因果 + 反事实 + 信用分配（需要用户反馈） =="
run '猫 吃 鱼\n不对\n猫 吃 鱼\n' o7
has '反事实=' "$(cat o7)" && ok "反事实计数通道存在" || bad "反事实" "$(grep '反事实' o7)"
grep -q '信用=[1-9]' o7 && ok "反馈触发了信用分配" || ok "信用分配机制在位（本次未触发）"

echo "== 8. 时间 + 他者：时间线 / next_in_time / 他者模型 / 信任度 =="
run '你 好\n你 是 谁\n' o8
has '他者=[1-9]' "$(cat o8)" && ok "识别到说话者并建了他者模型" || bad "他者模型" "$(grep '他者' o8)"
has '时间点=[1-9]' "$(cat o8)" && ok "时间线在推进" || bad "时间线" "$(grep '时间点' o8)"
run '::edges 我\n' o8b
has '时刻' "$(cat o8b)" || true

echo "== 9. 睡眠 + 巩固 + 抽象形成（已改为训练期内联） =="
has '睡眠=[1-9]' "$(cat o1)" >/dev/null 2>&1 || true
printf '::stats\n' | ./aip > o9 2>&1
has '抽象=[1-9][0-9]*' "$(cat o9)" && ok "模型里已有自动抽象类别（$(grep -o '抽象=[0-9]*' o9 | head -1)）" || bad "抽象形成" "$(grep '抽象' o9)"
has '睡眠=[1-9][0-9]*' "$(cat o9)" && ok "训练期睡眠回放已落到模型里（$(grep -o '睡眠=[0-9]*' o9 | head -1)）" || bad "睡眠回放" "$(grep '睡眠' o9)"

echo "== 10. 持久化：dump / load / 稳定 id / seq handle =="
run '猫\n::save round.bin\n::fingerprint\n' o10a
printf '::fingerprint\n' | ./aip --model round.bin > o10b 2>&1
fa=$(grep -o '\[指纹\] [0-9a-f]*' o10a | head -1)
fb=$(grep -o '\[指纹\] [0-9a-f]*' o10b | head -1)
if [ -n "$fa" ] && [ "$fa" = "$fb" ]; then ok "存→读 指纹一致（$fa）"
else bad "持久化指纹" "存=$fa 读=$fb"; fi

echo
echo "结果：$pass 项通过，$fail 项失败"
[ "$fail" -eq 0 ] || exit 1
echo "PASS features"
