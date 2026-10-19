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

# §6.397 **空格无关的断言**：`has "<答复>" "<期望>"` —— 两边都去掉空格再比（字面子串）。
# 为什么要有：这个月有 6 次断言写成「天 气 我 不 知 道」「没 有 时 钟」这种**逐字空格**的形式，
# 而答复里 `天气`/`没有` 各是一个词 ⇒ 假红。新写的断言一律用 `has`，别再手写 grep -q。
# （`grep -q` 仍保留给需要正则/行首锚点的断言。）
has() {
    _t=$(printf '%s' "$1" | tr -d ' \t')
    _p=$(printf '%s' "$2" | tr -d ' \t')
    case "$_t" in *"$_p"*) return 0 ;; *) return 1 ;; esac
}

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
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 不 确定 狗 吃 不 吃 猫" \
    && chk "「狗 吃 猫 吗」⇒ 我不确定（不是「不太懂」）" 1 \
    || chk "未知关系说「不确定」" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "不 确定" \
    && chk "换一对实体也一样（鸟 吃 猫 吗）" 1 \
    || chk "换实体" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "对" \
    && chk "已知事实不受影响（猫 吃 鱼 吗 ⇒ 对）" 1 \
    || chk "已知事实未受影响" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 5p)" "是不是 不 吃 猫" \
    && chk "否定的未知关系也说得清楚（狗 不 吃 猫 吗）" 1 \
    || chk "否定式" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"

# §6.370 数量 / 年龄 / 比较 / 时间词族
echo "⑪ §6.370 数量·年龄·比较说清「没记数目/不会比较」；时间词族走「没有时钟」"
out=$(run "猫 有 几 条 腿" "猫 几 岁 了" "猫 比 狗 大 吗" "后天 星期几")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "只 记 了 猫 有 腿" \
    && chk "「猫有几条腿」⇒ 不知道数目，但说得出图里有「猫有腿」" 1 \
    || chk "数量（继承）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "没 记 过 年龄" \
    && chk "「猫几岁了」⇒ 没记过年龄" 1 \
    || chk "年龄" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "不 会 比 较" \
    && chk "「猫比狗大吗」⇒ 不会比较（不是「不太懂」）" 1 \
    || chk "比较" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "没有 时 钟" \
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
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 太 懂" \
    && chk "关掉 shape 桶 ⇒ 同一句退回兜底（证明是那几行数据在起作用）" 1 \
    || chk "shape 消融没退回" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

# §6.373 反问标记：剥掉标记后**仍然是问句**（不许当陈述学进去）
echo "⑬ §6.373 反问标记（难不成/莫不是/谁不知道/岂/该不会）：答对且不污染知识"
out=$(run "猫 属 于 动物 吗" "难不成 猫 会 飞" "猫 会 飞 吗" "猫 岂 是 动物" "猫 该 不会 飞 吧")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 对.*不 会" \
    && chk "「难不成猫会飞」⇒ 不对（以前会被当成陈述、触发改口）" 1 \
    || chk "难不成" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 对.*不 会" \
    && chk "紧接着问「猫会飞吗」仍是「不对」⇒ 反问没把知识改掉" 1 \
    || chk "知识被污染" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "对.*属 于\|对.*属于" \
    && chk "「猫岂是动物」⇒ 对（不再造出「猫岂」这个新词）" 1 \
    || chk "岂" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "对.*不 会" \
    && chk "「猫该不会飞吧」⇒ 对，猫不会飞" 1 \
    || chk "该不会" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"
out=$(AIP_NODATA=lead run "难不成 猫 会 飞" "猫 会 飞 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 太 懂" \
    && chk "关掉 lead 桶 ⇒ 同一句掉兜底（说明 lead 行承重）" 1 \
    || chk "lead 消融没变化" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 对.*不 会" \
    && chk "关掉 lead 桶时「猫会飞吗」仍不对 ⇒ 不被学进去是代码守卫，不是数据" 1 \
    || chk "安全网依赖数据" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

# §6.374 提问式引导语：剥掉「请问/麻烦/帮我看看…」之后仍然是问句
echo "⑭ §6.374 提问式引导语不许把问句当陈述学（最长匹配 + 数据标记）"
out=$(run "请问 猫 会 飞" "猫 会 飞 吗" "请问一下 猫 会 飞" "猫 会 飞")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 对.*不 会" \
    && chk "「请问猫会飞」⇒ 不对（以前答「改过来了：猫会飞」，把知识改反）" 1 \
    || chk "请问" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 对.*不 会" \
    && chk "紧接着问「猫会飞吗」仍不对 ⇒ 知识没被改" 1 \
    || chk "知识被改" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 对.*不 会" \
    && chk "「请问一下猫会飞」⇒ 不对（最长匹配：请问一下 先于 请问）" 1 \
    || chk "最长匹配" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -qE "记下 了|改 过来 了" \
    && chk "裸陈述「猫会飞」仍当教学（这是正确行为，别一起挡掉）" 1 \
    || chk "裸陈述被误挡" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
out=$(AIP_NODATA=lead run "请问 猫 会 飞" "猫 会 飞 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 太 懂" \
    && chk "关掉 lead 桶 ⇒ 同一句掉兜底（不学进去）" 1 \
    || chk "lead 消融" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 对.*不 会" \
    && chk "关掉 lead 桶时「猫会飞吗」仍不对 ⇒ 安全网不依赖数据" 1 \
    || chk "安全网依赖数据" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

# §6.375 副词填充（真的/其实/确实/实际上/到底/究竟）：删掉再判，教学不受影响
echo "⑮ §6.375 副词填充不算语义：问句照答、陈述照学"
out=$(run "猫 真的 会 飞 吗" "猫 其实 会 飞 吗" "猫 确实 会 飞 吗" "猫 实际上 属于 什么" "猫 到底 会 飞 吗" "猫 真的 会 飞")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 对.*不 会" \
    && chk "「猫真的会飞吗」⇒ 不对（真的 被当填充删掉）" 1 \
    || chk "真的" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 对.*不 会" \
    && chk "「猫其实会飞吗」⇒ 不对" 1 \
    || chk "其实" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 对.*不 会" \
    && chk "「猫确实会飞吗」⇒ 不对（分词是 确/实 两个词，也认得）" 1 \
    || chk "确实" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "猫 属于 动物" \
    && chk "「猫实际上属于什么」⇒ 猫属于动物（三个词组成一个填充，也认得）" 1 \
    || chk "实际上" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "不 对.*不 会" \
    && chk "「猫到底会飞吗」⇒ 不对" 1 \
    || chk "到底" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"
printf '%s\n' "$out" | reps | sed -n 6p | grep -qE "记下 了|改 过来 了|我 知道" \
    && chk "带填充的**陈述**仍当教学（猫真的会飞 ⇒ 学到）" 1 \
    || chk "陈述被误清" 0 "$(printf '%s\n' "$out" | reps | sed -n 6p)"
out=$(AIP_NODATA=fill run "猫 真的 会 飞 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 对" \
    && chk "关掉 fill 桶 ⇒ 同一句不再正常作答（说明是那 6 行数据在起作用）" 0 \
    || chk "关掉 fill 桶就退回去（数据承重）" 1

# §6.376 第二组副词填充（根本/简直/竟然/居然/毕竟/想必/显然）；语气副词（可能）故意不收
echo "⑯ §6.376 第二组填充：真假照判；「可能」这类猜测语气不删"
out=$(run "猫 根本 会 飞 吗" "猫 简直 会 飞 吗" "猫 竟然 会 飞 吗" "猫 居然 属于 动物 吗" "猫 毕竟 属于 动物 吗" "猫 显然 属于 动物 吗" "猫 想必 会 飞 吗" "猫 根本 会 飞" "猫 可能 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 对.*不 会" \
    && chk "「猫根本会飞吗」⇒ 不对（根本 是填充）" 1 \
    || chk "根本" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 对.*不 会" \
    && chk "「猫简直会飞吗」⇒ 不对" 1 \
    || chk "简直" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "对.*属 于\|对.*属于" \
    && chk "「猫居然属于动物吗」⇒ 对" 1 \
    || chk "居然" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "对.*属 于\|对.*属于" \
    && chk "「猫毕竟属于动物吗」⇒ 对" 1 \
    || chk "毕竟" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"
printf '%s\n' "$out" | reps | sed -n 7p | grep -q "不 对.*不 会" \
    && chk "「猫想必会飞吗」⇒ 不对" 1 \
    || chk "想必" 0 "$(printf '%s\n' "$out" | reps | sed -n 7p)"
printf '%s\n' "$out" | reps | sed -n 8p | grep -qE "记下 了|改 过来 了|我 知道" \
    && chk "带填充的陈述仍当教学（猫根本会飞）" 1 \
    || chk "陈述被误清" 0 "$(printf '%s\n' "$out" | reps | sed -n 8p)"
# §6.377 之后：模态词**不删**，而是如实说"不会算概率"（删了会把"可能"答成"一定"）
has "$(printf '%s\n' "$out" | reps | sed -n 9p)" "不 会 算" \
    && chk "「猫可能会飞吗」⇒ 如实说不会算概率（不删词、也不当确定事实答）" 1 \
    || chk "模态答复" 0 "$(printf '%s\n' "$out" | reps | sed -n 9p)"

# §6.377 模态词（可能/也许/大概）：不删、不学，如实说"不会算概率"
echo "⑰ §6.377 模态词：不删词、不当事实学、如实说不会算概率"
out=$(run "猫 可能 会 飞 吗" "猫 可能 会 飞" "猫 会 飞 吗" "明天 可能 下雨 吗" "猫 一定 会 飞 吗" "猫 应该 属于 动物 吧")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 会 算" \
    && chk "「猫可能会飞吗」⇒ 不会算概率（不是「不对」，也不是兜底）" 1 \
    || chk "模态问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 对" \
    && chk "模态问句**不许**当成确定事实答「不对」" 0 \
    || chk "没把「可能」答成「一定」" 1
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "记 不 了" \
    && chk "「猫可能会飞」（陈述）⇒ 明说记不了（不再造「猫可能」这种词）" 1 \
    || chk "模态陈述" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 对.*不 会" \
    && chk "紧接着问「猫会飞吗」仍不对 ⇒ 模态陈述没污染知识" 1 \
    || chk "知识被污染" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "天气" \
    && chk "「明天可能下雨吗」仍走天气边界（模态行没抢走它）" 1 \
    || chk "天气被抢" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "不 对.*不 会" \
    && chk "「猫一定会飞吗」⇒ 不对（一定 是断言，可删）" 1 \
    || chk "一定" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"
printf '%s\n' "$out" | reps | sed -n 6p | grep -q "对.*属 于\|对.*属于" \
    && chk "「猫应该属于动物吧」⇒ 对（应该 可删）" 1 \
    || chk "应该" 0 "$(printf '%s\n' "$out" | reps | sed -n 6p)"
# 数据承重：关掉 reply 桶，模态答复退回兜底（这两行是 [reply]，不是代码）
out=$(AIP_NODATA=reply run "猫 可能 会 飞 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 会 算" \
    && chk "关掉 reply 桶 ⇒ 模态答复消失（说明是数据行在兜）" 0 \
    || chk "关掉 reply 桶就退回（数据承重）" 1

# §6.380 「你能,吗」太宽：把所有能力问题都接成通用清单，害得「你能背诗吗」拿不到准确的答复
echo "⑱ §6.380 能力问句收窄：具体能力要答「不敢说会」，只有"请我做件事"才给通用清单"
out=$(run "你 能 背 诗 吗" "你 能 说 英语 吗" "你 能 来 吗" "你 能 帮 我 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 敢 说 会" \
    && chk "「你能背诗吗」⇒ 不敢说会（不再被通用能力清单接走）" 1 \
    || chk "背诗" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "不 敢 说 会" \
    && chk "「你能说英语吗」⇒ 不敢说会" 1 \
    || chk "英语" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "看 是 什 么 事" \
    && chk "「你能来吗」⇒ 仍给通用清单（这行没被误删）" 1 \
    || chk "你能来吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

# §6.382 第三组态度副词（反正/终归/总归/好歹/偏偏）
echo "⑲ §6.382 第三组填充：态度副词删掉，真假照判"
out=$(run "猫 反 正 不 会 飞 吗" "猫 终 归 属于 动物 吗" "猫 偏 偏 会 飞 吗" "猫 反 正 属于 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对.*不 会" \
    && chk "「猫反正不会飞吗」⇒ 对" 1 \
    || chk "反正" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对.*属 于\|对.*属于" \
    && chk "「猫终归属于动物吗」⇒ 对" 1 \
    || chk "终归" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 对.*不 会" \
    && chk "「猫偏偏会飞吗」⇒ 不对" 1 \
    || chk "偏偏" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -qE "我 知道|记下 了|改 过来 了" \
    && chk "带态度副词的陈述仍当教学（不再造「猫反正」这种词）" 1 \
    || chk "陈述" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.383 「多少」= 「几」：改写成已有数量路径，别掉兜底
echo "⑳ §6.383 「多少」数量问句：改写成「几」再答"
out=$(run "猫 有 多少 条 腿" "猫 有 多少 个 朋友" "一 周 有 多少 天" "猫 有 几 条 腿")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "只 记 了 猫 有 腿" \
    && chk "「猫有多少条腿」⇒ 不知道数目，但说得出「猫有腿」" 1 \
    || chk "多少+量词" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "没 记 过 数 目" \
    && chk "「猫有多少个朋友」⇒ 没记过数目（几个 那一路）" 1 \
    || chk "多少+个" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "一周 有 几 天" \
    && chk "「一周有多少天」⇒ 两字主语也认（一/周 是被拆开的词）" 1 \
    || chk "两字主语" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "只 记 了 猫 有 腿" \
    && chk "「猫有几条腿」仍然正常（没被改写行弄坏）" 1 \
    || chk "几形式" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
# 承重：这几条是 shape 行，关掉 shape 桶就该退回兜底
out=$(AIP_NODATA=shape run "猫 有 多少 条 腿")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 太 懂" \
    && chk "关掉 shape 桶 ⇒「猫有多少条腿」退回兜底（数据承重）" 1 \
    || chk "shape 消融" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

# §6.384 「X 的 体重/年龄 是 多少」：的-字领属问法，别掉兜底
echo "㉑ §6.384 「的」字领属问法：说清不会量/没记年龄"
out=$(run "猫 的 体 重 是 多少" "猫 的 年 龄 是 多少" "猫 有 多 重")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 会 量" \
    && chk "「猫的体重是多少」⇒ 不会量具体数量（不是兜底）" 1 \
    || chk "体重" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "没 记 过 年" \
    && chk "「猫的年龄是多少」⇒ 没记过年龄" 1 \
    || chk "年龄" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "不 会 量" \
    && chk "「猫有多重」（老的量/尺度路径）仍然正常" 1 \
    || chk "多X 路径" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
# 承重：这两条是 [reply] 行
out=$(AIP_NODATA=reply run "猫 的 体 重 是 多少")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 会 量" \
    && chk "关掉 reply 桶 ⇒ 同一句不再有这条答复（数据承重）" 0 \
    || chk "关掉 reply 桶就退回（数据承重）" 1

# §6.385 其余 的-字领属属性：身高/寿命/价格/颜色
echo "㉒ §6.385 其它领属属性：都说"不记"，不掉兜底"
out=$(run "猫 的 身 高 是 多少" "猫 的 寿 命 是 多少" "猫 的 颜 色 是 什么" "猫 一 般 多 少 钱")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 会 量" \
    && chk "「猫的身高是多少」⇒ 不会量" 1 \
    || chk "身高" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "不 知 道 寿 命" \
    && chk "「猫的寿命是多少」⇒ 不知道寿命" 1 \
    || chk "寿命" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "颜 色 我 不 知 道" \
    && chk "「猫的颜色是什么」⇒ 不知道颜色" 1 \
    || chk "颜色" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "价 格 我 不 知 道" \
    && chk "「猫一般多少钱」⇒ 不知道价格" 1 \
    || chk "价格" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
out=$(AIP_NODATA=reply run "猫 的 身 高 是 多少")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "不 会 量" \
    && chk "关掉 reply 桶 ⇒ 同一句不再有这条答复（数据承重）" 0 \
    || chk "关掉 reply 桶就退回（数据承重）" 1

# §6.386 `X 是谁`（不空格时 是谁 一个词）：不学垃圾、不说词串，说清"没记名字"
echo "㉓ §6.386 名字类问题：不学垃圾、不说词串"
out=$(run "猫 是 谁" "猫是谁" "老鼠是谁" "天是谁" "小明是谁")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "没 记 过 名 字" \
    && chk "「猫 是 谁」⇒ 没记过名字（带上已知类别）" 1 \
    || chk "spaced 身份" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "猫 属 于 动物\|猫 属于 动物" \
    && chk "「猫是谁」（不空格，是谁 并成一个词）⇒ 同样能答" 1 \
    || chk "unspaced 身份" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 5p)" "记下 了" \
    && chk "「小明是谁」**不许**被学成事实（以前答「记下 了 ： 小 明 是谁」）" 0 \
    || chk "不把名字问题学成事实" 1
# 注册过名字之后，两种写法都要能反查
out=$(run "我 的 名字 是 小明" "小明 是 谁" "小明是谁")
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "叫 小明 的 有" \
    && chk "注册名字后「小明 是 谁」⇒ 反查" 1 \
    || chk "spaced 反查" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "叫 小明 的 有" \
    && chk "注册名字后「小明是谁」（不空格）⇒ 也反查" 1 \
    || chk "unspaced 反查" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

# §6.391 情感/意愿/信用类：宾语换人称 + "没有判据"的如实答复
echo "㉔ §6.391 情感类问句：宾语人称要对，答复说清没有判据"
out=$(run "你 会 想 我 吗" "你 会 在 意 我 吗" "你 信 任 我 吗" "你 讨 厌 我 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 会 想你" \
    && chk "「你会想我吗」⇒ 图里没记过「我会想**你**」（不是「我想我」）" 1 \
    || chk "宾语人称" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 会 想我" \
    && chk "不再出现「我 会 想我」这种自指" 0 \
    || chk "没有自指宾语" 1
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "没 有 判 据" \
    && chk "「你信任我吗」⇒ 说清没有判据（不是兜底）" 1 \
    || chk "信任" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "没 有 判 据" \
    && chk "「你讨厌我吗」⇒ 说清没有判据" 1 \
    || chk "讨厌" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
out=$(AIP_NODATA=reply run "你 信 任 我 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "没 有 判 据" \
    && chk "关掉 reply 桶 ⇒ 这条答复消失（数据承重）" 0 \
    || chk "关掉 reply 桶就退回（数据承重）" 1

# §6.392 态度动词同义扩展；「你在,吗」收紧成整句精确（否则 `你在意我吗` 被接成"在——你说"）
echo "㉕ §6.392 态度动词同义 + 在不在的精确匹配"
out=$(run "你 嫌 我 吗" "你 信 我 吗" "你 在 意 我 吗" "你 在 吗" "你 在 么" "这 个 奇 怪 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "没 有 判 据" \
    && chk "「你嫌我吗」⇒ 没有判据（同义动词也收）" 1 \
    || chk "嫌" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "没 有 判 据" \
    && chk "「你信我吗」⇒ 没有判据" 1 \
    || chk "信" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "在 —— 你 说" \
    && chk "「你在意我吗」**不许**被"在不在"那行接走" 0 \
    || chk "「你在意我吗」不被在不在行抢走" 1
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "在 —— 你 说" \
    && chk "「你在吗」（整句精确）仍正常" 1 \
    || chk "你在吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
has "$(printf '%s\n' "$out" | reps | sed -n 5p)" "在 —— 你 说" \
    && chk "「你在么」仍正常" 1 \
    || chk "你在么" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"
has "$(printf '%s\n' "$out" | reps | sed -n 6p)" "没 有 判 据" \
    && chk "「这个奇怪吗」**不许**被态度行误伤（`怪` 只收 `怪我`）" 0 \
    || chk "没有误伤「奇怪」" 1

# §6.393 词类 `@attitude`：往类里加同义词，答复行不动也能命中
echo "㉖ §6.393 词类引用：加词不加行"
out=$(run "你 嫉 妒 我 吗" "你 记 恨 我 吗" "你 看 不 上 我 吗" "你 嫌 我 吗" "这 个 奇 怪 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "没 有 判 据" \
    && chk "「你嫉妒我吗」⇒ 命中（嫉妒 只在 [class] 里，答复行没改）" 1 \
    || chk "class 追加词" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "没 有 判 据" \
    && chk "「你记恨我吗」⇒ 命中" 1 \
    || chk "class 记恨" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "没 有 判 据" \
    && chk "「你看不上我吗」⇒ 命中（三字词也行）" 1 \
    || chk "class 看不上" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 5p)" "没 有 判 据" \
    && chk "「这个奇怪吗」仍**不**被误伤" 0 \
    || chk "没有误伤奇怪" 1
out=$(AIP_NODATA=class run "你 嫉 妒 我 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "没 有 判 据" \
    && chk "关掉 class 桶 ⇒ 这条答复消失（类表承重）" 0 \
    || chk "关掉 class 桶就退回（类表承重）" 1

# §6.396 裸问法（只有一组关键词）：气温多少度 / 礼拜几 / 哪一年
echo "㉗ §6.396 裸问法补行：只有一组关键词也能接住"
out=$(run "气 温 多 少 度" "温 度 是 多 少" "礼 拜 几" "哪 一 年")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "天气我不知道" \
    && chk "「气温多少度」⇒ 天气边界（以前要求"天气词+语境词"两组都有）" 1 \
    || chk "气温" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "天气我不知道" \
    && chk "「温度是多少」⇒ 天气边界" 1 \
    || chk "温度" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "没有时钟" \
    && chk "「礼拜几」（裸问）⇒ 没有时钟" 1 \
    || chk "礼拜几" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "没有时钟" \
    && chk "「哪一年」（裸问）⇒ 没有时钟" 1 \
    || chk "哪一年" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.397 用空格无关的 has() 写断言；同时补两个"从没被走过"的词类词：晴 / 这周
echo "㉘ §6.397 has() 断言 + 晴/这周 两个未覆盖词"
out=$(run "明天 会 晴 吗" "这周 会 下雨 吗" "这周 几号" "下雪 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "天气 我 不 知道" \
    && chk "「明天会晴吗」⇒ 天气边界（晴 在 @weather 里）" 1 \
    || chk "晴" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "天气 我 不 知道" \
    && chk "「这周会下雨吗」⇒ 天气边界（这周 在 @timeword 里）" 1 \
    || chk "这周" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "没有 时 钟" \
    && chk "「这周几号」⇒ 没有时钟" 1 \
    || chk "这周几号" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "天气 我 不 知道" \
    && chk "「下雪吗」⇒ 天气边界" 1 \
    || chk "下雪" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
# has() 自身：逐字空格也能匹配（这正是它存在的理由）
has "天 气 我 不 知 道" "天气我 不知道" \
    && chk "has() 对空格不敏感（逐字空格也不假红）" 1 \
    || chk "has() 空格不敏感" 0
# §6.398 has() 的**反向测试**：期望写错时必须返回"不匹配"（否则它就是恒真的摆设）
if has "天气我不知道" "这个期望根本不在答复里"; then
    chk "has() 对错误的期望返回了「匹配」⇒ 断言会永远绿" 0
else
    chk "has() 对错误期望会说「不匹配」（断言不会恒绿）" 1
fi

# §6.398 听懂类变体 + C 样本剩余兜底的边界
echo "㉙ §6.398 「听得懂」变体并入能力答复"
out=$(run "你 听 得 懂 吗" "你 听 懂 了 吗" "你 听 不 听 得 懂")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 能 答 图 里 记 着 的 关系" \
    && chk "「你听得懂吗」⇒ 能力答复（不再兜底）" 1 \
    || chk "听得懂" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "我 能 答 图 里 记 着 的 关系" \
    && chk "「你听不听得懂」⇒ 同一答复" 1 \
    || chk "听不听得懂" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

# §6.399 可信度问句（真话/实话）走同一套可核查的保证
echo "㉚ §6.399 「你说的是真话吗」"
out=$(run "你 说 的 是 真话 吗" "你 跟 我 说 实话 吧")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 说 的 都 是 图 里 记 着 的" \
    && chk "「你说的是真话吗」⇒ 可核查的保证（不是兜底）" 1 \
    || chk "真话" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "我 说 的 都 是 图 里 记 着 的" \
    && chk "「你跟我说实话吧」⇒ 同一答复" 1 \
    || chk "实话" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

# §6.402 早退路径统一出口：数据行/裸短回话的答复也要能被「再说一遍」复述
echo "㉛ §6.402 早退也要记住说了什么"
out=$(run "天气 怎么样" "再说一遍" "嗯" "再说一遍")
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "我 刚才 说 的 是" \
    && has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "天气 我 不 知道" \
    && chk "数据行答复（天气）之后「再说一遍」⇒ 复述天气答复" 1 \
    || chk "数据行后复述" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "我 刚才 说 的 是" \
    && chk "裸短回话（嗯）之后「再说一遍」⇒ 复述「嗯，好」" 1 \
    || chk "裸回话后复述" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.404 在场/可用性一族（在听吗/方便吗/醒了吗/睡了吗）
echo "㉜ §6.404 在场/可用性问句"
out=$(run "你 有 在 听 吗" "你 现在 方便 吗" "你 醒 了 吗" "你 还 没 醒 吗" "你 睡 了 吗")
for k in 1 2 3 4 5; do
    if has "$(printf '%s\n' "$out" | reps | sed -n "${k}p")" "我 一 直 在"; then
        chk "在场问句第 $k 条 ⇒ 我一直在（不睡觉/不用等方便）" 1
    else
        chk "在场问句第 $k 条" 0 "$(printf '%s\n' "$out" | reps | sed -n "${k}p")"
    fi
done

# §6.405 征求看法 / 问"你还想知道什么"
echo "㉝ §6.405 看法类与"下一步问什么""
out=$(run "你 同 意 吗" "你 认 为 呢" "你 支 持 我 吗" "你 还 想 知 道 什么")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 没有 看法" \
    && chk "「你同意吗」⇒ 没有看法（不编意见）" 1 \
    || chk "同意" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "我 没有 看法" \
    && chk "「你认为呢」⇒ 没有看法" 1 \
    || chk "认为" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "我 没有 看法" \
    && chk "「你支持我吗」（带宾语）⇒ 没有看法" 1 \
    || chk "支持我" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "就 三 件 事" \
    && chk "「你还想知道什么」⇒ 能力菜单（不是兜底）" 1 \
    || chk "还想知道什么" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.406 patuse 扫出的"没被测例走过"的行：抽查 4 条，确认它们是"没测过"而不是"坏行"
echo "㉞ §6.406 测例没走过的 reply 行（抽查）"
out=$(run "你 笨 不 笨" "电脑 坏 了 怎么办" "我们 做 朋友 吧" "你 写 一 首 诗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "我 不 聪明 也 不 笨" \
    && chk "「你笨不笨」⇒ 如实说（不是兜底）" 1 \
    || chk "笨不笨" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "不 会 给 步 骤" \
    && chk "「电脑坏了怎么办」⇒ 不会给步骤（明说边界）" 1 \
    || chk "怎么办" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "文 字 里 陪 你" \
    && chk "「我们做朋友吧」⇒ 只能在文字里陪（不越界）" 1 \
    || chk "做朋友" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "真 不 会" \
    && chk "「你写一首诗」⇒ 真不会（不编）" 1 \
    || chk "写诗" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.414 A 样本三族：地点问句 / 感知问句 / 自我需求
echo "㉟ §6.414 地点/感知/自我需求三族"
out=$(run "这 里 有 什么" "你 见 过 吗" "你 需 要 什么" "你 怕 什么")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "你 说 的 是 哪 儿" \
    && chk "「这里有什么」⇒ 反问是哪儿（不猜）" 1 \
    || chk "地点" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "没 有 眼 睛 和 耳 朵" \
    && chk "「你见过吗」⇒ 说清没有眼睛耳朵" 1 \
    || chk "感知" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "只 想 把 图 里 的 事 记 准" \
    && chk "「你需要什么」⇒ 只有一个目的" 1 \
    || chk "需求" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "只 想 把 图 里 的 事 记 准" \
    && chk "「你怕什么」⇒ 同一答复（不编恐惧）" 1 \
    || chk "害怕" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

# §6.415 指代无上文（这个为什么/是谁）、读心类、永远在吗
echo "㊱ §6.415 指代/读心/永远在"
out=$(run "这 个 为 什么" "这 个 是 谁" "你 猜 我 想 什么" "我 知 道 你 想 什么" "你 永 远 在 这 里 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "你 说 的 是 哪 个" \
    && chk "「这个为什么」⇒ 反问是哪个（不猜指代）" 1 \
    || chk "这个为什么" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "你 说 的 是 哪 个" \
    && chk "「这个是谁」⇒ 同一答复" 1 \
    || chk "这个是谁" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "不 会 读 心" \
    && chk "「你猜我想什么」⇒ 不会读心" 1 \
    || chk "猜我想什么" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "不 会 读 心" \
    && chk "「我知道你想什么」（陈述）⇒ 也不会读心" 1 \
    || chk "我知道你想什么" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
has "$(printf '%s\n' "$out" | reps | sed -n 5p)" "我 一 直 在" \
    && chk "「你永远在这里吗」⇒ 我一直在" 1 \
    || chk "永远在" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"

# §6.416 B 样本三族：自我意图 / 理解类 / "帮得了吗"
echo "㊲ §6.416 自我意图/理解/帮得了吗"
out=$(run "你 想 做 什么" "你 希 望 什么" "你 理 解 吗" "你 帮 得 了 吗")
has "$(printf '%s\n' "$out" | reps | sed -n 1p)" "只 想 把 图 里 的 事 记 准" \
    && chk "「你想做什么」⇒ 只有一个目的（和需要什么同族）" 1 \
    || chk "想做" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
has "$(printf '%s\n' "$out" | reps | sed -n 2p)" "只 想 把 图 里 的 事 记 准" \
    && chk "「你希望什么」⇒ 同一答复" 1 \
    || chk "希望" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
has "$(printf '%s\n' "$out" | reps | sed -n 3p)" "能 答 图 里 记 着 的 关 系" \
    && chk "「你理解吗」⇒ 能力答复（和听懂了吗同族）" 1 \
    || chk "理解" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
has "$(printf '%s\n' "$out" | reps | sed -n 4p)" "只 能 在 文 字 里 帮" \
    && chk "「你帮得了吗」⇒ 只能说清做不到" 1 \
    || chk "帮得了" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS relations（L2：静态知识/传递推理/对话中现学/持久化/不误学/合写否定/情态说法/动态否定/触发接求解）" \
                  || { echo "FAIL relations"; exit 1; }
