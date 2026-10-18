#!/bin/sh
# tests/interact.sh — 「互动通道」的**端到端**验收（跨功能，一个进程从头聊到尾）
#
# 为什么单有一份：teach / askback / natcoin 各有自己的套件，但它们**串起来**用才是用户真做的事
# （先说一句 → 再问 → 说错了 → 纠正 → 存盘重开）。单套件绿不保证串起来绿：
#   · §6.172 纠正和 §6.170 猜测升格、§6.172g 继承例外、§6.173 造词，互相会踩；
#   · 存盘重开之后这些事实必须都还在。
# 默认两个开关都关，这里显式打开（`::askback on` / `::natcoin on`，不用环境变量）。
#
# 场景（**同一个进程**，顺序就是用户会说的话）：
#   ① 自然说法造新词：麒麟 属于 神兽 ⇒ 记下 了 ⇒ 问 ⇒ 对
#   ② 自然说法否定：企鹅 不 属于 鸟 ⇒ 问「属于 鸟 吗」⇒ 不 对
#   ③ 长新词：鲸鱼 属于 哺乳动物 ⇒ 问 ⇒ 对
#   ④ 我答错了、用户纠正：猫 属于 植物 吗 ⇒ 不 对 ⇒「不 对」⇒ 改 过来 了 ⇒ 问 ⇒ 对
#   ⑤ 继承答案被纠正成例外：猫 有 毛 吗 ⇒ 对（继承）⇒「不 对」⇒ 问 ⇒ 不 对
#   ⑥ 追问回话：狗 会 唱歌 吗（猜）⇒「不 对」⇒ 记下 了（狗 不 会 唱歌）⇒ 问 ⇒ 不 对
#   ⑦ 问句里含新词：恐龙 会 飞 吗 ⇒「会」⇒ 记下 了 ⇒ 问 ⇒ 对
#   ⑧ 接着聊别的不会把上面的知识弄坏（问一遍全部）
#   ⑨ 存盘 → 新进程 ::load ⇒ 全部还在
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_interact_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# 一个进程里跑完整个对话：run <输入行...>（固定带两个开关）
run() {
    { printf '::askback on\n::natcoin on\n'; for q in "$@"; do printf '%s\n' "$q"; done; printf '::quit\n'; } \
        | "$BIN" --model "$MODEL" 2>&1     # §6.248 修正：位置参数会被当成**语料**（实测 `./build_aip model.bin`
                                     # 走的是"预训练 1343 行"，不是加载模型）=> 必须用 --model
}
reps() { sed -n 's/.*AI > //p'; }          # 只留 AI 的答复
nth()  { reps | sed -n "$1p"; }            # 第几条答复（从 1 数）

echo "① 自然说法造新词：麒麟 属于 神兽"
out=$(run "麒麟 属于 神兽" "麒麟 属于 神兽 吗")
printf '%s\n' "$out" | nth 1 | grep -q "记下 了 ： 麒麟 属于 神兽" \
    && chk "记下 了（新词当场造）" 1 || chk "记下 了" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 2 | grep -q "对 ， 麒麟 -\[属于\]-> 神兽" \
    && chk "再问 ⇒ 对" 1 || chk "再问 ⇒ 对" 0 "$(printf '%s\n' "$out" | nth 1)"

echo "② 自然说法否定：企鹅 不 属于 鸟"
out=$(run "企鹅 不 属于 鸟" "企鹅 属于 鸟 吗")
printf '%s\n' "$out" | nth 1 | grep -q "记下 了 ： 企鹅 不 属于 鸟" \
    && chk "记成否定" 1 || chk "记成否定" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 2 | grep -q "不 对 ， 企鹅 -\[不 属于\]-> 鸟" \
    && chk "问肯定的 ⇒ 不 对" 1 || chk "问肯定的 ⇒ 不 对" 0 "$(printf '%s\n' "$out" | nth 1)"

echo "③ 长新词：鲸鱼 属于 哺乳动物"
out=$(run "鲸鱼 属于 哺乳动物" "鲸鱼 属于 哺乳动物 吗")
printf '%s\n' "$out" | nth 2 | grep -q "对 ， 鲸鱼 -\[属于\]-> 哺乳动物" \
    && chk "单字+整词混排能造且能问" 1 || chk "单字+整词混排" 0 "$(printf '%s\n' "$out" | nth 1)"

echo "④ 我答错了、用户纠正 ⇒ 改口"
out=$(run "猫 属于 植物 吗" "不 对" "猫 属于 植物 吗")
printf '%s\n' "$out" | nth 1 | grep -q "不 对" \
    && chk "先答的是「不 对」（独占规则）" 1 || chk "先答的是「不 对」" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 2 | grep -q "改 过来 了 ： 猫 属于 植物" \
    && chk "纠正 ⇒「改 过来 了 ： 猫 属于 植物」" 1 || chk "纠正 ⇒ 改口" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 3 | grep -q "对 ， 猫 -\[属于\]-> 植物" \
    && chk "再问 ⇒ 对" 1 || chk "再问 ⇒ 对" 0 "$(printf '%s\n' "$out" | nth 2)"

echo "⑤ 继承来的答案被纠正成**具体例外**"
out=$(run "猫 有 毛 吗" "不 对" "猫 有 毛 吗")
printf '%s\n' "$out" | nth 1 | grep -q "属于·继承" \
    && chk "先答是继承来的" 1 || chk "先答是继承来的" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 3 | grep -q "不 对 ， 猫 -\[不 有\]-> 毛" \
    && chk "例外写进去 ⇒ 再问 不 对" 1 || chk "例外写进去" 0 "$(printf '%s\n' "$out" | nth 2)"

echo "⑥ 追问回话：猜错了、回一句就改"
out=$(run "狗 会 唱歌 吗" "不 对" "狗 会 唱歌 吗")
printf '%s\n' "$out" | nth 2 | grep -q "记下 了 ： 狗 不 会 唱歌" \
    && chk "「不 对」⇒ 记成 狗 不 会 唱歌" 1 || chk "「不 对」⇒ 记成否定" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 3 | grep -q "不 对 ， 狗" \
    && chk "再问 ⇒ 不 对" 1 || chk "再问 ⇒ 不 对" 0 "$(printf '%s\n' "$out" | nth 2)"

echo "⑦ 问句里含新词：恐龙 会 飞 吗 ⇒ 回「会」就造词+学"
out=$(run "恐龙 会 飞 吗" "会" "恐龙 会 飞 吗")
printf '%s\n' "$out" | nth 2 | grep -q "记下 了 ： 恐龙 会 飞" \
    && chk "证据不够时挂追问，回「会」⇒ 记下 了" 1 || chk "回「会」⇒ 记下 了" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 3 | grep -q "对 ， 恐龙 -\[会\]-> 飞" \
    && chk "再问 ⇒ 对" 1 || chk "再问 ⇒ 对" 0 "$(printf '%s\n' "$out" | nth 2)"

echo "⑧ 同一段对话里，学到的东西互不干扰"
out=$(run "麒麟 属于 神兽" "企鹅 不 属于 鸟" "鲸鱼 属于 哺乳动物" "狗 会 唱歌 吗" "不 对" \
          "麒麟 属于 神兽 吗" "企鹅 属于 鸟 吗" "鲸鱼 属于 哺乳动物 吗" "狗 会 唱歌 吗")
printf '%s\n' "$out" | nth 6 | grep -q "对 ， 麒麟 -\[属于\]-> 神兽" \
    && chk "麒麟 还在" 1 || chk "麒麟 还在" 0 "$(printf '%s\n' "$out" | nth 5)"
printf '%s\n' "$out" | nth 7 | grep -q "不 对 ， 企鹅 -\[不 属于\]-> 鸟" \
    && chk "企鹅 的否定还在" 1 || chk "企鹅 的否定还在" 0 "$(printf '%s\n' "$out" | nth 6)"
printf '%s\n' "$out" | nth 8 | grep -q "对 ， 鲸鱼 -\[属于\]-> 哺乳动物" \
    && chk "鲸鱼 还在" 1 || chk "鲸鱼 还在" 0 "$(printf '%s\n' "$out" | nth 8)"
printf '%s\n' "$out" | nth 9 | grep -q "不 对 ， 狗" \
    && chk "狗 的否定还在" 1 || chk "狗 的否定还在" 0 "$(printf '%s\n' "$out" | nth 9)"

echo "⑨ 存盘 → 新进程 ::load ⇒ 全部还在"
run "麒麟 属于 神兽" "企鹅 不 属于 鸟" "鲸鱼 属于 哺乳动物" "狗 会 唱歌 吗" "不 对" \
    "猫 有 毛 吗" "不 对" "恐龙 会 飞 吗" "会" "::save $TMP/chat.bin" > /dev/null
out=$(run "::load $TMP/chat.bin" "麒麟 属于 神兽 吗" "企鹅 属于 鸟 吗" \
          "鲸鱼 属于 哺乳动物 吗" "狗 会 唱歌 吗" "猫 有 毛 吗" "恐龙 会 飞 吗")
printf '%s\n' "$out" | nth 1 | grep -q "对 ， 麒麟" \
    && chk "重载后 麒麟 属于 神兽 还在" 1 || chk "重载后 麒麟" 0 "$(printf '%s\n' "$out" | nth 1)"
printf '%s\n' "$out" | nth 2 | grep -q "不 对 ， 企鹅" \
    && chk "重载后 企鹅 不 属于 鸟 还在" 1 || chk "重载后 企鹅" 0 "$(printf '%s\n' "$out" | nth 2)"
printf '%s\n' "$out" | nth 3 | grep -q "对 ， 鲸鱼" \
    && chk "重载后 鲸鱼 属于 哺乳动物 还在" 1 || chk "重载后 鲸鱼" 0 "$(printf '%s\n' "$out" | nth 3)"
printf '%s\n' "$out" | nth 4 | grep -q "不 对 ， 狗" \
    && chk "重载后 狗 不 会 唱歌 还在" 1 || chk "重载后 狗" 0 "$(printf '%s\n' "$out" | nth 4)"
printf '%s\n' "$out" | nth 5 | grep -q "不 对 ， 猫 -\[不 有\]-> 毛" \
    && chk "重载后 继承例外 还在" 1 || chk "重载后 继承例外" 0 "$(printf '%s\n' "$out" | nth 5)"
printf '%s\n' "$out" | nth 6 | grep -q "对 ， 恐龙" \
    && chk "重载后 恐龙 会 飞 还在" 1 || chk "重载后 恐龙" 0 "$(printf '%s\n' "$out" | nth 6)"

echo "⑩ 载入失败要说清原因（文件不存在 / 格式不认），并且不影响原来的模型"
printf 'not-a-model' > "$TMP/bad.bin"
out=$(run "::load $TMP/nonexist.bin" "猫 属于 动物 吗" | grep -a "加载失败" || true)
printf '%s\n' "$out" | grep -q "文件不存在" \
    && chk "缺文件 ⇒「文件不存在」" 1 \
    || chk "缺文件 ⇒「文件不存在」" 0 "$out"
out=$(run "::load $TMP/bad.bin" "猫 属于 动物 吗")
printf '%s\n' "$out" | grep -a "加载失败" | grep -q "格式不认识" \
    && chk "坏文件 ⇒「格式不认识」" 1 \
    || chk "坏文件 ⇒「格式不认识」" 0 "$(printf '%s\n' "$out" | grep -a "加载失败")"
r=$(printf '%s\n' "$out" | nth 1)
printf '%s\n' "$r" | grep -q "对 ， 猫 -\[属于\]-> 动物" \
    && chk "载入失败后原来的模型照常答" 1 \
    || chk "载入失败后原来的模型照常答" 0 "$r"

echo "⑪ §6.205 名字介绍的自然说法（分词切没切开都要认）"
out=$(run "我 的 名字 是 小明" "你 知道 我 叫 什么 吗" "我 名字 是 小红" "我 的 名字 叫 小刚")
printf '%s\n' "$out" | grep -q "记下 了 ： 我 叫 小明" \
    && chk "「我 的 名字 是 小明」⇒ 学成 我 叫 小明" 1 \
    || chk "「我 的 名字 是 小明」" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | grep -q "我 记 的 是 ： 我 叫 小明" \
    && chk "接着问「我 叫 什么」⇒ 答得出来" 1 \
    || chk "接着问「我 叫 什么」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | grep -q "记下 了 ： 我 叫 小红" \
    && chk "「我 名字 是 小红」（名字被切成 名/字）也认" 1 \
    || chk "「我 名字 是 小红」" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | grep -q "记下 了 ： 我 叫 小刚" \
    && chk "「我 的 名字 叫 小刚」也认" 1 \
    || chk "「我 的 名字 叫 小刚」" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

echo "⑫ §6.212 名字类问法的形状归一（问得出、反查得到）"
out=$(run "我 的 名字 是 小明" "我 的 名字 是 什么" "你 知道 我 的 名字 吗" "谁 叫 小明" "小明 是 谁")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 记 的 是 ： 我 叫 小明" \
    && chk "「我 的 名字 是 什么」⇒ 答出名字" 1 \
    || chk "「我 的 名字 是 什么」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "我 记 的 是 ： 我 叫 小明" \
    && chk "「你 知道 我 的 名字 吗」⇒ 答出名字" 1 \
    || chk "「你 知道 我 的 名字 吗」" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "叫 小明 的 有 ： 我" \
    && chk "「小明 是 谁」⇒ 反查成「谁 叫 小明」" 1 \
    || chk "「小明 是 谁」" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"

echo "⑬ §6.215 「哪/哪里/哪儿」= 问地点（含 哪/里 被切开）"
out=$(run "猫 在 家里" "猫 在 哪" "猫 在 哪 里" "猫 在 哪 儿")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 记 的 是 ： 猫 在 家里" \
    && chk "「猫 在 哪」⇒ 答出地点" 1 \
    || chk "「猫 在 哪」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "我 记 的 是 ： 猫 在 家里" \
    && chk "「猫 在 哪 里」（切开）⇒ 答出地点" 1 \
    || chk "「猫 在 哪 里」" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

echo "⑭ §6.216 「再说一遍」：原样复述上一句（含排版），没说过就说没说过"
out=$(run "猫 属于 动物 吗" "再 说 一 遍")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 刚才 说 的 是 ： 对 ， 猫 -\[属于\]-> 动物" \
    && chk "「再 说 一 遍」⇒ 原样复述（含 -[属于]-> 排版）" 1 \
    || chk "「再 说 一 遍」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "再 说 一 遍")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 还 没 说 过 什么" \
    && chk "还没说过 ⇒ 明说「还没说过什么」" 1 \
    || chk "还没说过" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "⑮ §6.217 「刚才聊了什么 / 我刚才说了什么」"
out=$(run "我 喜欢 猫" "猫 属于 动物 吗" "我 刚才 说 了 什么" "我们 刚才 聊 了 什么")
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "你 刚才 说 的 是 ： 猫 属于 动物 吗" \
    && chk "「我 刚才 说 了 什么」⇒ 上一句用户原话（不是本句）" 1 \
    || chk "「我 刚才 说 了 什么」" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "我 喜欢 猫" \
    && chk "「我们 刚才 聊 了 什么」⇒ 列出最近几轮" 1 \
    || chk "「我们 刚才 聊 了 什么」" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

echo "⑯ §6.218 「A V 在 B」⇒ 学地点（动词那半句如实说没记）；新地点替掉旧地点"
if [ "${AIP_LOCATIVE:-1}" = "0" ]; then
    echo "  （AIP_LOCATIVE=0：反向测量，跳过这一节）"
else
out=$(run "我 住 在 北京" "我 在 哪")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我 在 北京" \
    && chk "「我 住 在 北京」⇒ 学成 我 在 北京（并说明住没记）" 1 \
    || chk "「我 住 在 北京」" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 记 的 是 ： 我 在 北京" \
    && chk "接着「我 在 哪」⇒ 答北京" 1 \
    || chk "接着「我 在 哪」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "我 住 在 北京" "我 住 在 学校" "我 在 哪")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "先前 记 的 我 在 北京 撤 掉 了" \
    && chk "换地点 ⇒ 旧地点撤掉，并说出来" 1 \
    || chk "换地点" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "我 记 的 是 ： 我 在 学校" \
    && chk "换完再问 ⇒ 只有学校" 1 \
    || chk "换完再问" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
fi

echo "⑰ §6.219 「A 在 B V」：地点边界靠"图里认不认得"（学校 认得 ⇒ 学校，不认得就不猜）"
if [ "${AIP_LOCATIVE:-1}" = "0" ]; then
    echo "  （AIP_LOCATIVE=0：反向测量，跳过这一节）"
else
out=$(run "我 住 在 北京" "我 在 学校 学习" "我 在 哪")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "记下 了 ： 我 在 学校" \
    && chk "「我 在 学校 学习」⇒ 学成 我 在 学校（学习那半句说明没记）" 1 \
    || chk "「我 在 学校 学习」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "我 记 的 是 ： 我 在 学校" \
    && chk "接着「我 在 哪」⇒ 学校" 1 \
    || chk "接着「我 在 哪」" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "我 在 学校 学习")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 在 学校学习" \
    && chk "地点边界不许贪（不能是「学校学习」）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "地点边界不许贪（不能是「学校学习」）" 1
fi

echo "⑱ §6.226 虚词不当谓语/宾语（不学垃圾）；「不懂」= 再说一遍"
out=$(run "我 忘 了" "举 个 例子" "说 中文" "我 不 明白")
# 只看**学习确认话**（`记下 了 ：`），别把帮助/开关提示里的「记下 了」也算进来
printf '%s\n' "$out" | grep -q "记下 了 ：" \
    && chk "虚词类说法不该学成关系（忘 了 / 个 / 中 文）" 0 "$(printf '%s\n' "$out" | grep -a "记下 了 ：" | head -1)" \
    || chk "虚词类说法不该学成关系（忘 了 / 个 / 中 文）" 1
out=$(run "猫 属于 动物 吗" "不懂")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "没 讲 清 吗 ？ 我 再 说 一 遍 ： 对 ， 猫 -\[属于\]-> 动物" \
    && chk "「不懂」⇒ 把上一句再说一遍" 1 \
    || chk "「不懂」⇒ 把上一句再说一遍" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "我 不 明白")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "那 我 换 个 说 法" \
    && chk "没说过什么时说「我 不 明白」⇒ 换个说法（不出词串）" 1 \
    || chk "「我 不 明白」（空历史）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "⑲ §6.227 地点/名字不能是虚词（不学垃圾）"
out=$(run "我 住 在 很" "我 在 哪")
printf '%s\n' "$out" | grep -q "记下 了 ：" \
    && chk "地点是虚词（很）⇒ 不学" 0 "$(printf '%s\n' "$out" | grep -a "记下 了 ：" | head -1)" \
    || chk "地点是虚词（很）⇒ 不学" 1
out=$(run "我 的 名字 是 很" "我 叫 什么")
printf '%s\n' "$out" | grep -q "记下 了 ：" \
    && chk "名字是虚词（很）⇒ 不学" 0 "$(printf '%s\n' "$out" | grep -a "记下 了 ：" | head -1)" \
    || chk "名字是虚词（很）⇒ 不学" 1
out=$(run "我 住 在 学校" "我 在 哪")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 记 的 是 ： 我 在 学校" \
    && chk "正常地点照旧学得进" 1 \
    || chk "正常地点照旧学得进" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "⑳ §6.229 主语不能是别的虚词（代词/指示词除外）"
out=$(run "很 属于 猫" "了 在 家里" "很 叫 小明")
printf '%s\n' "$out" | grep -q "记下 了 ：" \
    && chk "虚词当主语（很/了）⇒ 不学" 0 "$(printf '%s\n' "$out" | grep -a "记下 了 ：" | head -1)" \
    || chk "虚词当主语（很/了）⇒ 不学" 1
out=$(run "::rel open on" "这 属于 猫" "什么 属于 猫")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 这 属于 猫" \
    && chk "指示词当主语照旧可以（这 属于 猫）" 1 \
    || chk "指示词当主语" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "很 属于 猫" "::rel open on" "什么 属于 猫")
printf '%s\n' "$out" | grep -q "属于 猫 的 有 ： 很" \
    && chk "垃圾主语没有进图（什么 属于 猫 不该答 很）" 0 "$(printf '%s\n' "$out" | grep -a "属于 猫 的 有")" \
    || chk "垃圾主语没有进图" 1

echo "㉑ §6.234 量词问句归一 + 单字肯定别掉流水线"
out=$(run "猫 是 一 种 什么" "猫 是 一 只 什么")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 记 的 是 ： 猫 属于 动物" \
    && chk "「猫 是 一 种 什么」⇒ 按「属于 什么」答" 1 \
    || chk "「猫 是 一 种 什么」" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "能" "会" "可以")
n=$(printf '%s\n' "$out" | reps | grep -c "嗯 ， 好" || true)
[ "$n" -eq 3 ] && chk "单字肯定（能/会/可以）⇒「嗯，好」，不再出词串" 1 \
    || chk "单字肯定" 0 "$(printf '%s\n' "$out" | reps | head -2)"

echo "㉒ §6.239 并列主语「A 和 B 都 R C」：学两条、问两边"
out=$(run "猫 和 狗 都 是 动物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对 ， 猫 和 狗 都 是 动物" \
    && chk "两边都知道 ⇒ 对" 1 \
    || chk "两边都知道 ⇒ 对" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "猫 和 桌子 都 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 对 ： 猫 不 会 飞" \
    && chk "一边已知为假 ⇒ 不 对（并指出是哪边）" 1 \
    || chk "一边已知为假 ⇒ 不 对" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "考拉 和 熊猫 都 是 动物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 不 确定" \
    && chk "两边都不知道 ⇒ 明说「只知一边」" 1 \
    || chk "两边都不知道" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "猫 和 狗 都 是 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 猫 和 狗 都 是 动物" \
    && chk "陈述 ⇒ 拆成两条学" 1 \
    || chk "陈述 ⇒ 拆成两条学" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㉓ §6.240 并列宾语「A R B1 和 B2」（含顿号）"
out=$(run "我 喜欢 猫 和 狗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我 喜欢 猫 、 狗" \
    && chk "陈述 ⇒ 拆成两条学" 1 \
    || chk "陈述 ⇒ 拆成两条学" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "我 喜欢 猫 和 狗" "我 喜欢 猫 和 狗 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 我 都 喜欢 这 2 个" \
    && chk "问句 ⇒ 两边都知道就答「对」" 1 \
    || chk "问句 ⇒ 对" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "猫 吃 鱼 和 石头 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 不 确定" \
    && chk "一边不知道 ⇒ 明说「只知一边」" 1 \
    || chk "一边不知道" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "我 喜欢 猫 、 狗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我 喜欢 猫 、 狗" \
    && chk "顿号也算并列" 1 \
    || chk "顿号也算并列" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "我 喜欢 猫 、 狗 、 鱼" "我 喜欢 猫 、 狗 、 鱼 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "猫 、 狗 、 鱼" \
    && chk "三个并列项 ⇒ 拆三条学（新）" 1 \
    || chk "三个并列项" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "这 3 个" \
    && chk "三个并列项问 ⇒ 都对" 1 \
    || chk "三个并列项问" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "㉔ §6.242 复句：一句多条陈述"
out=$(run "猫 属于 动物，狗 属于 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 猫 属于 动物 ； 狗 属于 动物" \
    && chk "两个子句各学一条" 1 \
    || chk "两个子句各学一条" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "鸟 会 飞，鱼 会 游泳" "鸟 会 飞 吗" "鱼 会 游泳 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 鸟 -\[会\]-> 飞" \
    && chk "复句里学的第一条查得到" 1 \
    || chk "复句第一条" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "对 ， 鱼 -\[会\]-> 游泳" \
    && chk "复句里学的第二条查得到" 1 \
    || chk "复句第二条" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "猫 属于 动物，狗 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ：" \
    && chk "混了问句就整句回落（不半答）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "混了问句就整句回落（不半答）" 1

echo "㉕ §6.243 复句里的省略/指代（「狗 也 是」）"
out=$(run "猫 属于 动物，狗 也 是" "狗 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "狗 属于 动物" \
    && chk "省略句用上一句补全（并说明「接着上一句」）" 1 \
    || chk "省略句补全" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 狗 -\[属于\]-> 动物" \
    && chk "补全的那条真的学进去了" 1 \
    || chk "补全的那条真学进" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "鸟 会 飞，鱼 也 会" "鱼 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 鱼 -\[会\]-> 飞" \
    && chk "谓语省略（鱼 也 会）也能补" 1 \
    || chk "谓语省略" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "猫 属于 动物，狗 也 会")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 狗" \
    && chk "谓语对不上就不补（狗 也 会 ≠ 属于）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "谓语对不上就不补" 1

echo "㉖ §6.244 并列词归一（以及 / 还有）"
out=$(run "我 喜欢 猫 以及 狗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我 喜欢 猫 、 狗" \
    && chk "「以及」（分词切碎）也能当并列" 1 \
    || chk "以及" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "我 喜欢 猫 还有 狗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我 喜欢 猫 、 狗" \
    && chk "「还有」（两个 token）也能当并列" 1 \
    || chk "还有" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "猫 以及 狗 都 是 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 猫 和 狗 都 是 动物" \
    && chk "并列主语也认「以及」" 1 \
    || chk "并列主语 以及" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "猫 属于 猫以及狗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ：" \
    && chk "宾语里带「以及」不学成 blob" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "宾语里带「以及」不学成 blob" 1

echo "㉗ §6.245 选择不是并列：不学、问句分别查"
out=$(run "我 喜欢 猫 或 狗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ：" \
    && chk "陈述里带「或」⇒ 不学（选择不能当并列）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "陈述里带「或」⇒ 不学" 1
out=$(run "猫 属于 动物 或 植物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对 ： 猫 属于 动物" \
    && chk "选择问句「或」⇒ 分别查，答对并说清哪边" 1 \
    || chk "选择问句「或」" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "猫 属于 动物 还是 植物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对 ： 猫 属于 动物" \
    && chk "「还是」（不带吗）也算问句" 1 \
    || chk "「还是」问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "猫 属于 鱼 或 植物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 不 确定 你 指 哪 个" \
    && chk "两边都不确定 ⇒ 明说" 1 \
    || chk "两边都不确定" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㉘ §6.246 并列谓语（A R B1 也 R B2）"
out=$(run "猫 会 爬树 也 会 抓老鼠")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 猫 会 抓老鼠" \
    && chk "重复谓语也算并列（猫 会 爬树 也 会 抓老鼠）" 1 \
    || chk "并列谓语" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "我 喜欢 猫 也 喜欢 狗" "我 喜欢 狗 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 我 -\[喜欢\]-> 狗" \
    && chk "重复谓语学进来的第二条查得到" 1 \
    || chk "重复谓语第二条" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "我 喜欢 猫 也 喜欢 狗" "我 喜欢 猫 也 喜欢 狗 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 我 都 喜欢 这 2 个" \
    && chk "并列谓语的问句答得出（先教后问）" 1 \
    || chk "并列谓语问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "猫 会 爬树 也 会 抓老鼠")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "抓老鼠（爬树 我 还 不 认识）" \
    && chk "没认出来的项如实说（爬树 被切成 爬/树）" 1 \
    || chk "没认出来的项如实说" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㉙ §6.247 并列主语只报真的写进去的"
out=$(run "考拉 和 猫 都 是 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 猫 是 动物（考拉 我 还 不 认识）" \
    && chk "一边不在词池 ⇒ 只报另一边并说明" 1 \
    || chk "并列主语只报写进去的" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "考拉 和 猫 都 是 动物" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 猫 -\[属于\]-> 动物" \
    && chk "报的那条确实学进去了（写后能读回）" 1 \
    || chk "报的那条能读回" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "㉚ §6.248 写后读回校验（可选开关）"
out=$(run "::verifywrites on" "::natcoin on" "麒麟 属于 神兽" "::verifywrites")
last=$(printf '%s\n' "$out" | grep -a 写后读回 | tail -1)
if printf '%s\n' "$out" | grep -q "写后读回校验 = 开"; then chk "::verifywrites on 生效" 1; else chk "::verifywrites on" 0; fi
if printf '%s\n' "$last" | grep -q "不符 0 次"; then chk "校验过的写入全都能读回（不符 0）" 1; else chk "校验不符 0" 0 "$last"; fi
if printf '%s\n' "$last" | grep -q "校验过 0 次"; then chk "校验计数真的在动（不是没校验）" 0 "$last"; else chk "校验计数真的在动" 1; fi

echo "㉛ §6.254 「忘 掉 / 删 掉 X R Y」：如实说删不了，并给替代"
out=$(run "忘 掉 猫 属于 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "这 条 我 删 不 了" \
    && chk "要求删除 ⇒ 说清硬底线（不假装删了）" 1 \
    || chk "要求删除 ⇒ 说清硬底线" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "删 掉 猫 属于 动物" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 猫 -\[属于\]-> 动物" \
    && chk "说完也没真删（再问还是 对）" 1 \
    || chk "说完也没真删" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "忘 掉 猫 属于 石头")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 图 里 没 有" \
    && chk "本来没这条 ⇒ 说「不用删」" 1 \
    || chk "本来没这条" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "忘记 猫 属于 动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "这 条 我 删 不 了" \
    && chk "「忘记」也认" 1 \
    || chk "「忘记」也认" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㉜ §6.255 句首指代（它/它们/这个/那个）"
out=$(run "猫 属于 动物" "它 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "猫 -\[不 会\]-> 飞" \
    && chk "「它」指回上一轮主语（猫）" 1 \
    || chk "「它」指回上一轮主语" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "狗 属于 动物" "它 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "狗 -\[不 会\]-> 飞" \
    && chk "换主语后「它」跟着换（狗）" 1 \
    || chk "换主语后「它」跟着换" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "它 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "它" \
    && chk "没上文时「它」不瞎替换（原样）" 1 \
    || chk "没上文时「它」不瞎替换" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㉝ §6.256 天气/预报：没数据就明说，不编"
out=$(run "明天 会 下雨 吗")
r1=$(printf '%s\n' "$out" | reps | sed -n 1p)
printf '%s\n' "$r1" | grep -q "我 没有 天气 数据" \
    && chk "天气预报 ⇒ 明说没有数据" 1 \
    || chk "天气预报 ⇒ 明说没有数据" 0 "$r1"
printf '%s\n' "$r1" | grep -q "可能 会 下雨" \
    && chk "不编造（不说「明天可能会下雨」）" 0 "$r1" \
    || chk "不编造（不说「明天可能会下雨」）" 1
out=$(run "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对 ， 猫 -\[属于\]-> 动物" \
    && chk "非天气问题不受影响" 1 \
    || chk "非天气问题不受影响" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㉞ §6.257 能力边界（联网/算术）明说不会"
out=$(run "你 上 网 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 不 联 网" \
    && chk "问联网 ⇒ 明说不联网" 1 \
    || chk "问联网 ⇒ 明说不联网" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "一 加 一 等于 几")
r1=$(printf '%s\n' "$out" | reps | sed -n 1p)
printf '%s\n' "$r1" | grep -q "算 术 我 不 会" \
    && chk "问算术 ⇒ 明说不会（不给答案、不糊过去）" 1 \
    || chk "问算术 ⇒ 明说不会" 0 "$r1"
printf '%s\n' "$r1" | grep -qE "(^|[^0-9])2([^0-9]|$)" \
    && chk "算术不假装会（没有编出结果）" 0 "$r1" \
    || chk "算术不假装会" 1

echo "㉟ §6.258 可用性三补：自述身份统一 / 没有时钟 / 占位地名不学"
out=$(run "你 叫 什么 名字" "你 是 谁" "你叫什么")
n=$(printf '%s\n' "$out" | reps | grep -c "我 叫 JainkAI" || true)
[ "$n" -eq 3 ] && chk "三种问法给同一句自述（不再各说各的）" 1 \
    || chk "自述身份统一" 0 "$(printf '%s\n' "$out" | reps | head -3 | tr '\n' '|')"
out=$(run "今天几号" "现在 几 点")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 没有 时 钟" \
    && chk "问日期 ⇒ 明说没有时钟" 1 \
    || chk "问日期 ⇒ 明说没有时钟" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | grep -q "记下 了 ： 今天" \
    && chk "日期问题不该被当成知识记下" 0 "$(printf '%s\n' "$out" | grep -a "记下 了" | head -1)" \
    || chk "日期问题不该被当成知识记下" 1
out=$(run "我 住 在 一个 地方" "我 在 哪")
printf '%s\n' "$out" | grep -q "一个地方" && c=0 || c=1
[ "$c" -eq 1 ] && chk "占位地名（一个地方）不学" 1 || chk "占位地名不学" 0 "$(printf '%s\n' "$out" | grep -a 一个地方 | head -1)"

echo "㊱ §6.259 「你能做什么」给能力清单（不是闲聊模板）"
out=$(run "你能做什么" "你 会 什么" "你能干什么")
n=$(printf '%s\n' "$out" | reps | grep -c "就 三 件 事" || true)
[ "$n" -eq 3 ] && chk "三种问法都给同一份清单" 1 \
    || chk "能力清单三种问法一致" 0 "$(printf '%s\n' "$out" | reps | head -3 | tr '\n' '|')"
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 会 的 我 直说" \
    && chk "清单里同时说清做不到的（不联网/不会算术/不知道天气）" 1 \
    || chk "清单里说清做不到的" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㊲ §6.260 关于「我」的具体问题（语言/年龄/位置/出处/感情）"
out=$(run "你 会 什么 语言" "你 多 大 了" "你 住 在 哪" "谁 做 的 你" "你 有 感情 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "只 有 中文" \
    && chk "问语言 ⇒ 只说中文（且不再被打成\"能做什么\"）" 1 \
    || chk "问语言 ⇒ 只说中文" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 没 有 年龄" \
    && chk "问年龄 ⇒ 没有年龄（不是\"没有生日但可以有\"）" 1 \
    || chk "问年龄 ⇒ 没有年龄" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "没有 物理 位置" \
    && chk "问住处 ⇒ 没有物理位置" 1 \
    || chk "问住处 ⇒ 没有物理位置" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "单 头 文件" \
    && chk "问出处 ⇒ 说清是什么实现" 1 \
    || chk "问出处 ⇒ 说清实现" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
printf '%s\n' "$out" | reps | sed -n 5p | grep -q "我 没有 感情" \
    && chk "问感情 ⇒ 明确说没有（主语不再搞反）" 1 \
    || chk "问感情 ⇒ 明确说没有" 0 "$(printf '%s\n' "$out" | reps | sed -n 5p)"

echo "㊳ §6.261 自我/能力路由：八种问法各答各的（不被相邻判据抢走）"
out=$(run "你 会 什么 语言" "你 能 做 什么" "你 是 谁" "今天 几 号" "明天 会 下雨 吗" "你 上 网 吗" "一 加 一 等于 几" "你 多 大 了")
want="只 有 中文|就 三 件 事|JainkAI|没有 时 钟|天气 我 不 知道|我 不 联 网|算 术 我 不 会|我 没 有 年龄"
i=0
old_ifs="$IFS"; IFS='|'
for w in $want; do
    i=$((i+1))
    got=$(printf '%s\n' "$out" | reps | sed -n "${i}p")
    printf '%s\n' "$got" | grep -q "$w" \
        && chk "路由 $i ⇒ $w" 1 \
        || chk "路由 $i ⇒ $w" 0 "$got"
done
IFS="$old_ifs"

echo "㊴ §6.262/§6.263 不带空格的「在哪」+ 引导语「教你/告诉你」"
out=$(run "我 住 在 上海" "我在哪" "我住哪")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 记 的 是 ： 我 在 上海" \
    && chk "「我在哪」（无空格）答得出" 1 \
    || chk "「我在哪」（无空格）" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "我 记 的 是 ： 我 在 上海" \
    && chk "「我住哪」也答得出" 1 \
    || chk "「我住哪」" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "告诉你我喜欢猫" "我喜欢猫吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我 喜欢 猫" \
    && chk "引导语「告诉你」剥掉后照常学" 1 \
    || chk "引导语剥掉后照常学" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 我 -\[喜欢\]-> 猫" \
    && chk "学进来的那条查得到" 1 \
    || chk "学进来的那条查得到" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "㊵ §6.264 造词默认开（新词直接教得进）；`在` 不归造词"
out=$(run "教你考拉属于动物" "考拉属于动物吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 考拉 属于 动物（新词当场造）" \
    && chk "默认就能教新词（不用先开开关）" 1 \
    || chk "默认就能教新词" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "对 ， 考拉 -\[属于\]-> 动物" \
    && chk "教完就能问回来" 1 \
    || chk "教完就能问回来" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "我 在 想 事情")
printf '%s\n' "$out" | grep -q "记下 了 ： 我 在 想事情" \
    && chk "「我 在 想 事情」不该被造出「想事情」这种地点" 0 "$(printf '%s\n' "$out" | grep -a "记下 了" | head -1)" \
    || chk "「我 在 想 事情」不该被造出地点" 1
out=$(run "::natcoin off" "教你袋鼠会跳")
printf '%s\n' "$out" | grep -q "记下 了 ： 袋鼠" \
    && chk "::natcoin off 关得回去" 0 "$(printf '%s\n' "$out" | grep -a "记下 了" | head -1)" \
    || chk "::natcoin off 关得回去" 1

echo "㊶ §6.265 多字新主语（我表弟）：代词+名词的复合能学；代词+虚词仍挡"
out=$(run "我表弟是大学生" "我表弟属于什么")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 我表弟" \
    && chk "「我表弟是大学生」学得进（代词+名词复合）" 1 \
    || chk "「我表弟是大学生」学得进" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我表弟 属于 大学生" \
    && chk "学进来的复合主语查得到" 1 \
    || chk "复合主语查得到" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "猫 很 属于 动物")
printf '%s\n' "$out" | grep -q "记下 了 ： 猫很" \
    && chk "代词/名词之外的虚词复合仍挡（猫很）" 0 "$(printf '%s\n' "$out" | grep -a "记下 了" | head -1)" \
    || chk "虚词复合仍挡" 1

echo "㊷ §6.266/§6.267 为什么问句（白箱看家本事）+ 做不到的功能请求明说"
out=$(run "为什么猫属于动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "因为 猫 属于 动物" \
    && chk "「为什么猫属于动物」⇒ 给依据链" 1 \
    || chk "为什么…属于…" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "为什么猫属于植物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "其实 是 反 的：猫 不 属于 植物" \
    && chk "问反的 ⇒ 指出其实是反的（不再说成成立）" 1 \
    || chk "问反的 ⇒ 指出反的" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "为什么猫不会飞")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "因为 猫 不 会 飞" \
    && chk "否定问句 ⇒ 因为…不…" 1 \
    || chk "否定问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "讲个笑话" "说点别的")
n=$(printf '%s\n' "$out" | reps | grep -c "这 个 我 真 不 会" || true)
[ "$n" -eq 2 ] && chk "讲笑话/换话题 ⇒ 明说不会（不再出词串）" 1 \
    || chk "做不到的请求明说" 0 "$(printf '%s\n' "$out" | reps | head -2 | tr '\n' '|')"

echo "㊸ §6.268 省略问句「那狗呢 / 鸟呢」：沿用上一句的关系与宾语"
out=$(run "猫不会飞吗" "那狗呢")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "狗 -\[不 会\]-> 飞" \
    && chk "「那狗呢」沿用上一句（不 会 飞）" 1 \
    || chk "「那狗呢」沿用上一句" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "猫属于动物吗" "那鸟呢")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "鸟 -\[属于\]-> 动物" \
    && chk "换关系后也跟着换（那鸟呢 ⇒ 属于 动物）" 1 \
    || chk "换关系后跟着换" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "那狗呢")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对 ，" \
    && chk "没上文时「那狗呢」不瞎补全（不编出「对，…」）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "没上文时不瞎补全" 1

echo "㊹ §6.269 能力问句如实说 + 否定的「什么」问句 + 规则句明说学不进 + 反问我能不能保证"
out=$(run "你会算术吗" "帮我查一下北京天气" "你会写诗吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "算 术 我 不 会" \
    && chk "「你会算术吗」⇒ 明说不会（不再答\"你应该会算术\"）" 1 \
    || chk "算术能力问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "天气 我 不 知道" \
    && chk "「帮你查一下北京天气」⇒ 明说没有天气数据" 1 \
    || chk "天气请求式问法" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "这 个 我 真 不 会" \
    && chk "「你会写诗吗」⇒ 明说不会（不再答\"我会试着写诗\"）" 1 \
    || chk "写诗能力问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "我不喜欢猫" "我不喜欢什么" "谁不喜欢猫")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 记 的 是 ： 我 不 喜欢 猫" \
    && chk "否定的「什么」问句答得出（分词把「不喜欢」当一个词也认）" 1 \
    || chk "否定的什么问句" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 喜欢 猫 的 有 ： 我" \
    && chk "否定的反查「谁不喜欢猫」答得出" 1 \
    || chk "否定的反查" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "教你会飞的动物是鸟")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "一 条 规则" \
    && chk "规则句（…的…是…）明说学不进，不再出词串" 1 \
    || chk "规则句明说学不进" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "我在哪")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "你 还 没 告诉 我 你 在 哪" \
    && chk "还没教过地点时「我在哪」给怎么教（不再像复述问题）" 1 \
    || chk "我在哪给怎么教" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "你确定吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "每 条 都 能 追" \
    && chk "「你确定吗」⇒ 给可核查的保证（不再答\"我没什么事我有空\"）" 1 \
    || chk "你确定吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "㊺ §6.270 对话胶水 + 问句不当事实学 + 量/尺度问题"
out=$(run "不好意思" "你觉得呢" "你记得我吗" "我不明白")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "没 关系" \
    && chk "「不好意思」⇒ 没关系（不再出词串）" 1 \
    || chk "不好意思" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 没有 看法" \
    && chk "「你觉得呢」⇒ 明说没有看法（不再答补白）" 1 \
    || chk "你觉得呢" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 记 得 你" \
    && chk "「你记得我吗」⇒ 明说不记人（不编）" 1 \
    || chk "你记得我吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "换 个 说 法" \
    && chk "「我不明白」（无空格）⇒ 当没听懂处理" 1 \
    || chk "我不明白" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"
out=$(run "记住猫喜欢吃鱼" "我们聊到哪了")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 猫 喜欢" \
    && chk "「记住X」引导语剥掉（不再学成「记住猫」）" 1 \
    || chk "记住引导语" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "刚才 我们 聊 了" \
    && chk "「我们聊到哪了」⇒ 复述刚才几轮" 1 \
    || chk "我们聊到哪了" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "太阳有多大" "太阳有多大")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 会 量" \
    && chk "「太阳有多大」⇒ 明说记不了数量" 1 \
    || chk "量/尺度问题" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | grep -q "记下 了 ： 太阳" \
    && chk "问句不当事实学（太阳 有 多大 不记）" 0 "$(printf '%s\n' "$out" | grep -a "记下 了" | head -1)" \
    || chk "问句不当事实学" 1

echo "㊻ §6.271 整句检索的假阳性：问句不再被不相干的陈述句抢答 + 问候有应答"
out=$(run "猫喜欢吃什么" "地球是圆的吗" "你好吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "饿 的 时候" \
    && chk "「猫喜欢吃什么」不被「你 喜欢 吃 什么」的答案抢答" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "问句不被不相干陈述句抢答（猫喜欢吃什么）" 1
printf '%s\n' "$out" | reps | sed -n 2p | grep -qE "饿 的 时候|我 是 真 的 在 和 你 说 话|我 是 一 个 程序" \
    && chk "「地球是圆的吗」不吐闲聊句" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)" \
    || chk "问句不被闲聊句抢答（地球是圆的吗）" 1
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "挺 好" \
    && chk "「你好吗」有正常应答（不再拒答）" 1 \
    || chk "你好吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

echo "㊼ §6.272 生词被切成单字（上海/夸克）：问句说\"还不认识\"，教一句就接上"
out=$(run "上海在哪" "夸克属于什么")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "还 不 认识 「上海」" \
    && chk "「上海在哪」⇒ 明说还不认识（不再\"这个我不太懂\"）" 1 \
    || chk "上海在哪" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "还 不 认识 「夸克」" \
    && chk "「夸克属于什么」⇒ 明说还不认识" 1 \
    || chk "夸克属于什么" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "教你上海属于城市" "上海属于什么" "上海在哪")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 上海 属于 城市" \
    && chk "「教你上海属于城市」学得进（方位字上海不再被虚词守卫挡）" 1 \
    || chk "学上海" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "上海 属于 城市" \
    && chk "教完就能问回来" 1 \
    || chk "问回来" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "上海 在 什么 ， 图里 没 记 过" \
    && chk "多字主语接上「在」问句（问的是地点，不是\"不太懂\"）" 1 \
    || chk "多字主语+在" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

echo "㊽ §6.273 三/四字生词（石家庄/笔记本/电子产品）：不认识就说，教一句就接上"
out=$(run "石家庄在哪" "笔记本属于什么")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "还 不 认识 「石家庄」" \
    && chk "「石家庄在哪」（三字）⇒ 明说还不认识" 1 \
    || chk "石家庄在哪" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "还 不 认识 「笔记本」" \
    && chk "「笔记本属于什么」（三字）⇒ 明说还不认识" 1 \
    || chk "笔记本属于什么" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "笔记本属于电子产品" "笔记本属于什么" "石家庄属于城市" "石家庄属于什么")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 笔记本 属于 电子产品" \
    && chk "四字宾语（电子产品）学得进" 1 \
    || chk "四字宾语" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "笔记本 属于 电子产品" \
    && chk "教完问得回来（笔记本）" 1 \
    || chk "问回来（笔记本）" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "记下 了 ： 石家庄 属于 城市" \
    && chk "三字主语（石家庄）学得进" 1 \
    || chk "三字主语" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 4p | grep -q "石家庄 属于 城市" \
    && chk "教完问得回来（石家庄）" 1 \
    || chk "问回来（石家庄）" 0 "$(printf '%s\n' "$out" | reps | sed -n 4p)"

echo "㊾ §6.274 长句不许误学（整句上限放宽到 9 之后补的看护）"
out=$(run "猫喜欢鱼属于狗" "猫属于狗属于猫" "猫属于狗每天叫")
printf '%s\n' "$out" | grep -q "记下 了 ： 猫" \
    && chk "分句被当成宾语 ⇒ 不学（猫喜欢鱼属于狗 / 狗每天叫）" 0 "$(printf '%s\n' "$out" | grep -a "记下 了" | head -1)" \
    || chk "分句不当宾语学" 1
printf '%s\n' "$out" | grep -q "属于 狗属于猫" \
    && chk "宾语里再夹关系词 ⇒ 不学" 0 "$(printf '%s\n' "$out" | grep -a "属于 狗属于猫" | head -1)" \
    || chk "宾语夹关系词不学" 1
out=$(run "石家庄属于城市" "笔记本属于电子产品" "我表弟是大学生")
n=$(printf '%s\n' "$out" | reps | grep -c "记下 了" || true)
[ "$n" -eq 3 ] && chk "合法的复合/长句照旧学得进（没误伤）" 1 \
    || chk "合法复合没误伤" 0 "$(printf '%s\n' "$out" | reps | head -3 | tr '\n' '|')"

echo "㊿ §6.275 「你会 X 吗」：没记过就明说，教一句就认（不再\"我会试着 X\"）"
out=$(run "你会游泳吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 敢 说 会" \
    && chk "「你会游泳吗」⇒ 明说没记过（不再\"我会试着游泳\"）" 1 \
    || chk "你会游泳吗" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "试 着" \
    && chk "不再出现\"我会试着 X\"这种假装" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "不再假装会" 1
out=$(run "你会游泳" "你会游泳吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 你 会 游泳" \
    && chk "陈述句「你会游泳」照样能教（没被这条抢掉）" 1 \
    || chk "教你会游泳" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "你 -\[会\]-> 游泳" \
    && chk "教完问回来 ⇒ 对" 1 \
    || chk "问回来" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "51 §6.276 「为什么他以为…」：他人视角——归属先摆明，再说我图里记的是同一条还是反的"
out=$(run "他以为猫属于植物" "为什么他以为猫属于植物")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "归 他，不 进 我 的 知识" \
    && chk "「为什么他以为…」⇒ 先说清这是他的看法（不再\"这个我不太懂\"）" 1 \
    || chk "为什么他以为" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "我 图 里 记 的 是 反 的" \
    && chk "和我的知识相反 ⇒ 指出是反的" 1 \
    || chk "指出反的" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "为什么他以为猫属于动物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "没 记 过 他 说 过 这 条" \
    && chk "没听他说过 ⇒ 明说没记过（不编他的看法）" 1 \
    || chk "没记过" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "他以为猫属于动物" "为什么他以为猫属于动物")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "这 条 我 图 里 也 记 着" \
    && chk "他的看法和我一致 ⇒ 说明一致" 1 \
    || chk "一致" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "52 §6.277 他人视角的三种语序 + 两字名字 + 复数合并"
out=$(run "他以为猫属于植物" "他为什么以为猫属于植物")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "归 他，不 进 我 的 知识" \
    && chk "「他为什么以为…」（为什么在中间）也答得出" 1 \
    || chk "他为什么以为" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "小明以为猫属于植物" "为什么小明以为猫属于植物")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 小明 以为 猫 属于 植物" \
    && chk "两字名字的看法学得进（小明以为…）" 1 \
    || chk "小明以为" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "归 小明" \
    && chk "问小明也答得出（名字没被切成\"明\"）" 1 \
    || chk "问小明" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "小明以为猫属于植物" "他以为猫属于植物" "为什么他们都以为猫属于植物")
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "有 2 个 人" \
    && chk "复数合并：他们 => 数出两个人" 1 \
    || chk "复数合并" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "他说猫属于植物" "为什么他说猫属于植物")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "归 他，不 进 我 的 知识" \
    && chk "「为什么他说…」也按他的看法答" 1 \
    || chk "为什么他说" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "53 §6.278 句中指代（它）+ 复数的立场要分开数"
out=$(run "猫属于动物，它会飞吗" "猫属于动物。它会飞吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "猫 -\[不 会\]-> 飞" \
    && chk "「猫属于动物，它会飞吗」⇒ 它=猫，答得出（不再\"这个我不太懂\"）" 1 \
    || chk "句中指代（逗号）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "猫 -\[不 会\]-> 飞" \
    && chk "句号分隔也认" 1 \
    || chk "句中指代（句号）" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "他以为猫属于植物" "她以为猫不属于植物" "为什么他们都以为猫属于植物")
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "1 个 人 说 是" \
    && chk "复数里一正一反 ⇒ 分开数（不再\"2 个人都这么想\"）" 1 \
    || chk "复数立场分开数" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "不 是 一 边 倒" \
    && chk "并明说\"不是一边倒\"" 1 \
    || chk "不是一边倒" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"

echo "54 §6.279 主语并列/选择 2~4 项（或/、/和）"
out=$(run "猫或狗属于动物吗" "猫或狗或鸟属于动物吗" "猫或狗会飞吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "对 ， 猫 、 狗 都 属于 动物" \
    && chk "「猫或狗属于动物吗」（两项\"或\"）⇒ 对" 1 \
    || chk "两项或" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "猫 、 狗 、 鸟 都 属于 动物" \
    && chk "三项\"或\"也答得出" 1 \
    || chk "三项或" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
printf '%s\n' "$out" | reps | sed -n 3p | grep -q "都 不 会 飞" \
    && chk "全都不成立 ⇒ 都 不（不是逐项复读）" 1 \
    || chk "全都不成立" 0 "$(printf '%s\n' "$out" | reps | sed -n 3p)"
out=$(run "猫或狗或鸟会飞吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "鸟：对" \
    && chk "有对有不对 ⇒ 逐项分开报" 1 \
    || chk "逐项分开报" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"

echo "55 §6.280 多字并列项（洗衣机/冰箱）+ A-不-A（你会不会游泳）"
out=$(run "洗衣机或冰箱属于电器吗" "洗衣机、冰箱、电视都属于电器吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "洗衣机：图里 没 记 过" \
    && chk "多字并列项也逐项报（洗衣机/冰箱）" 1 \
    || chk "多字并列项" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "电视：图里 没 记 过" \
    && chk "三项多字并列（末尾\"都\"不算进名字）" 1 \
    || chk "三项多字并列" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "你会不会游泳")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "不 敢 说 会" \
    && chk "「你会不会游泳」⇒ 明说没记过（不再出闲聊）" 1 \
    || chk "你会不会游泳" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
out=$(run "你会游泳" "你会不会游泳")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "你 -\[会\]-> 游泳" \
    && chk "教过之后 A-不-A 照样答得出" 1 \
    || chk "教过之后" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"

echo "56 §6.281 宾语前置（猫你喜欢吗 / 游泳你会吗）"
out=$(run "猫你喜欢吗" "游泳你会吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "我 喜欢 猫" \
    && chk "「猫你喜欢吗」⇒ 还原成「你喜欢猫吗」答得出" 1 \
    || chk "宾语前置（喜欢）" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "不 敢 说 会" \
    && chk "「游泳你会吗」⇒ 能力问句也认得（不再\"我会试着游泳\"）" 1 \
    || chk "宾语前置（会）" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run "猫你属于什么")
printf '%s\n' "$out" | grep -q "猫你" \
    && chk "「猫你属于什么」不再被当成新词「猫你」" 0 "$(printf '%s\n' "$out" | grep -a 猫你 | head -1)" \
    || chk "不再把\"猫你\"当新词" 1

if [ "$fail" -ne 0 ]; then
    echo "FAIL interact（互动通道端到端）"
    exit 1
fi
echo "PASS interact（互动通道端到端：造词/否定/纠正/继承例外/追问/新词问句/存盘重载）"
