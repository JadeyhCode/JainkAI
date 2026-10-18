#!/bin/sh
# tests/askback.sh — 「追问回话」askback 的回归（§6.168）
#
# 看护的互动：它对一个闭合问句**自己承认推不出来**（关系层 unknown 那一支），
# 下一轮用户只回「会 / 对 / 不 会 / 不 对」⇒ 当教学写进图。
# 用户原话是「主要的知识获取途径还要是互动」—— 以前这条回话会掉进流水线，
# 答出「会 怕 得 快 我 答 上」这种词串，互动就断在那儿。
#
# §6.202 起**默认开**（`AIP_ASKBACK=0` 或 `::askback off` 退回）。关着时必须与原来一样
# （不教、不改答案）—— 下面 ① 就是显式用 `AIP_ASKBACK=0` 钉这件事的。
#
# 场景（每个都在**新进程**里跑，教过的边不互相污染）：
#   ① 关：回「会」不教 —— 再问还是推不出来
#   ② 开：回「会」⇒「好，记下 了」；再问 ⇒ 对
#   ③ 开：回「不 会」⇒ 记成否定；再问 ⇒ 不对
#   ④ 开：问句本身是否定（`不 会 … 吗`）回「对」⇒ 记成否定（极性**相乘**）
#   ⑤ 开：回一句别的话 ⇒ 追问作废（只等一轮），再回「会」也不会教
#   ⑥ 开：本来答得出来的问句**不**留追问（回「会」不会冒出一条「记下 了」）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_askback_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# run <AIP_ASKBACK 0|1> <输入行...>：独立进程，隔离每条场景
run() {
    ab=$1; shift
    { for q in "$@"; do printf '%s\n' "$q"; done; printf '::quit\n'; } \
        | env AIP_ASKBACK="$ab" "$BIN" --model "$MODEL" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }            # 只留 AI 的答复
ans()  { reps | head -1; }

# 触发句：桌子/飞 都在出厂词表里、但知识库里没有「桌子 会 飞」⇒ 关系层会说推不出来
Q="桌子 会 飞 吗"

echo "① 关：回「会」不教（再问还是推不出来）"
out=$(run 0 "$Q" "会" "$Q")
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
case "$a" in
    *"对 ，"*) chk "关着时没被教（第 3 句不该是「对」）" 0 "$a" ;;
    *)         chk "关着时没被教" 1 ;;
esac

echo "② 开：回「会」⇒ 记下 了；再问 ⇒ 对"
out=$(run 1 "$Q" "会" "$Q")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "记下 了 ： 桌子 会 飞" \
    && chk "回「会」⇒「记下 了 ： 桌子 会 飞」" 1 \
    || chk "回「会」⇒「记下 了 ： 桌子 会 飞」" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "对" \
    && chk "再问 ⇒ 对" 1 \
    || chk "再问 ⇒ 对" 0 "$a"

echo "③ 开：回「不 会」⇒ 记成否定；再问 ⇒ 不对"
out=$(run 1 "$Q" "不 会" "$Q")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "记下 了 ： 桌子 不 会 飞" \
    && chk "回「不 会」⇒ 记成 不 会" 1 \
    || chk "回「不 会」⇒ 记成 不 会" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "不 对" \
    && chk "再问 ⇒ 不 对" 1 \
    || chk "再问 ⇒ 不 对" 0 "$a"

echo "④ 开：问句本身否定（桌子 不 会 飞 吗）回「对」⇒ 记成否定"
out=$(run 1 "桌子 不 会 飞 吗" "对" "桌子 不 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "记下 了 ： 桌子 不 会 飞" \
    && chk "否定问句 + 对 ⇒ 否定事实" 1 \
    || chk "否定问句 + 对 ⇒ 否定事实" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "对 ，" \
    && chk "再问否定问句 ⇒ 对" 1 \
    || chk "再问否定问句 ⇒ 对" 0 "$a"

echo "⑤ 开：回一句别的话 ⇒ 追问作废；再回「会」也不教"
out=$(run 1 "$Q" "今天 天气 很 好" "会" "$Q")
printf '%s\n' "$out" | reps | grep -q "记下 了 ： 桌子" \
    && chk "追问只等一轮（说过别的话就作废）" 0 "$(printf '%s\n' "$out" | reps | grep "记下 了")" \
    || chk "追问只等一轮（说过别的话就作废）" 1
a=$(printf '%s\n' "$out" | reps | sed -n 4p)
case "$a" in
    *"对 ，"*) chk "作废后没被教" 0 "$a" ;;
    *)         chk "作废后没被教" 1 ;;
esac

echo "⑥ 开：本来就答得出来的问句不留追问"
out=$(run 1 "猫 属于 动物 吗" "会" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | grep -q "记下 了" \
    && chk "答得出来的问句不留追问" 0 "$(printf '%s\n' "$out" | reps | grep "记下 了")" \
    || chk "答得出来的问句不留追问" 1

echo "⑦ 猜过的再确认 ⇒ 猜测被**升格**成知识（§6.170：答复里不再带「·猜」）"
out=$(run 1 "狗 会 唱歌 吗" "对" "狗 会 唱歌 吗")
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "对 ， 狗 -\[会\]-> 唱歌" \
    && chk "再问 ⇒「对 ， 狗 -[会]-> 唱歌」（不是 会·猜）" 1 \
    || chk "再问 ⇒ 猜测已升格成知识" 0 "$a"

echo "⑧ 关：答「对」之后说「不 对」不纠正（老行为：知识不动）"
out=$(run 0 "猫 属于 动物 吗" "不 对" "猫 属于 动物 吗")
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "^对 ，" \
    && chk "关着时纠正不生效（再问还是「对」）" 1 \
    || chk "关着时纠正不生效" 0 "$a"

echo "⑨ 开：答「对」之后说「不 对」⇒ 改口；再问 ⇒ 不对"
out=$(run 1 "猫 属于 动物 吗" "不 对" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "改 过来 了 ： 猫 不 属于 动物" \
    && chk "回「不 对」⇒「改 过来 了 ： 猫 不 属于 动物」" 1 \
    || chk "回「不 对」⇒ 改口" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "不 对 ， 猫 -\[不 属于\]-> 动物" \
    && chk "再问 ⇒「不 对 ， 猫 -[不 属于]-> 动物」" 1 \
    || chk "再问 ⇒ 改口生效" 0 "$a"

echo "⑩ 开：答「对」之后说「对」⇒ 只确认，不改图"
out=$(run 1 "猫 属于 动物 吗" "对" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "那 我 就 这 么 记 着" \
    && chk "回「对」⇒ 只确认" 1 \
    || chk "回「对」⇒ 只确认" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "^对 ，" \
    && chk "确认之后答案没被改" 1 \
    || chk "确认之后答案没被改" 0 "$a"

echo "⑪ 开：中间插一句别的话 ⇒ 纠正作废（只等一轮）"
out=$(run 1 "猫 属于 动物 吗" "今天 天气 很 好" "不 对" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | grep -q "改 过来 了" \
    && chk "追问只等一轮（插话后不纠正）" 0 "$(printf '%s\n' "$out" | reps | grep "改 过来 了")" \
    || chk "追问只等一轮（插话后不纠正）" 1
a=$(printf '%s\n' "$out" | reps | sed -n 4p)
printf '%s\n' "$a" | grep -q "^对 ，" \
    && chk "插话之后知识没被改" 1 \
    || chk "插话之后知识没被改" 0 "$a"

echo "⑫ 开：我答「不对」、用户回「对」⇒ 确认**否定**那条（极性不能弄反）"
out=$(run 1 "猫 属于 植物 吗" "对" "猫 属于 植物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "猫 不 属于 植物" \
    && chk "确认的是「猫 不 属于 植物」" 1 \
    || chk "确认的是否定那条" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "不 属于" \
    && chk "再问仍是否定" 1 \
    || chk "再问仍是否定" 0 "$a"

echo "⑬ 开：我答「不对」、用户回「不 对」⇒ 翻成**肯定**"
out=$(run 1 "猫 属于 植物 吗" "不 对" "猫 属于 植物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "改 过来 了 ： 猫 属于 植物" \
    && chk "回「不 对」⇒ 翻成肯定" 1 \
    || chk "回「不 对」⇒ 翻成肯定" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "^对 ，" \
    && chk "再问 ⇒「对」" 1 \
    || chk "再问 ⇒「对」" 0 "$a"

echo "⑭ 开：整句说法也认（你 说 的 不 对），但不替我认领别人的错（我 说 的 不 对）"
out=$(run 1 "猫 属于 动物 吗" "你 说 的 不 对" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "改 过来 了 ： 猫 不 属于 动物" \
    && chk "「你 说 的 不 对」⇒ 改口" 1 \
    || chk "「你 说 的 不 对」⇒ 改口" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run 1 "猫 属于 动物 吗" "我 说 的 不 对" "猫 属于 动物 吗")
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "^对 ，" \
    && chk "「我 说 的 不 对」不改我的图" 1 \
    || chk "「我 说 的 不 对」不改我的图" 0 "$a"

echo "⑮ §6.172d：改口留「修正标记」节点（当场落地，关着没有）"
out=$(run 1 "猫 属于 动物 吗" "不 对" "::edges [修正:猫_不_属于_动物]")
printf '%s\n' "$out" | grep -q "修正:猫_不_属于_动物\]\] 有 1 条边" \
    && chk "开着时图里有 [修正:猫_不_属于_动物]" 1 \
    || chk "开着时图里有修正标记" 0 "$(printf '%s\n' "$out" | grep "修正" | tail -1)"
out=$(run 0 "猫 属于 动物 吗" "不 对" "::edges [修正:猫_不_属于_动物]")
printf '%s\n' "$out" | grep -q "没有这个符号节点" \
    && chk "关着时没有修正标记（老行为）" 1 \
    || chk "关着时没有修正标记" 0 "$(printf '%s\n' "$out" | grep "修正" | tail -1)"

echo "⑯ ::askback on 命令也能开（不用环境变量）"
out=$(run 0 "::askback on" "猫 属于 动物 吗" "不 对" "猫 属于 动物 吗")
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "不 对 ， 猫 -\[不 属于\]-> 动物" \
    && chk "::askback on 之后纠正生效" 1 \
    || chk "::askback on 之后纠正生效" 0 "$a"
printf '%s\n' "$out" | grep -q "追问回话 = 开" \
    && chk "命令回显状态" 1 \
    || chk "命令回显状态" 0

echo "⑰ 存盘：追问学到的 / 改口改掉的，重开进程还在"
run 1 "猫 属于 动物 吗" "不 对" "::save $TMP/a1.bin" > /dev/null
a=$(run 1 "::load $TMP/a1.bin" "猫 属于 动物 吗" | reps | head -1)
printf '%s\n' "$a" | grep -q "不 对 ， 猫 -\[不 属于\]-> 动物" \
    && chk "改口存盘后还认（猫 不 属于 动物）" 1 \
    || chk "改口存盘后还认" 0 "$a"
run 1 "狗 会 唱 歌 吗" "不 对" "::save $TMP/a2.bin" > /dev/null
a=$(run 1 "::load $TMP/a2.bin" "狗 会 唱 歌 吗" | reps | head -1)
printf '%s\n' "$a" | grep -q "不 对 ， 狗" \
    && chk "追问回话学到的（狗 不 会 唱 歌）存盘后还在" 1 \
    || chk "追问回话学到的存盘后还在" 0 "$a"

echo "⑱ ::askback clear 能手动丢掉挂着的追问"
out=$(run 1 "狗 会 唱 歌 吗" "::askback clear" "不 对" "狗 会 唱 歌 吗")
printf '%s\n' "$out" | reps | grep -q "记下 了" \
    && chk "丢掉之后那句「不 对」不再被当成回话" 0 "$(printf '%s\n' "$out" | reps | grep "记下 了")" \
    || chk "丢掉之后那句「不 对」不再被当成回话" 1
printf '%s\n' "$out" | grep -q "丢掉了挂着的追问" \
    && chk "clear 回显状态" 1 \
    || chk "clear 回显状态" 0

echo "⑲ §6.174 问句里含新词（恐龙 会 飞 吗）：回一句就造词 + 学"
out=$(run 1 "恐龙 会 飞 吗" "会" "恐龙 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "记下 了 ： 恐龙 会 飞" \
    && chk "回「会」⇒ 造词 + 记成肯定" 1 \
    || chk "回「会」⇒ 造词 + 记成肯定" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
a=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$a" | grep -q "对 ， 恐龙 -\[会\]-> 飞" \
    && chk "再问 ⇒ 对" 1 \
    || chk "再问 ⇒ 对" 0 "$a"
out=$(run 1 "恐龙 会 飞 吗" "不 会" "恐龙 会 飞 吗")
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "记下 了 ： 恐龙 不 会 飞" \
    && chk "回「不 会」⇒ 记成否定" 1 \
    || chk "回「不 会」⇒ 记成否定" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
out=$(run 0 "恐龙 会 飞 吗" "会" "恐龙 会 飞 吗")
printf '%s\n' "$out" | reps | grep -q "记下 了" \
    && chk "关着时不造词不学" 0 "$(printf '%s\n' "$out" | reps | grep "记下 了")" \
    || chk "关着时不造词不学" 1

echo "⑳ 纠正**继承**答案 ⇒ 写成具体例外（具体优先于一般）"
out=$(run 1 "猫 有 毛 吗" "不 对" "猫 有 毛 吗")
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "属于·继承" \
    && chk "先答的是**继承**来的（属于·继承）" 1 \
    || chk "先答的是继承来的" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
printf '%s\n' "$out" | reps | sed -n 2p | grep -q "改 过来 了 ： 猫 不 有 毛" \
    && chk "纠正写成具体事实（猫 不 有 毛）" 1 \
    || chk "纠正写成具体事实" 0 "$(printf '%s\n' "$out" | reps | sed -n 2p)"
r=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$r" | grep -q "不 对 ， 猫 -\[不 有\]-> 毛" \
    && chk "再问 ⇒ 具体事实压过继承" 1 \
    || chk "再问 ⇒ 具体事实压过继承" 0 "$r"

echo "㉑ §6.188 追问能跨存盘：挂起 → ::save → 新进程 ::load → 回一句照样教"
run 1 "狗 会 唱歌 吗" "::save $TMP/pend.bin" > /dev/null
out=$(run 1 "::load $TMP/pend.bin" "::askback" "不 对" "狗 会 唱歌 吗")
printf '%s\n' "$out" | grep -q "挂着一条追问：狗 会 唱歌" \
    && chk "重载后状态里能看到挂着的追问" 1 \
    || chk "重载后能看到挂着的追问" 0 "$(printf '%s\n' "$out" | grep -a "挂着")"
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了 ： 狗 不 会 唱歌" \
    && chk "重载后回「不 对」照样教" 1 \
    || chk "重载后回「不 对」照样教" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)"
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
printf '%s\n' "$r" | grep -q "不 对 ， 狗" \
    && chk "再问 ⇒ 不 对" 1 \
    || chk "再问 ⇒ 不 对" 0 "$r"
echo "㉒ 吃过的那条追问不该在下次载入时复活"
run 1 "狗 会 唱歌 吗" "不 对" "::save $TMP/pend2.bin" > /dev/null
out=$(run 1 "::load $TMP/pend2.bin" "::askback" "不 对" "狗 会 唱歌 吗")
printf '%s\n' "$out" | grep -q "没有挂着的追问" \
    && chk "吃掉后重载没有挂着的追问" 1 \
    || chk "吃掉后重载没有挂着的追问" 0 "$(printf '%s\n' "$out" | grep -a "挂着")"
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了" \
    && chk "不会再被那句「不 对」教一次" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "不会再被那句「不 对」教一次" 1

echo "㉓ 手动 clear 也要把标记清掉（否则存盘再载入会复活）"
run 1 "狗 会 唱歌 吗" "::askback clear" "::save $TMP/pend3.bin" > /dev/null
out=$(run 1 "::load $TMP/pend3.bin" "::askback" "不 对" "狗 会 唱歌 吗")
printf '%s\n' "$out" | grep -q "没有挂着的追问" \
    && chk "clear 之后重载没有挂着的追问" 1 \
    || chk "clear 之后重载没有挂着的追问" 0 "$(printf '%s\n' "$out" | grep -a "挂着")"
printf '%s\n' "$out" | reps | sed -n 1p | grep -q "记下 了" \
    && chk "clear 之后那句「不 对」不再教" 0 "$(printf '%s\n' "$out" | reps | sed -n 1p)" \
    || chk "clear 之后那句「不 对」不再教" 1

echo "㉔ §6.201 只有「我没把握」跨存盘；「我给了确定答案」那条不落标记（存盘就丢）"
run 1 "猫 属于 动物 吗" "::save $TMP/k2.bin" > /dev/null
out=$(run 1 "::load $TMP/k2.bin" "::askback" "对")
printf '%s\n' "$out" | grep -q "没有挂着的追问" \
    && chk "确定答案的 pending 不跨存盘（重载后没有）" 1 \
    || chk "确定答案的 pending 不跨存盘" 0 "$(printf '%s\n' "$out" | grep -a "挂着")"
printf '%s\n' "$out" | reps | grep -q "那 我 就 这 么 记 着" \
    && chk "重载后回「对」不触发确认" 0 "$(printf '%s\n' "$out" | reps | grep "记 着")" \
    || chk "重载后回「对」不触发确认" 1

if [ "$fail" -ne 0 ]; then
    echo "FAIL askback（追问回话）"
    exit 1
fi
echo "PASS askback（追问回话：承认推不出来 → 用户回一句 → 记进图）"
