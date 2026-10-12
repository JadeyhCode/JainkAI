#!/bin/sh
# tests/meas_trigger.sh — 量「对话触发」认多少种**说法**
#
# 为什么要量：`tokenop.sh` 第 ⑩ 节只钉住了 5 句话（加个词 / 加个前提 / 提问不触发 /
# 塞个条件 / 搭个词儿）。而这一层的已知局限就是"**认说法**，不是懂话"（§四.B①）：
# 到底认多少种、哪些说法漏了、哪些该不触发却触发了 —— 没人量过。
#
# 做法：每句话开一个**干净会话**（模型从 pristine 复制），看回复里有没有
#   「照做 了」（触发）以及触发的是哪一类：
#     插词 → 回复里有 `[临:`（插进去的是临时 token）
#     前提 → 回复里有 `前提`
#   然后与**期望**对照（ins / prem / none）。
#
# 用法：sh tests/meas_trigger.sh [输出文件]      默认 _t/meas/trigger.tsv
#       AIP_TRIGGER=norm sh tests/meas_trigger.sh   量新匹配器（说法归一表 + 槽位）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${1:-_t/meas/trigger.tsv}
WORK=_t/trigger_meas
rm -rf "$WORK"; mkdir -p "$WORK"
mkdir -p "$(dirname "$OUT")"
[ -f model.bin ] || { echo "缺 model.bin"; exit 1; }
CC=${CC:-gcc}
$CC -std=c11 -O2 -o "$WORK/aip" main.c -lm
cp model.bin "$WORK/"
cp frames.txt relations.txt "$WORK/" 2>/dev/null || true

: > "$OUT"
ok=0; bad=0
# 表：<期望>\t<话>\t<名字断言>
#   期望     ins=该触发且插词 / prem=该触发且加前提 / none=不该触发
#   名字断言  - = 不查；临 = 必须插出**临时 token**（`临:`）；<词> = 插进去的必须叫这个名字
#   （名字断言是必须的：`加 上 个 词` 会**误把"上"当成词名**插进去 —— 那也是错，得量出来）
while IFS='	' read -r want phrase wantname; do
    case $want in ''|'#'*) continue ;; esac
    ( cd "$WORK" && cp model.bin m.bin )
    out=$(cd "$WORK" && printf '::notepad off\n::rel analogy off\n::rel analogy solver off\n::premhold mention\n%s\n::quit\n' "$phrase" \
          | ./aip --model m.bin 2>&1)
    got=none
    # 只看**触发那一句回复**（整段输出里有 `::premhold mention` 的状态行，它也带"前提"两个字，
    # 拿整段判会把插词误判成加前提 —— 第一版就是这么错的）。
    # 触发的两种形状（实测）：插词 `A —X→ B（已提出）`；前提 `A —[前提]→ P → 鱼`。
    line=$(printf '%s\n' "$out" | grep '照做 了' | tail -1)
    if [ -n "$line" ]; then
        if printf '%s\n' "$line" | grep -q '前提'; then got=prem; else got=ins; fi
    fi
    verdict=OK
    [ "$got" = "$want" ] || verdict="**种类不对**"
    if [ "$verdict" = OK ] && [ "$wantname" != "-" ]; then
        case $wantname in
            临) printf '%s\n' "$line" | grep -q '临:' || verdict="**该插临时 token，却插了别的**" ;;
            *)  printf '%s\n' "$line" | grep -q -- "$wantname" || verdict="**名字不对（该是 $wantname）**" ;;
        esac
    fi
    if [ "$verdict" = OK ]; then ok=$((ok+1)); mark=OK
    else bad=$((bad+1)); mark="$verdict"; fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$want" "$got" "$mark" "$phrase" "$wantname" >> "$OUT"
done <<'TABLE'
ins	猫 和 鱼 之间 加 个 词	临
ins	猫 和 鱼 之间 加 词	-
ins	把 猫 和 鱼 之间 加 个 词	-
ins	在 猫 和 鱼 之间 加 个 词	-
ins	给 猫 和 鱼 之间 加 个 词	-
ins	猫 跟 鱼 中间 插 个 词儿	-
ins	猫 和 鱼 之间 塞 个 词 吧	-
ins	猫 和 鱼 之间 放 一个 词	-
ins	猫 和 鱼 之间 接 个 词	-
ins	猫 和 鱼 之间 搭 个 token	-
ins	猫 和 鱼 之间 添 个 词	-
ins	猫 鱼 之间 加 个 词	-
ins	猫 和 鱼 之间 加 个 词 月亮	月亮
ins	猫 和 鱼 之间 堆 个 词	-
ins	猫 和 鱼 之间 搁 个 词	-
ins	猫 和 鱼 之间 加 上 个 词	临
ins	猫 和 鱼 当中 加 个 词	-
ins	猫 和 鱼 之间 给 我 加 个 词	-
ins	加 个 词 在 猫 和 鱼 之间	-
ins	猫 和 鱼 之间 加 一 个 词	-
prem	猫 和 鱼 之间 加 个 前提 饿	-
prem	猫 和 鱼 之间 塞 个 条件 饿	-
prem	把 猫 和 鱼 之间 加 个 条件 饿	-
prem	猫 和 鱼 之间 加 个 前题 饿	-
prem	猫 和 鱼 之间 加 个 反 前提 饿	-
prem	猫 和 鱼 之间 加 个 前提词 饿	-
none	猫 和 鱼 之间 有 什么 关系	-
none	猫 和 鱼 是 什么 关系	-
none	我 想 知道 猫 和 鱼 之间 有 什么 关系	-
none	猫 和 鱼 之间 的 关系 是 什么	-
none	猫 和 鱼 之间 别 加 词	-
none	猫 和 鱼 之间 不 要 加 词	-
none	猫 和 鱼 之间 别 塞 个 条件	-
TABLE
printf '\n对话触发覆盖：%d 对 / %d 不一致（共 %d 句）\n' "$ok" "$bad" "$((ok+bad))"
awk -F'\t' '$3 != "OK" { printf "  [期望 %s→实际 %s] %s  %s\n", $1, $2, $3, $4 }' "$OUT"
