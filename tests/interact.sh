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
        | "$BIN" "$MODEL" 2>&1
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

if [ "$fail" -ne 0 ]; then
    echo "FAIL interact（互动通道端到端）"
    exit 1
fi
echo "PASS interact（互动通道端到端：造词/否定/纠正/继承例外/追问/新词问句/存盘重载）"
