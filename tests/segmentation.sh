#!/bin/sh
# tests/segmentation.sh — 「切分是可以选的」这条能力的回归
#
# 切词器只给一版切分（最长匹配），但哪一版对要看**能不能被理解**。
# 这里看护四个操作，每个都在新进程里重新加载模型：
#
#   ① 拆解：`你 睡 得 好吗` —— `好吗` 是个词典词，挡住了模板要的 `吗`；
#            拆成 好 + 吗 就能匹配（好 进槽位，吗 落在固定词位置）
#   ② 合并：`你 知道 人工 智能 吗` —— 切词器把「人工智能」切成四个单字；
#            拼回去（拼出来的必须已经是池子里的词）才能匹配
#   ③ 中转：`猫跟狗哪个好` —— 模板写死了「和」；和/跟/与 互认才接得住
#   ④ 护栏：拆开的碎片不许一起塞进同一个槽位
#            （`你 怕 老师 吗` 拆成 老+师 会答出「我 怕 老 师」）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

fail=0
chk() {  # $1=说明 $2=0/1
    if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi
}

# run <开关行> <输入行...>  →  打印所有 AI 回复
run() {
    opt=$1; shift
    { printf '::load %s\n::char off\n' "$MODEL"
      [ -n "$opt" ] && printf '%s\n' "$opt"
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::stats\n::quit\n'
    } | "$BIN" 2>&1
}
reps()  { sed -n 's/.*AI > //p'; }
stat()  { sed -n 's/.*帧生成=/帧生成=/p' | tail -1; }
nth()   { sed -n "${1}p"; }

echo "① 拆解：你睡得好吗"
out=$(run "" "你睡得好吗" "你喜欢雪吗")
r1=$(printf '%s\n' "$out" | reps | nth 1)
r2=$(printf '%s\n' "$out" | reps | nth 2)
[ "$r1" = "我 睡 得 好 你 呢" ] && chk "拆解后匹配上帧（$r1）" 1 \
                               || chk "拆解后匹配上帧（实得「$r1」）" 0
printf '%s\n' "$out" | stat | grep -q '拆解1' && chk "统计里 拆解=1" 1 || chk "统计里 拆解=1" 0
[ "$r2" = "我 喜欢 雪" ] && chk "主切分能中的句子不受影响（$r2）" 1 \
                        || chk "主切分能中的句子不受影响（实得「$r2」）" 0

out=$(run "::seg off" "你睡得好吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
[ "$r" != "我 睡 得 好 你 呢" ] && chk "::seg off 时不再拆解（$r）" 1 \
                               || chk "::seg off 时不再拆解" 0
printf '%s\n' "$out" | stat | grep -q '拆解0' && chk "关掉后 拆解=0" 1 || chk "关掉后 拆解=0" 0

echo "② 合并：你知道人工 智能吗"
out=$(run "" "你知道人工 智能吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
[ "$r" = "我 知道 人工智能" ] && chk "合并后匹配上帧（$r）" 1 \
                             || chk "合并后匹配上帧（实得「$r」）" 0
printf '%s\n' "$out" | stat | grep -q '合并1' && chk "统计里 合并=1" 1 || chk "统计里 合并=1" 0
out=$(run "::seg off" "你知道人工 智能吗")
printf '%s\n' "$out" | stat | grep -q '合并0' && chk "::seg off 时不再合并" 1 \
                                            || chk "::seg off 时不再合并" 0

echo "③ 中转：猫跟狗哪个好"
out=$(run "" "猫跟狗哪个好")
r=$(printf '%s\n' "$out" | reps | nth 1)
[ "$r" = "我 觉得 猫 更 好" ] && chk "中转词容错接住了（$r）" 1 \
                             || chk "中转词容错接住了（实得「$r」）" 0
printf '%s\n' "$out" | stat | grep -q '中转1' && chk "统计里 中转=1" 1 || chk "统计里 中转=1" 0
out=$(run "::relay off" "猫跟狗哪个好")
r=$(printf '%s\n' "$out" | reps | nth 1)
[ "$r" != "我 觉得 猫 更 好" ] && chk "::relay off 时不再容错（$r）" 1 \
                              || chk "::relay off 时不再容错" 0
out=$(run "::relay off" "猫和狗哪个好")
r=$(printf '%s\n' "$out" | reps | nth 1)
[ "$r" = "我 觉得 猫 更 好" ] && chk "模板原词「和」在关掉后依然能中（$r）" 1 \
                             || chk "模板原词「和」在关掉后依然能中（实得「$r」）" 0

echo "④ 护栏：碎片不许一起塞进槽位"
out=$(run "" "你怕老师吗" "你睡得好吗")
r1=$(printf '%s\n' "$out" | reps | nth 1)
printf '%s\n' "$out" | grep -q '我 怕 老 师' && chk "没有「我 怕 老 师」这种碎片答案" 0 \
                                             || chk "没有「我 怕 老 师」这种碎片答案（现答：$r1）" 1
out=$(run "::seg off" "你怕老师吗")
r=$(printf '%s\n' "$out" | reps | nth 1)
printf '%s\n' "$out" | grep -q '我 怕 老 师' && chk "关掉换切分后也没有碎片答案" 0 \
                                             || chk "关掉换切分后也没有碎片答案（现答：$r）" 1

echo
[ "$fail" -eq 0 ] && echo "PASS segmentation（拆解 / 合并 / 中转 / 碎片护栏，各自可关）" \
                  || { echo "FAIL segmentation"; exit 1; }
