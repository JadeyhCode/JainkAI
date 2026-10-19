#!/bin/sh
# tests/relations.sh — L2 语义关系层的回归
#
# 看护四件事：
#   ① 静态知识 + 传递推理：`猫 有 毛 吗` 靠 猫→动物→毛 推出来，
#      而且**把推导路径打出来**（这才是「明确思维链」）
#   ② 对话中动态学知识：用户陈述一句事实就当场学进图里，之后能用来回答，
#      并且**存盘再读还在**（rel 位掩码写进了模型 v6）
#   ③ 闭合世界的「不是」只对知识库里的实体说，不许抢答普通问句
#   ④ 不误学：疑问句（你 是 谁）不当事实记下来
#   ⑥ 合写否定（不会/不喜欢/不是）与两词写法（不 会）走**同一份声明**、同答案
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }
[ -f relations.txt ] || { echo "FAIL: 缺 relations.txt"; exit 1; }

TMP=./_rel_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi }

run() {   # <输入...>  → 打印回复行
    { printf '::load %s\n::char off\n::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n' "$MODEL"
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'
    } | "$BIN" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }
nth()  { sed -n "${1}p"; }

echo "⓪ §6.144 关系别名可达（靠 lexicon.txt 把多字词保住整词）"
# 别名只对**分词器不切开**的词有效：`喜爱` 不被切开才轮得到 `REL_ALIASES` 说话。
out=$(run "我 喜爱 猫" "我 惧怕 狗" "::rel 我")
printf '%s\n' "$out" | grep -q -- '--\[喜欢\]--> 猫' && chk "「喜爱」归到核心关系「喜欢」（不是凭空造个「喜」）" 1 \
                                             || chk "「喜爱」归到「喜欢」（实得：$(printf '%s\n' "$out" | grep -- '-->' | head -2 | tr '\n' ' ')）" 0
printf '%s\n' "$out" | grep -q -- '--\[怕\]--> 狗' && chk "「惧怕」归到核心关系「怕」" 1 \
                                             || chk "「惧怕」归到「怕」" 0

echo "① 静态知识 + 多跳推理（带推导路径）"
out=$(run "猫 有 毛 吗" "猫 属于 生物 吗" "鸟 会 飞 吗" "猫 吃 鱼 吗")
r1=$(printf '%s\n' "$out" | reps | nth 1)
r2=$(printf '%s\n' "$out" | reps | nth 2)
r3=$(printf '%s\n' "$out" | reps | nth 3)
r4=$(printf '%s\n' "$out" | reps | nth 4)
echo "     猫 有 毛 吗      → $r1"
echo "     猫 属于 生物 吗  → $r2"
echo "     鸟 会 飞 吗      → $r3"
echo "     猫 吃 鱼 吗      → $r4"
case "$r1" in *"猫 -[属于·继承]-> 动物 ； 动物 -[有]-> 毛"*) chk "猫 有 毛：多跳推出且打了**带规则名**的链" 1 ;;
             *) chk "猫 有 毛：多跳推出且打了带规则名的链（实得「$r1」）" 0 ;; esac
case "$r2" in *"猫 -[属于·传递]-> 动物 ； 动物 -[属于]-> 生物"*) chk "猫 属于 生物：传递推理且标了「传递」" 1 ;;
             *) chk "猫 属于 生物：传递推理且标了「传递」（实得「$r2」）" 0 ;; esac
case "$r3" in "对 ， 鸟 -[会]-> 飞"*) chk "鸟 会 飞：直接事实" 1 ;;
             *) chk "鸟 会 飞：直接事实（实得「$r3」）" 0 ;; esac
case "$r4" in "对 ， 猫 -[吃]-> 鱼"*) chk "猫 吃 鱼：直接事实" 1 ;;
             *) chk "猫 吃 鱼：直接事实（实得「$r4」）" 0 ;; esac

echo "② 闭合世界：知识库里的实体才答「不是」"
out=$(run "猫 会 飞 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in "不 对"*"猫 -[不 会]-> 飞"*) chk "猫 会 飞 吗 → 不 对，且给出否定依据（$r）" 1 ;;
             *) chk "猫 会 飞 吗 → 不 对，且给出否定依据（实得「$r」）" 0 ;; esac
# 普通问句不许被关系层抢答；§6.275 起也不再渲染成「我会试着游泳」，而是明说没记过
out=$(run "你 会 游泳 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in "这 件 事 我 不 敢 说 会"*) chk "你 会 游泳 吗：没被关系层抢答、也没假装会（$r）" 1 ;;
             *) chk "你 会 游泳 吗：没被关系层抢答、没假装会（实得「$r」）" 0 ;; esac

echo "③ 对话中动态学知识，并且**存盘再读还在**"
out=$(run "猫 属于 宠物" "猫 属于 宠物 吗")
r1=$(printf '%s\n' "$out" | reps | nth 1)
r2=$(printf '%s\n' "$out" | reps | nth 2)
echo "     陈述 → $r1"
echo "     追问 → $r2"
case "$r1" in "记下 了 ： 猫 属于 宠物") chk "陈述句被当场学下来（$r1）" 1 ;;
             *) chk "陈述句被当场学下来（实得「$r1」）" 0 ;; esac
case "$r2" in "对 ， 猫 -[属于]-> 宠物"*) chk "刚学的知识立刻能用来回答（$r2）" 1 ;;
             *) chk "刚学的知识立刻能用来回答（实得「$r2」）" 0 ;; esac
# 存盘 → 重载 → 还在吗？（rel 位掩码写进模型 v6）
{ printf '::load %s\n::char off\n猫 属于 宠物\n::save %s/m.bin\n::load %s/m.bin\n::char off\n猫 属于 宠物 吗\n::quit\n' \
      "$MODEL" "$TMP" "$TMP"; } | "$BIN" 2>&1 | sed -n 's/.*AI > //p' > "$TMP/persist.txt"
last=$(tail -1 "$TMP/persist.txt")
case "$last" in "对 ， 猫 -[属于]-> 宠物"*) chk "存盘重载后动态知识还在（$last）" 1 ;;
              *) chk "存盘重载后动态知识还在（实得「$last」）" 0 ;; esac

echo "④ 不误学：疑问句不当事实"
out=$(run "你 是 谁")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"记下 了"*) chk "「你 是 谁」不该被当成事实记下" 0 ;;
             *) chk "「你 是 谁」没被当成事实记下（现答：$r）" 1 ;; esac
# §6.171：分词把「多少钱」拆成 多/少/钱，整词检查看不出问句；rel_parse 拼回整词后要查第二遍
out=$(run "这个 东西 多 少 钱" "::rel 这个")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"记下 了"*) chk "「多 少 钱」不许被当事实学（实得「$r」）" 0 ;;
             *) chk "「这个 东西 多 少 钱」没被学成事实（现答：$r）" 1 ;; esac
printf '%s\n' "$out" | grep -q -- '--\[东西\]-->' \
    && chk "图里没有「东西」这条垃圾谓语" 0 \
    || chk "图里没有「东西」这条垃圾谓语" 1
out=$(run "猫 属于 动物")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in "我 知道 ： 猫 属于 动物") chk "已知事实回「我 知道」而不是重复学（$r）" 1 ;;
             *) chk "已知事实回「我 知道」（实得「$r」）" 0 ;; esac

# §6.199：独占关系被记了多个宾语时，::rel 要把它**说出来**（学的时候没拦，至少要能查出来）
out=$(run "我 属于 动物" "我 是 小明" "::rel 我")
printf '%s\n' "$out" | grep -q "「属于」在数据里是\*\*独占\*\*关系，这里却记了 2 个宾语" \
    && chk "独占关系记了 2 个宾语 ⇒ ::rel 有⚠提示" 1 \
    || chk "独占关系记了 2 个宾语 ⇒ ::rel 有⚠提示" 0 "$(printf '%s\n' "$out" | grep -a "⚠" | head -1)"

# §6.214 反查表是"只追加 + 读时校验"的：漏写一条 = 漏知识，所以拿图当基准逐条对拍
out=$(run "::revcheck")
printf '%s\n' "$out" | grep -q "反查表缺 0 条" \
    && chk "::revcheck：出厂模型反查表不缺条目" 1 \
    || chk "::revcheck" 0 "$(printf '%s\n' "$out" | grep -a revcheck)"
# §6.223 类比猜出来的那条也要进反查表（`agent_guess_remember` 是直写 rel 位的，
# 第一版漏了追加 ⇒ revcheck 报"缺 1 条：狗 -[会]-> 唱歌"）
# §6.225 混合写入（地点 + 继承 + 类比猜测 + 动态否定 + 造词）之后，反查表仍要一条不缺
out=$(run "我 住 在 北京" "我 在 学校 学习" "狗 会 唱歌 吗" "猫 不 喵 鱼" "::natcoin on" "麒麟 属于 神兽" "::revcheck")
printf '%s\n' "$out" | grep -q "反查表缺 0 条" \
    && chk "::revcheck：混合写入之后也一条不缺" 1 \
    || chk "::revcheck（混合写入）" 0 "$(printf '%s\n' "$out" | grep -a revcheck)"
out=$(run "猫 不 喵 鱼" "::revcheck")
printf '%s\n' "$out" | grep -q "反查表缺 0 条" \
    && chk "::revcheck：否定的动态关系也进了反查表" 1 \
    || chk "::revcheck（动态否定）" 0 "$(printf '%s\n' "$out" | grep -a revcheck)"
out=$(run "狗 会 唱歌 吗" "::revcheck")
printf '%s\n' "$out" | grep -q "反查表缺 0 条" \
    && chk "::revcheck：类比猜出来的那条也进了反查表" 1 \
    || chk "::revcheck（猜过之后）" 0 "$(printf '%s\n' "$out" | grep -a revcheck)"
out=$(run "::natcoin on" "考拉 属于 动物" "狗 会 唱歌 吗" "不 对" "::revcheck")
printf '%s\n' "$out" | grep -q "反查表缺 0 条" \
    && chk "::revcheck：现场教的两条也都进了反查表" 1 \
    || chk "::revcheck（教完）" 0 "$(printf '%s\n' "$out" | grep -a revcheck)"

echo "⑤ ::why 打出推导，::rel 列出知识，::relinfer off 可关"
out=$(run "::why 猫 有 毛" "::rel 猫")
printf '%s\n' "$out" | grep -q '猫 -\[属于·继承\]-> 动物' \
    && chk "::why 打出带规则名的推导路径" 1 || chk "::why 打出带规则名的推导路径" 0
printf '%s\n' "$out" | grep -q -- '--\[属于\]--> 动物' \
    && chk "::rel 列出关系知识" 1 || chk "::rel 列出关系知识" 0
out=$(run "::relinfer off" "猫 有 毛 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"对 ， 猫 -[属于·继承]-> 动物 ； 动物 -[有]-> 毛"*) chk "::relinfer off 时不再推理" 0 ;;
             *) chk "::relinfer off 时不再推理（现答：$r）" 1 ;; esac

echo "== ⑥ 合写否定（不会/不喜欢/不是）：同一份声明的**两种写法**都要认 =="
# 关系表声明的是两词形式（`不 会`），但分词器把最常见的「不会」切成**一个** token，
# 而「不属于/不能/不敢」却是两词 —— 修之前合写的整条掉进降级答复（audit §6.69）。
out=$(run "猫 不会 飞 吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
case "$r" in *"不 太 懂"*|*"不 太 懂"*) chk "合写「不会」不再掉进降级答复（现答：$r）" 0 ;;
             *) chk "合写「不会」认出来了（$r）" 1 ;; esac
printf '%s\n' "$out" | grep -aq '对 ， 猫 -\[不 会\]-> 飞' \
    && chk "而且答对了（猫 不会 飞 → 对）" 1 || chk "答案（实得：$r）" 0
# 两种写法必须**同答案**（同一条声明，两个切法）
a=$(run "猫 不会 飞 吗" | reps | nth 1)
b=$(run "猫 不 会 飞 吗" | reps | nth 1)
[ "$a" = "$b" ] && chk "合写与两词写法**同答案**（$a）" 1 \
                || chk "两种写法不一致（合写=$a / 两词=$b）" 0
out=$(run "猫 不是 动物 吗")
printf '%s\n' "$out" | grep -aq '猫 -\[属于\]-> 动物' \
    && chk "「不是」也认（按 属于 的否定走）" 1 \
    || chk "不是（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0
# 归属：合写否定说给他听时，存进**他名下**（并且不把我自己的知识改掉）
out=$(run "甲 说 猫 不会 游泳" "::others")
printf '%s\n' "$out" | grep -aq '记下 了 ： 甲 说 猫 不 会 游泳' \
    && chk "合写否定进他的信念表，而且**不双重否定**（甲 说 猫 不 会 游泳）" 1 \
    || chk "归属（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0
# 肯定句不受影响（别把关系词本身当否定）
out=$(run "猫 会 飞 吗")
printf '%s\n' "$out" | grep -aq '猫 -\[不 会\]-> 飞' \
    && chk "肯定句照旧（猫 会 飞 吗 → 依据仍是「不 会」那条否定边）" 1 \
    || chk "肯定句（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0


echo "== ⑦ 情态说法（不能/没法）：说法的归一，不是新语义 =="
# 合写否定修好之后剩下的尾巴：`不能/没法` 不是任何关系的"否定形式"，而是**情态写法**；
# 归到哪条关系上是**一张数据表**（`REL_MODALS[]`），加一行多认一个说法。
out=$(run "猫 不能 飞 吗")
printf '%s\n' "$out" | grep -aq '猫 -\[不 会\]-> 飞' \
    && chk "「不能」归到「会」的否定上（不再掉降级答复）" 1 \
    || chk "不能（实得：$(printf '%s\n' "$out" | reps | nth 1)）" 0
out=$(run "猫 没法 飞 吗")
printf '%s\n' "$out" | grep -aq '猫 -\[不 会\]-> 飞' \
    && chk "「没法」同归一（分词器把它切成 没+法，也在表里认）" 1 || chk "没法" 0
# 三种写法同答案：不能 / 没法 / 不 会
a=$(run "猫 不能 飞 吗" | reps | nth 1); b=$(run "猫 没法 飞 吗" | reps | nth 1)
c=$(run "猫 不 会 飞 吗" | reps | nth 1)
[ "$a" = "$b" ] && [ "$b" = "$c" ] && chk "三种写法**同答案**（$a）" 1 \
    || chk "三种写法不一致（$a / $b / $c）" 0
out=$(run "甲 说 猫 不能 游泳" "::others")
printf '%s\n' "$out" | grep -aq '记下 了 ： 甲 说 猫 不 会 游泳' \
    && chk "归属：情态说法也进他的信念表（归一后记成 不 会）" 1 || chk "归属" 0
# 不回归：没有/没 有 那条路（有 的否定）与肯定句照旧
out=$(run "猫 没有 毛")
printf '%s\n' "$out" | grep -aq '记下 了 ： 猫 没有 毛' \
    && chk "「没有」那条路照旧（没被情态表抢走）" 1 || chk "没有" 0
out=$(run "猫 有 毛 吗")
printf '%s\n' "$out" | grep -aq '动物 -\[有\]-> 毛' \
    && chk "肯定句照旧" 1 || chk "肯定句" 0
# 旧实现会把「能」「法」当场登记成**动态谓语**（图上多个脏谓语）——
# 判据：问情态说法**不会多出**动态谓语（出厂模型本来就有几个，所以比"多没多"，不比绝对值）。
d_plain=$(run "猫 有 毛 吗" "::stats" | grep -a '动态关系' | head -1)
d_modal=$(run "猫 不能 飞 吗" "::stats" | grep -a '动态关系' | head -1)
[ -n "$d_plain" ] && [ "$d_plain" = "$d_modal" ] \
    && chk "问情态说法**不会多出**动态谓语（旧实现会把「能」登记进去）: $d_modal" 1 \
    || chk "动态谓语多了（普通=$d_plain / 情态=$d_modal）" 0



echo "== ⑧ 动态关系的否定（清单外的谓语也认「不」） =="
out=$(run "猫 喵 鱼" "猫 不 喵 狗")
printf '%s\n' "$out" | grep -aq '记下 了 ： 猫 喵 鱼' && chk "动态谓语（清单外）：肯定照样现场学" 1 || chk "动态肯定" 0
printf '%s\n' "$out" | grep -aq '记下 了 ： 猫 不 喵 狗' \
    && chk "动态谓语 + 否定：**现在也学得进**（以前整句掉降级）" 1 \
    || chk "动态否定（实得：$(printf '%s\n' "$out" | reps | nth 3)）" 0

echo "== ⑨ ② 触发接求解：缺一端的问句（§6.200 起**默认开**；::rel open off 退回） =="
if [ -z "${AIP_RELOPEN:-}" ]; then
    out=$(run "::rel open off" "猫 属于 什么")
    printf '%s\n' "$out" | grep -aq '我 记 的 是 ： 猫 属于' \
        && chk "关掉之后不该答开放式问句" 0 || chk "关掉：照旧「不太懂」（退回老行为）" 1
    out=$(run "猫 属于 什么")
    printf '%s\n' "$out" | grep -aq '我 记 的 是 ： 猫 属于 动物' \
        && chk "默认（开着）就答开放式问句（§6.200 翻的默认）" 1 \
        || chk "默认就答开放式问句" 0 "$(printf '%s\n' "$out" | reps | head -1)"
fi
out=$(run "::rel open on" "猫 属于 什么" "谁 属于 动物" "猫 会 什么")
printf '%s\n' "$out" | grep -aq '我 记 的 是 ： 猫 属于 动物' \
    && chk "开着：「A R 什么」正向查邻居 ⇒ 答出宾语" 1 \
    || chk "正向（实得：$(printf '%s\n' "$out" | reps | head -1)）" 0
printf '%s\n' "$out" | grep -aq '属于 动物 的 有 ： 猫' \
    && chk "开着：「谁 R B」**反向扫** ⇒ 答出主语（还列了多个）" 1 \
    || chk "反向（实得：$(printf '%s\n' "$out" | reps | sed -n 2p)）" 0
printf '%s\n' "$out" | grep -aq '猫 会' && chk "换一条关系也走得通（「猫 会 什么」）" 1 || chk "换关系" 0

# §6.369 两个已知实体 + 已知关系 + 图里没有这条边 ⇒ 说「不确定」，不是「不太懂」
echo "⑩ §6.369 听懂了但图里没有：说「不确定」"
out=$(run "狗 吃 猫 吗" "鸟 吃 猫 吗" "猫 吃 鱼 吗" "狗 吃 猫 吗" "狗 不 吃 猫 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 不 确定 狗 吃 不 吃 猫" \
    && chk "「狗 吃 猫 吗」⇒ 我不确定（不是「不太懂」）" 1 \
    || chk "未知关系说「不确定」" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 确定" \
    && chk "换一对实体也一样（鸟 吃 猫 吗）" 1 \
    || chk "换实体" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "对" \
    && chk "已知事实不受影响（猫 吃 鱼 吗 ⇒ 对）" 1 \
    || chk "已知事实未受影响" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "是不是 不 吃 猫" \
    && chk "否定的未知关系也说得清楚（狗 不 吃 猫 吗）" 1 \
    || chk "否定式" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"

# §6.370 数量 / 年龄 / 比较 / 时间词族
echo "⑪ §6.370 数量·年龄·比较说清「没记数目/不会比较」；时间词族走「没有时钟」"
out=$(run "猫 有 几 条 腿" "猫 几 岁 了" "猫 比 狗 大 吗" "后天 星期几")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "只 记 了 猫 有 腿" \
    && chk "「猫有几条腿」⇒ 不知道数目，但说得出图里有「猫有腿」" 1 \
    || chk "数量（继承）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "没 记 过 年龄" \
    && chk "「猫几岁了」⇒ 没记过年龄" 1 \
    || chk "年龄" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 会 比 较" \
    && chk "「猫比狗大吗」⇒ 不会比较（不是「不太懂」）" 1 \
    || chk "比较" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "没有 时 钟" \
    && chk "「后天星期几」⇒ 没有时钟（时间词族都能命中）" 1 \
    || chk "时间词族" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.372 反问/强调问（难道 / 不是…吗）：纯形状行，删掉"难道"后照常判极性
echo "⑫ §6.372 反问句：删掉强调标记，极性照常判；关掉 shape 桶就退回兜底"
out=$(run "猫 难 道 不是 动物 吗" "猫 难 道 不会 飞 吗" "猫 不是 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 对.*猫.*属于.*动物" \
    && chk "「猫难道不是动物吗」⇒ 对（纠正嵌入式否定：猫属于动物）" 1 \
    || chk "难道+不是" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对.*不 会.*飞" \
    && chk "「猫难道不会飞吗」⇒ 对，猫不会飞" 1 \
    || chk "难道+不会" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 对.*不 会" \
    && chk "「猫不是会飞吗」⇒ 不对（不再答出「不是会 不会」那种乱句）" 1 \
    || chk "不是…吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(AIP_NODATA=shape run "猫 难 道 不是 动物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 太 懂" \
    && chk "关掉 shape 桶 ⇒ 同一句退回兜底（证明是那几行数据在起作用）" 1 \
    || chk "shape 消融没退回" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS relations（L2：静态知识/传递推理/对话中现学/持久化/不误学/合写否定/情态说法/动态否定/触发接求解）" \
                  || { echo "FAIL relations"; exit 1; }
