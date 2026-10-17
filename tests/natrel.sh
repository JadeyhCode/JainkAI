#!/bin/sh
# tests/natrel.sh — 「自然说法」：能/可以 → 会（§6.161，默认关；AIP_RELNAT=1）
#
# 为什么要有这一条：实测 `鸟 能 飞` 说得进（陈述侧把它当**动态谓语「能」**记下），
# 而 `鸟 能 飞 吗` 问不出（查询侧不认 `能`）—— 同一句话**说得进、问不出**。
# 开关打开后 `能`/`可以` 归到核心关系「会」，两边说的是同一件事。
#
# 关着时必须**逐字节不动**（所以第一条断言就是"关着时还是老答案"）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }
run() { { for q in "$@"; do printf '%s\n' "$q"; done; printf '::quit\n'; } | env $ENVV "$BIN" 2>&1; }
ans() { sed -n 's/.*AI > //p' | head -1; }

echo "① 关着（默认）：还是老答案（不动既有行为）"
ENVV=""
r=$(run "::char off" "鸟 能 飞 吗" | ans)
case "$r" in *"不 确定"*) chk "关着：鸟 能 飞 吗 ⇒「不 确定」（老行为）" 1 ;;
                 *)      chk "关着：鸟 能 飞 吗 ⇒「不 确定」" 0 "$r" ;; esac

echo "② 开着：能/可以 → 会（查询侧）"
ENVV="AIP_RELNAT=1"
r=$(run "::char off" "鸟 能 飞 吗" | ans)
case "$r" in *"对"*"鸟"*) chk "开着：鸟 能 飞 吗 ⇒ 对" 1 ;; *) chk "开着：鸟 能 飞 吗 ⇒ 对" 0 "$r" ;; esac
r=$(run "::char off" "鸟 可以 飞 吗" | ans)
case "$r" in *"对"*"鸟"*) chk "开着：鸟 可以 飞 吗 ⇒ 对" 1 ;; *) chk "开着：鸟 可以 飞 吗 ⇒ 对" 0 "$r" ;; esac

echo "③ 开着：陈述侧与查询侧**说的是同一件事**"
out=$(run "::char off" "鱼 能 跳" "鱼 能 跳 吗")
printf '%s\n' "$out" | grep -aq "记下 了" && chk "鱼 能 跳 ⇒ 记下了" 1 || chk "鱼 能 跳 ⇒ 记下了" 0
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | sed -n '2p')   # 第二句才是问句
case "$r" in *"对"*"会"*) chk "鱼 能 跳 吗 ⇒ 对（记的是「会」，查的也是「会」）" 1 ;;
                  *)     chk "鱼 能 跳 吗 ⇒ 对" 0 "$r" ;; esac

echo "④ 开着：否定问句仍然能把话说清"
r=$(run "::char off" "鸟 不 能 飞 吗" | ans)
case "$r" in *"不 对"*) chk "鸟 不 能 飞 吗 ⇒ 不 对（纠正：鸟会飞）" 1 ;;
                 *)      chk "鸟 不 能 飞 吗 ⇒ 不 对" 0 "$r" ;; esac

echo "④b 量词（§6.163）：是 一种 X —— 关着不动、开着才剥"
ENVV=""
r=$(run "::char off" "猫 是 一种 动物 吗" | ans)
case "$r" in *"不 太 懂"*) chk "关着：猫 是 一种 动物 吗 ⇒「不 太 懂」（老行为）" 1 ;;
                 *)          chk "关着：猫 是 一种 动物 吗 ⇒「不 太 懂」" 0 "$r" ;; esac
ENVV="AIP_RELNAT=1"
r=$(run "::char off" "猫 是 一种 动物 吗" | ans)
case "$r" in *"对"*"属于"*) chk "开着：猫 是 一种 动物 吗 ⇒ 对（剥掉量词）" 1 ;;
                   *)        chk "开着：猫 是 一种 动物 吗 ⇒ 对" 0 "$r" ;; esac
r=$(run "::char off" "猫 是 个 动物 吗" | ans)
case "$r" in *"对"*) chk "开着：猫 是 个 动物 吗 ⇒ 对（单字量词也剥）" 1 ;;
             *)      chk "开着：猫 是 个 动物 吗 ⇒ 对" 0 "$r" ;; esac
out=$(run "::char off" "鱼 是 一种 动物" "鱼 是 一种 动物 吗")
r=$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | sed -n '2p')
case "$r" in *"对"*) chk "开着：陈述侧也认（鱼 是 一种 动物 → 问 ⇒ 对）" 1 ;;
             *)      chk "开着：陈述侧也认" 0 "$r" ;; esac

echo "④c 数词+量词（§6.164）：一 只 / 两 只 / 一 种 都能剥"
ENVV="AIP_RELNAT=1"
r=$(run "::char off" "猫 是 一 只 动物 吗" | ans)
case "$r" in *"对"*"属于"*) chk "开着：猫 是 一 只 动物 吗 ⇒ 对（数词+量词）" 1 ;;
                   *)        chk "开着：猫 是 一 只 动物 吗 ⇒ 对" 0 "$r" ;; esac
r=$(run "::char off" "猫 是 两 只 动物 吗" | ans)
case "$r" in *"对"*) chk "开着：猫 是 两 只 动物 吗 ⇒ 对" 1 ;; *) chk "开着：猫 是 两 只 动物 吗 ⇒ 对" 0 "$r" ;; esac
ENVV=""
r=$(run "::char off" "猫 是 一 只 动物 吗" | ans)
case "$r" in *"不 太 懂"*) chk "关着：猫 是 一 只 动物 吗 ⇒「不 太 懂」（老行为）" 1 ;;
                 *)          chk "关着：猫 是 一 只 动物 吗 ⇒「不 太 懂」" 0 "$r" ;; esac

echo "⑤ 不误伤：三词日常句不因为开了开关就变成关系层答"
r=$(run "::char off" "你 能 来 吗" | ans)
case "$r" in *"--\["*) chk "你 能 来 吗 **没有** 被当成关系断言" 0 "$r" ;;
                 *)     chk "你 能 来 吗 没被当成关系断言（$r）" 1 ;; esac

[ "$fail" -eq 0 ] && { echo "PASS natrel（能/可以 → 会：默认关、开着才生效、两边一致、不误伤）"; exit 0; }
echo "FAIL natrel"; exit 1
