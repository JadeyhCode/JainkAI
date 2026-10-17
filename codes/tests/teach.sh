#!/bin/sh
# tests/teach.sh — 「显式教学」::teach 的回归（§6.159）
#
# 看护这条**互动学习的入口**：`::teach <主语> <关系词> <宾语>`。
# 为什么要单独一条命令：实测直接说一句「麒麟 属于 神兽」**学不到** ——
# 新词被分词器切开（`神兽` → `神`+`兽`），关系层就成不了事实。
# `::teach` 会先**当场造词**（进池，分词器"池内最长匹配"从此按整词切），再写关系边。
#
# 场景（每个都在**新进程**里跑，教过的边不会互相污染）：
#   ① 全词都在词表里：教 → 问 ⇒ 对
#   ② **含新词**：教 `麒麟 属于 神兽` ⇒ 当场造词 2 个 → 问 ⇒ 对（这是本条的重点）
#   ③ 否定：`::teach 猫 不 属于 植物` ⇒ 问 ⇒ 不对
#   ④ 动态关系（关系词不认识）：`::teach 月亮 喵 鱼` ⇒ 学会，且不进核心关系表
#   ⑤ 存档重载：教 → ::save → 新进程 ::load ⇒ 还认（含新词）
#   ⑥ 用法错误：参数不够 ⇒ 给用法提示，**不许崩**
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_teach_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# run <输入行...>：每条都是独立进程（教过的边会进图，必须隔离）
run() {
    { for q in "$@"; do printf '%s\n' "$q"; done; printf '::quit\n'; } | "$BIN" 2>&1
}
ans() { sed -n 's/.*AI > //p' | head -1; }

echo "① 全词都在词表里：教 ⇒ 问得出来"
r=$(run "::char off" "::teach 电脑 属于 动物" "电脑 属于 动物 吗" | ans)
case "$r" in *"对"*) chk "电脑 属于 动物 吗 ⇒ 对" 1 ;; *) chk "电脑 属于 动物 吗 ⇒ 对" 0 "$r" ;; esac

echo "② 含**新词**：当场造词 ⇒ 问得出来（本条重点）"
out=$(run "::char off" "::teach 麒麟 属于 神兽" "麒麟 属于 神兽 吗")
printf '%s\n' "$out" | grep -aq "当场造词 2 个" \
    && chk "教新词时**当场造词 2 个**" 1 || chk "教新词时当场造词 2 个" 0
r=$(printf '%s\n' "$out" | ans)
case "$r" in *"对"*"麒麟"*) chk "麒麟 属于 神兽 吗 ⇒ 对（新词已能整词识别）" 1 ;;
                    *) chk "麒麟 属于 神兽 吗 ⇒ 对" 0 "$r" ;; esac

echo "③ 否定：::teach <A> 不 <R> <B>"
r=$(run "::char off" "::teach 猫 不 属于 植物" "猫 属于 植物 吗" | ans)
case "$r" in *"不 对"*) chk "猫 属于 植物 吗 ⇒ 不对" 1 ;; *) chk "猫 属于 植物 吗 ⇒ 不对" 0 "$r" ;; esac

echo "④ 动态关系（关系词不认识）：照样学会"
out=$(run "::char off" "::teach 月亮 喵 鱼" "::rel 月亮")
printf '%s\n' "$out" | grep -aq "教 会 了.*喵" \
    && chk "「::teach 月亮 喵 鱼」被接受（登记成动态关系）" 1 || chk "动态关系被接受" 0
printf '%s\n' "$out" | grep -aq "喵" && chk "::rel 月亮 里能看到这条边" 1 || chk "::rel 里能看到" 0

echo "④b 动态关系**能被问**（§6.162 修：以前"教得进、问不出"）"
out=$(run "::char off" "::teach 月亮 喵 鱼" "月亮 喵 鱼 吗")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | head -1)
case "$r" in *"对"*"喵"*) chk "月亮 喵 鱼 吗 ⇒ 对（动态关系可查）" 1 ;;
               *)         chk "月亮 喵 鱼 吗 ⇒ 对" 0 "$r" ;; esac
out=$(run "::char off" "::rel open on" "::teach 月亮 喵 鱼" "月亮 喵 什么" "谁 喵 鱼")
printf '%s\n' "$out" | grep -aq "我 记 的 是 ： 月亮 喵 鱼" \
    && chk "开放式 月亮 喵 什么 ⇒ 月亮 喵 鱼" 1 || chk "开放式 月亮 喵 什么" 0
printf '%s\n' "$out" | grep -aq "喵 鱼 的 有 ： 月亮" \
    && chk "反查 谁 喵 鱼 ⇒ 月亮" 1 || chk "反查 谁 喵 鱼" 0

echo "⑤ 存档重载：教过的（含新词）还在"
run "::char off" "::teach 麒麟 属于 神兽" "::save $TMP/t1.bin" > /dev/null
r=$(run "::load $TMP/t1.bin" "::char off" "麒麟 属于 神兽 吗" | ans)
case "$r" in *"对"*) chk "重载后 麒麟 属于 神兽 吗 ⇒ 对" 1 ;; *) chk "重载后仍认新词" 0 "$r" ;; esac

echo "⑥ 用法错误不崩"
out=$(run "::teach 只有两个词" "你好")
printf '%s\n' "$out" | grep -aq "\[用法\]" && chk "参数不够 ⇒ 给用法提示" 1 || chk "参数不够 ⇒ 给用法提示" 0
printf '%s\n' "$out" | grep -aq "AI > " && chk "而且进程没崩（后面还能答话）" 1 || chk "进程没崩" 0

rm -rf "$TMP"
[ "$fail" -eq 0 ] && { echo "PASS teach（显式教学：造词 + 否定 + 动态关系 + 存盘 + 用法）"; exit 0; }
echo "FAIL teach"; exit 1
