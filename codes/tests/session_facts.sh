#!/bin/sh
# tests/session_facts.sh — **会话图**的事实一致性（把互动学到的东西钉住）（§6.178）
#
# 为什么单有一份：`session.bin` 是真正在长的那张图（十几轮互动学到的 60+ 条事实）。
# 单套件都用出厂 `model.bin` 当夹具，**碰不到**"图变厚之后还认不认账"：
#   · 学过的会被后来的会话改掉吗（改口是同一条边只留一个极性）；
#   · 否定/例外会不会被继承或独占规则盖掉；
#   · 厚图里检索会不会抢在关系层前面乱答。
#
# 这里只钉**真事实**（都是我们真教过、且现实为真的那批），外加"正反两问必须相反"的
# 一致性检查。会话图继续长不会让这些事实变假 —— 真要改它们，得是用户明确纠正。
#
# 用法：sh tests/session_facts.sh          （缺 session.bin 时 **SKIP** 而不是红）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
SESSION=${SESSION:-session.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
if [ ! -f "$SESSION" ]; then
    echo "SKIP session_facts（没有 $SESSION —— 会话图不在仓库里）"
    exit 0
fi

TMP=./_session_facts_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

run() {
    { printf '::load %s\n' "$SESSION"; for q in "$@"; do printf '%s\n' "$q"; done; printf '::quit\n'; } \
        | "$BIN" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }
nth()  { reps | sed -n "$1p"; }

# fact <序号> <问句> <期望片段>：一次进程只问一句（互不干扰，也好看清是哪条坏了）
fact() {
    out=$(run "$2")
    r=$(printf '%s\n' "$out" | nth 1)
    printf '%s\n' "$r" | grep -q "$3" \
        && chk "$2 ⇒ $3" 1 \
        || { chk "$2 ⇒ $3" 0 "$r"; return 1; }
    return 0
}

echo "① 真事实：学过的不该忘（每条一个进程）"
fact 1 "麒麟 属于 神兽 吗"        "对 ， 麒麟 -\[属于\]-> 神兽"     || true
fact 2 "麒麟 会 飞 吗"            "对 ， 麒麟 -\[会\]-> 飞"         || true
fact 3 "龙 会 飞 吗"              "对 ， 龙 -\[会\]-> 飞"           || true
fact 4 "凤凰 会 唱歌 吗"          "对 ， 凤凰 -\[会\]-> 唱歌"       || true
fact 5 "企鹅 属于 鸟 吗"          "对 ， 企鹅 -\[属于\]-> 鸟"       || true
fact 6 "企鹅 不 会 飞 吗"         "对 ， 企鹅 -\[不 会\]-> 飞"      || true
fact 7 "企鹅 会 游泳 吗"          "对 ， 企鹅 -\[会\]-> 游泳"       || true
fact 8 "蝙蝠 会 飞 吗"            "对 ， 蝙蝠 -\[会\]-> 飞"         || true
fact 9 "蝙蝠 不 属于 鸟 吗"       "对 ， 蝙蝠 -\[不 属于\]-> 鸟"    || true
fact 10 "鲸鱼 属于 哺乳动物 吗"   "对 ， 鲸鱼 -\[属于\]-> 哺乳动物" || true
fact 11 "鲸鱼 不 属于 鱼 吗"      "对 ， 鲸鱼 -\[不 属于\]-> 鱼"    || true
fact 12 "鲸鱼 不 会 飞 吗"        "对 ， 鲸鱼 -\[不 会\]-> 飞"      || true
fact 13 "桌子 不 会 飞 吗"        "对 ， 桌子 -\[不 会\]-> 飞"      || true
fact 14 "手机 属于 工具 吗"       "对 ， 手机 -\[属于\]-> 工具"     || true

echo "② 一致性：同一件事正反两问必须相反"
pair() {
    out=$(run "$1" "$2")
    a=$(printf '%s\n' "$out" | nth 1)
    b=$(printf '%s\n' "$out" | nth 2)
    ok=1
    printf '%s\n' "$a" | grep -q "^对 " || ok=0
    printf '%s\n' "$b" | grep -q "^不 对 " || ok=0
    chk "$1 ⇒ 对 且 $2 ⇒ 不 对" "$ok" "「$a」/「$b」"
}
pair "企鹅 属于 鸟 吗" "企鹅 不 属于 鸟 吗"
pair "蝙蝠 会 飞 吗" "蝙蝠 不 会 飞 吗"
pair "鲸鱼 属于 哺乳动物 吗" "鲸鱼 不 属于 哺乳动物 吗"
pair "龙 会 飞 吗" "龙 不 会 飞 吗"

echo "③ 图里查得到（不是只在答复里）"
out=$(run "::rel 鲸鱼")
printf '%s\n' "$out" | grep -q -- '--\[属于\]--> 哺乳动物' \
    && chk "::rel 鲸鱼 能看到 属于 哺乳动物" 1 \
    || chk "::rel 鲸鱼 能看到 属于 哺乳动物" 0

if [ "$fail" -ne 0 ]; then
    echo "FAIL session_facts（会话图事实一致性）"
    exit 1
fi
echo "PASS session_facts（会话图：14 条真事实 + 4 组正反一致 + 图里查得到）"
