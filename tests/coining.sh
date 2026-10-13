#!/bin/sh
# tests/coining.sh — 「现场造词」的回归
#
# 看护这条能力：用户说了个语料里没有的复合词（`你 吃 火锅 了 吗`），
# 系统当场把它造成一个 Token、接到该槽位的示范词上，并记进白名单。
#
# 五个场景，每个都在**新进程里重新加载模型**（造词会改图，不隔离会互相污染）：
#   ① 基本：造出来 → 答出「我 吃 了 火锅」，且 火锅 成为一个 Token
#   ② 重复：只造一次（第二次走白名单，不再造）
#   ③ 存档重载：仍然认得出（靠 ASSOC_ROLE_COINED 的类成员边）
#   ④ 开关：::coin off 时不造（退回按字答「我 吃 了 火 锅」）
#   ⑤ 反面：不许把「怕+危险」「知道+天气」这种**非切词产物**拼成新词；
#           没有任何部件有图证据时（北京）也不许造
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_coin_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

run() {   # 把 stdin 喂给二进制，返回全部输出
    "$BIN" 2>&1
}
reply() {   # 从输出里取最后一条 AI 回复
    sed -n 's/.*AI > //p' | tail -1
}
stat_line() {   # 从输出里取最后的计数行
    sed -n 's/.*帧生成=/帧生成=/p' | tail -1
}

fail=0
chk() {  # $1=说明 $2=0/1
    if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi
}

echo "① 基本：造出「火锅」"
out=$(printf '::load %s\n::char off\n你吃火锅了吗\n::think 火锅\n::stats\n::quit\n' "$MODEL" | run)
r=$(printf '%s\n' "$out" | reply)
[ "$r" = "我 吃 了 火锅" ] && chk "回复是「我 吃 了 火锅」（实得「$r」）" 1 \
                          || chk "回复是「我 吃 了 火锅」（实得「$r」）" 0
# ::think 火锅 应该报「思考 1 个词」—— 说明它已经是一个 Token 而不是 火+锅
printf '%s\n' "$out" | grep -q '思考 1 个词' \
    && chk "「火锅」已是一个整体 Token（::think 报 1 个词）" 1 \
    || chk "「火锅」已是一个整体 Token（::think 报 1 个词）" 0
printf '%s\n' "$out" | stat_line | grep -q '现场造词1' \
    && chk "统计里 现场造词=1" 1 || chk "统计里 现场造词=1" 0

echo "② 重复三次：只许造一次"
out=$(printf '::load %s\n::char off\n你吃火锅了吗\n你吃火锅了吗\n你吃火锅了吗\n::stats\n::quit\n' "$MODEL" | run)
s=$(printf '%s\n' "$out" | stat_line)
printf '%s\n' "$s" | grep -q '现场造词1' && chk "现场造词仍为 1（$s）" 1 \
                                         || chk "现场造词仍为 1（$s）" 0
n=$(printf '%s\n' "$out" | grep -c '我 吃 了 火锅' || true)
[ "$n" -eq 3 ] && chk "三次都答「我 吃 了 火锅」" 1 || chk "三次都答「我 吃 了 火锅」（实得 $n 次）" 0

echo "③ 存档重载后仍认得出"
out=$(printf '::load %s\n::char off\n你吃火锅了吗\n::save %s/m.bin\n::load %s/m.bin\n::char off\n你吃火锅了吗\n::stats\n::quit\n' \
        "$MODEL" "$TMP" "$TMP" | run)
r=$(printf '%s\n' "$out" | reply)
[ "$r" = "我 吃 了 火锅" ] && chk "重载后仍答「我 吃 了 火锅」（实得「$r」）" 1 \
                          || chk "重载后仍答「我 吃 了 火锅」（实得「$r」）" 0
s=$(printf '%s\n' "$out" | stat_line)
printf '%s\n' "$s" | grep -q '现场造词0' && chk "重载后不再造词（$s）" 1 \
                                        || chk "重载后不再造词（$s）" 0

echo "④ ::coin off 时不造词"
out=$(printf '::load %s\n::char off\n::coin off\n你吃火锅了吗\n::stats\n::quit\n' "$MODEL" | run)
r=$(printf '%s\n' "$out" | reply)
[ "$r" = "我 吃 了 火 锅" ] && chk "关掉后按字答「我 吃 了 火 锅」（实得「$r」）" 1 \
                           || chk "关掉后按字答「我 吃 了 火 锅」（实得「$r」）" 0
printf '%s\n' "$out" | stat_line | grep -q '现场造词0' \
    && chk "关掉后 现场造词=0" 1 || chk "关掉后 现场造词=0" 0

echo "⑤ 反面：非切词产物 / 无证据都不许造"
for case in "你怕危险吗:怕危险" "你知道天气吗:知道天气" "你喜欢北京吗:北京" ; do
    q=${case%%:*}; bad=${case##*:}
    out=$(printf '::load %s\n::char off\n%s\n::stats\n::quit\n' "$MODEL" "$q" | run)
    if printf '%s\n' "$out" | grep -q "$bad"; then
        chk "$q 不许造出「$bad」" 0
    else
        chk "$q 不许造出「$bad」（现答：$(printf '%s\n' "$out" | reply)）" 1
    fi
done
# 北京 的部件 北/京 在图里一条边都没有 → 无从关联 → 也不该造
out=$(printf '::load %s\n::char off\n你喜欢北京吗\n::stats\n::quit\n' "$MODEL" | run)
printf '%s\n' "$out" | stat_line | grep -q '现场造词0' \
    && chk "「北京」部件无图证据，不造词" 1 || chk "「北京」部件无图证据，不造词" 0

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS coining（现场造词：造出 / 只造一次 / 重载仍在 / 可关 / 不乱造）" \
                  || { echo "FAIL coining"; exit 1; }
