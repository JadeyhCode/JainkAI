#!/bin/sh
# tests/bucketload.sh — **每个数据桶都得"承重"**（§6.378）
#
# 为什么单有一份：这几轮我手工做了五六次消融（`AIP_NODATA=lead/slot/confuse/reply/fill`），
# 每次都靠人记得去验。口径是：**关掉这个桶，至少有一条探针的答复必须变**——
# 否则要么这个桶是死数据，要么探针选的句子根本没走它。
#
# 表里每一行 = `桶名|探针输入`。检查方式：同一句跑两次（开 / 关该桶），答复必须不同。
# 注意：`shape` 的消融只关**表**，代码里的形状引擎不受影响；`reply` 关掉后
# 天气/寒暄这些会掉到别的路径，正是我们要看的变化。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }
[ -f patterns.txt ] || { echo "FAIL: 缺 patterns.txt"; exit 1; }

TMP=./_bucketload_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（$3）}"; fail=1; fi }

ask() {   # ask <桶名或空> <输入> → 第一句答复；输入里的 `;` 当换行（多轮探针用，§6.534）
    printf '%s\n::quit\n' "$(printf '%s' "$2" | tr ';' '\n')" \
        | env AIP_NODATA="$1" timeout 30 "$BIN" --model "$MODEL" 2>/dev/null \
        | sed -n 's/.*AI > //p' | tail -1
}

echo "① 每个桶：关掉它，至少一条探针的答复必须变"
while IFS='|' read -r bucket probe; do
    case "$bucket" in ''|'#'*) continue ;; esac
    on=$(ask "" "$probe")
    off=$(ask "$bucket" "$probe")
    if [ -z "$on" ] || [ -z "$off" ]; then
        chk "[$bucket] 探针没拿到答复" 0 "on=「$on」 off=「$off」"
    elif [ "$on" = "$off" ]; then
        chk "[$bucket] 关掉之后答复没变（死桶或探针没走它）" 0 "两侧都是「$on」"
    else
        chk "[$bucket] 承重（$probe）" 1
    fi
done <<'TABLE'
lead|请注意 猫 属于 什么
slot|猫 属于 哪一类
confuse|我不明白你说什么
reply|天气 怎么样
shape|猫 是 哪 类 动物
syn|猫咪属于什么
affix|猫们属于什么
tail|猫 会 飞 吗 呀
fill|猫 真的 会 飞 吗
class|你 嫌 我 吗
qword|猫 哪儿 在
qpart|请问猫是什么
say|猫 属于 动物 吗
bare|好的
verdict|猫 属于 动物 吗;不对
TABLE

echo "② 全部关掉（AIP_NODATA=1）时，每条探针都必须变（证明它们都真的依赖数据）"
while IFS='|' read -r bucket probe; do
    case "$bucket" in ''|'#'*) continue ;; esac
    on=$(ask "" "$probe")
    off=$(ask "1" "$probe")
    if [ "$on" = "$off" ]; then
        chk "[$bucket] 全关也不变 ⇒ 这条探针不依赖数据" 0 "「$on」"
    fi
done <<'TABLE'
lead|请注意 猫 属于 什么
slot|猫 属于 哪一类
confuse|我不明白你说什么
reply|天气 怎么样
shape|猫 是 哪 类 动物
syn|猫咪属于什么
affix|猫们属于什么
tail|猫 会 飞 吗 呀
fill|猫 真的 会 飞 吗
class|你 嫌 我 吗
qword|猫 哪儿 在
qpart|请问猫是什么
# say 不列在②里：它的**种子表就是同一批字符串**（没有数据时兜底），
# 所以"全部关掉"不改变答复；它靠①单桶消融证明承重（§6.532）。
TABLE
echo "  （②没有逐条 OK，只在失败时报；全过就是数据都承重）"

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS bucketload（15 个桶都有'关掉就变'的探针）" \
                  || { echo "FAIL bucketload"; exit 1; }
