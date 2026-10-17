#!/bin/sh
# tests/natcoin.sh — 「自然说法里的新词」的回归（§6.173）
#
# 用户定的主线是**互动获取知识**。`::teach` 能教新词，但自然说一句
# `麒麟 属于 神兽` 以前整句掉降级答复（主语被分词器切成 麒/麟，形状判定不认）。
#
# 现在（默认**关**，`AIP_NATCOIN=1`）：
#   · 从第 2 个词往后找**第一个已知核心关系词**（不登记新谓语）；
#   · 它前面是主语、后面是宾语，两边各自拼成一个词；
#   · 每一侧最多 2 个词；多字整词只有"独占一侧"才认（单字才能拼）；
#   · 有疑问词 / 否定 / 虚词 ⇒ 不认（宁可少学，不可学歪）。
#
# 场景（每段都在**新进程**里跑）：
#   ① 关着：整句不学（老行为）
#   ② 开着：`麒麟 属于 神兽` ⇒ 记下 了；问 ⇒ 对
#   ③ 开着：`麒麟 是 神兽`（是 → 属于）；问 ⇒ 对
#   ④ 开着：`企鹅 会 游泳`（主语新词、宾语池里的词）；问 ⇒ 对
#   ⑤ 垃圾守卫：`我 在 学校 学习` 不许拼出 `学校学习`
#   ⑥ 垃圾守卫：`桌子 上 有 书` 不许拼出 `桌子上`
#   ⑦ 否定 + 新词：不认（登记在案的局限）
#   ⑧ 存盘重载：造出来的词和那条事实都还在
#   ⑨ 关着：新词不进池（`::edges` 说没有这个符号节点）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_natcoin_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# run <AIP_NATCOIN 0|1> <输入行...>：独立进程，隔离每段场景
run() {
    nc=$1; shift
    { for q in "$@"; do printf '%s\n' "$q"; done; printf '::quit\n'; } \
        | env AIP_NATCOIN="$nc" "$BIN" "$MODEL" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }
ans()  { reps | head -1; }

echo "① 关着：整句不学（老行为）"
out=$(run 0 "麒麟 属于 神兽" "麒麟 属于 神兽 吗")
r=$(printf '%s\n' "$out" | ans)
case "$r" in *"记下 了"*) chk "关着时不该学新词" 0 "$r" ;; *) chk "关着时整句不学（现答：$r）" 1 ;; esac
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
case "$r" in *"^对 ，"*) chk "关着时问也答不出" 0 "$r" ;; *) chk "关着时问也答不出（现答：$r）" 1 ;; esac

echo "② 开着：麒麟 属于 神兽 ⇒ 记下 了；问 ⇒ 对"
out=$(run 1 "麒麟 属于 神兽" "麒麟 属于 神兽 吗")
r=$(printf '%s\n' "$out" | ans)
printf '%s\n' "$r" | grep -q "记下 了 ： 麒麟 属于 神兽" \
    && chk "记下 了 ： 麒麟 属于 神兽（新词当场造）" 1 \
    || chk "记下 了 ： 麒麟 属于 神兽" 0 "$r"
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
printf '%s\n' "$r" | grep -q "对 ， 麒麟 -\[属于\]-> 神兽" \
    && chk "再问 ⇒ 对" 1 \
    || chk "再问 ⇒ 对" 0 "$r"

echo "③ 开着：麒麟 是 神兽（是 → 属于）"
out=$(run 1 "麒麟 是 神兽" "麒麟 属于 神兽 吗")
printf '%s\n' "$out" | ans | grep -q "记下 了 ： 麒麟 是 神兽" \
    && chk "「是」也认（关系落到 属于）" 1 \
    || chk "「是」也认" 0 "$(printf '%s\n' "$out" | ans)"
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
printf '%s\n' "$r" | grep -q "对 ， 麒麟 -\[属于\]-> 神兽" \
    && chk "问「属于」也对" 1 \
    || chk "问「属于」也对" 0 "$r"

echo "④ 开着：企鹅 会 游泳（主语新词、宾语池里的整词）"
out=$(run 1 "企鹅 会 游泳" "企鹅 会 游泳 吗")
printf '%s\n' "$out" | ans | grep -q "记下 了 ： 企鹅 会 游泳" \
    && chk "记下 了 ： 企鹅 会 游泳" 1 \
    || chk "记下 了 ： 企鹅 会 游泳" 0 "$(printf '%s\n' "$out" | ans)"
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
printf '%s\n' "$r" | grep -q "对 ， 企鹅 -\[会\]-> 游泳" \
    && chk "再问 ⇒ 对" 1 \
    || chk "再问 ⇒ 对" 0 "$r"

echo "⑤ 垃圾守卫：我 在 学校 学习 不许拼出 学校学习"
out=$(run 1 "我 在 学校 学习" "::rel 我")
printf '%s\n' "$out" | reps | grep -q "记下 了" \
    && chk "「我 在 学校 学习」不该学" 0 "$(printf '%s\n' "$out" | reps | head -1)" \
    || chk "「我 在 学校 学习」没被学" 1
printf '%s\n' "$out" | grep -q "学校学习" \
    && chk "图里没有「学校学习」" 0 \
    || chk "图里没有「学校学习」" 1

echo "⑥ 垃圾守卫：桌子 上 有 书 不许拼出 桌子上"
out=$(run 1 "桌子 上 有 书" "::edges 桌子")
printf '%s\n' "$out" | reps | grep -q "记下 了" \
    && chk "「桌子 上 有 书」不该学" 0 "$(printf '%s\n' "$out" | reps | head -1)" \
    || chk "「桌子 上 有 书」没被学" 1

echo "⑦ 否定 + 新词：「企鹅 不 属于 鸟」⇒ 记成否定"
out=$(run 1 "企鹅 不 属于 鸟" "企鹅 属于 鸟 吗" "企鹅 不 属于 鸟 吗")
printf '%s\n' "$out" | ans | grep -q "记下 了 ： 企鹅 不 属于 鸟" \
    && chk "否定陈述学成否定" 1 \
    || chk "否定陈述学成否定" 0 "$(printf '%s\n' "$out" | ans)"
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
printf '%s\n' "$r" | grep -q "不 对 ， 企鹅 -\[不 属于\]-> 鸟" \
    && chk "问肯定的 ⇒ 不 对" 1 \
    || chk "问肯定的 ⇒ 不 对" 0 "$r"
r=$(printf '%s\n' "$out" | reps | sed -n 3p)
printf '%s\n' "$r" | grep -q "对 ， 企鹅 -\[不 属于\]-> 鸟" \
    && chk "问否定的 ⇒ 对" 1 \
    || chk "问否定的 ⇒ 对" 0 "$r"

echo "⑧ 存盘重载：造出来的词和事实都还在"
run 1 "麒麟 属于 神兽" "::save $TMP/n1.bin" > /dev/null
out=$(run 1 "::load $TMP/n1.bin" "麒麟 属于 神兽 吗")
r=$(printf '%s\n' "$out" | reps | head -1)
printf '%s\n' "$r" | grep -q "对 ， 麒麟 -\[属于\]-> 神兽" \
    && chk "重载后仍认（新词随模型存盘）" 1 \
    || chk "重载后仍认" 0 "$r"

echo "⑨ 关着：新词不进池"
out=$(run 0 "麒麟 属于 神兽" "::edges 麒麟")
printf '%s\n' "$out" | grep -q "没有这个符号节点" \
    && chk "关着时 麒麟 不在图里" 1 \
    || chk "关着时 麒麟 不在图里" 0

echo "⑩ ::natcoin on 命令也能开（不用环境变量）"
out=$(run 0 "::natcoin on" "麒麟 属于 神兽" "麒麟 属于 神兽 吗")
printf '%s\n' "$out" | ans | grep -q "记下 了 ： 麒麟 属于 神兽" \
    && chk "::natcoin on 之后造词 + 学" 1 \
    || chk "::natcoin on 之后造词 + 学" 0 "$(printf '%s\n' "$out" | ans)"
printf '%s\n' "$out" | grep -q "自然说法里的新词 = 开" \
    && chk "命令回显状态" 1 \
    || chk "命令回显状态" 0

echo "⑪ 合写否定：「企鹅 不是 鸟」也认（否定已经在词里，措辞别再补一个「不」）"
out=$(run 1 "企鹅 不是 鸟" "企鹅 属于 鸟 吗")
printf '%s\n' "$out" | ans | grep -q "记下 了 ： 企鹅 不是 鸟" \
    && chk "记下 了 ： 企鹅 不是 鸟" 1 \
    || chk "记下 了 ： 企鹅 不是 鸟" 0 "$(printf '%s\n' "$out" | ans)"
r=$(printf '%s\n' "$out" | reps | sed -n 2p)
printf '%s\n' "$r" | grep -q "不 对 ， 企鹅 -\[不 属于\]-> 鸟" \
    && chk "存成否定（问肯定的 ⇒ 不 对）" 1 \
    || chk "存成否定" 0 "$r"

if [ "$fail" -ne 0 ]; then
    echo "FAIL natcoin（自然说法里的新词）"
    exit 1
fi
echo "PASS natcoin（自然说法里的新词：造词 + 学一条核心关系，垃圾形状不认）"
