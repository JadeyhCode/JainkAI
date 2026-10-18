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

if [ "$fail" -ne 0 ]; then
    echo "FAIL interact（互动通道端到端）"
    exit 1
fi
echo "PASS interact（互动通道端到端：造词/否定/纠正/继承例外/追问/新词问句/存盘重载）"
