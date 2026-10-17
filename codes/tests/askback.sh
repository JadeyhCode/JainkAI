#!/bin/sh
# tests/askback.sh — 「追问回话」askback 的回归（§6.168）
#
# 看护的互动：它对一个闭合问句**自己承认推不出来**（关系层 unknown 那一支），
# 下一轮用户只回「会 / 对 / 不 会 / 不 对」⇒ 当教学写进图。
# 用户原话是「主要的知识获取途径还要是互动」—— 以前这条回话会掉进流水线，
# 答出「会 怕 得 快 我 答 上」这种词串，互动就断在那儿。
#
# 默认**关**（`AIP_ASKBACK=1` 才开）。关着时必须与原来一样（不教、不改答案）。
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
        | env AIP_ASKBACK="$ab" "$BIN" "$MODEL" 2>&1
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

if [ "$fail" -ne 0 ]; then
    echo "FAIL askback（追问回话）"
    exit 1
fi
echo "PASS askback（追问回话：承认推不出来 → 用户回一句 → 记进图）"
