#!/bin/sh
# tests/cot.sh — 思维链（::cot）与降级答复的回归
#
# 看护两件事：
#   ① 链是**咬合**的，不是并列念一遍四种策略 ——
#      骨架 → 校验 → 降级：降级那一步只在「校验没成」之后才出现；
#      真骨架接住了就**不该**有降级步。
#   ② 降级答复真的改善了输出：`你知道北京吗` 以前是一句光秃秃的拒答
#      （「这个我 不 太 懂」），现在是「我 不 太 懂 北京」——
#      把用户问的东西复述出来，比笼统认输有用。
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

# run <开关行> <输入...>  →  完整输出（含 ::cot）
run() {
    opt=$1; shift
    { printf '::load %s\n::char off\n' "$MODEL"
      [ -n "$opt" ] && printf '%s\n' "$opt"
      for q in "$@"; do printf '%s\n' "$q"; done
      printf '::cot\n::quit\n'
    } | "$BIN" 2>&1
}
reps()   { sed -n 's/.*AI > //p'; }
stat()   { sed -n 's/.*帧生成=/帧生成=/p' | tail -1; }
cotnum() { grep -n "$1" | sed -n 's/^\([0-9]*\):.*/\1/p' | head -1; }
# 链里第几步是某个动作（返回空表示没有）
step_of() { printf '%s\n' "$1" | sed -n 's/^ *\([0-9]*\)\. '"$2"'.*/\1/p' | head -1; }

echo "① 骨架接住了：链上该有校验[成]，不该有降级"
out=$(run "" "你知道量子吗")
r=$(printf '%s\n' "$out" | reps | head -1)
[ "$r" = "我 知道 量子" ] && chk "真骨架答出来了（$r）" 1 || chk "真骨架答出来了（实得「$r」）" 0
p=$(step_of "$out" 骨架); g=$(step_of "$out" 校验); f=$(step_of "$out" 降级)
[ -n "$p" ] && [ -n "$g" ] && chk "链上有 骨架($p) 和 校验($g)" 1 \
                          || chk "链上有 骨架 和 校验" 0
[ -z "$f" ] && chk "真接住时**没有**降级步" 1 || chk "真接住时不该有降级步（却在第 $f 步）" 0

echo "② 骨架接不住：降级必须在校验[没成]之后"
out=$(run "" "你知道北京吗")
r=$(printf '%s\n' "$out" | reps | head -1)
# 降级答复可能被表达层加一个语气前缀（应该/大概/可能…），所以查**包含**
# 「不 太 懂 北京」而不是整串相等：表达层的语气由 sig_gap 等派生信号决定，
# 而派生层（槽类/emb/idf）现在是「按最终的图（含关系层、帧层）现算」的 ——
# 以前槽类算在关系层之前，装载态与训练态会给出**不同**的答复，
# 这个测试当时钉住的是那个不一致态。见 tests/model_roundtrip.sh 的 [3/5]。
case "$r" in
    *"不 太 懂 北京"*) chk "降级答复复述了用户的词（$r）" 1 ;;
    *)                 chk "降级答复复述了用户的词（实得「$r」）" 0 ;;
esac
p=$(step_of "$out" 骨架); g=$(step_of "$out" 校验); f=$(step_of "$out" 降级)
[ -n "$p" ] && [ -n "$g" ] && [ -n "$f" ] && chk "三步都在（骨架 $p / 校验 $g / 降级 $f）" 1 \
                                        || chk "三步都在（骨架 ${p:-无} / 校验 ${g:-无} / 降级 ${f:-无}）" 0
if [ -n "$p" ] && [ -n "$g" ] && [ -n "$f" ] && [ "$p" -lt "$g" ] && [ "$g" -lt "$f" ]; then
    chk "顺序咬合：骨架 < 校验 < 降级（$p < $g < $f）" 1
else
    chk "顺序咬合：骨架 < 校验 < 降级" 0
fi
printf '%s\n' "$out" | grep -q '类证据不够' \
    && chk "校验步写明了为什么没过（类证据不够）" 1 \
    || chk "校验步写明了为什么没过（类证据不够）" 0

echo "③ ::fall off 时不再降级"
out=$(run "::fall off" "你知道北京吗")
r=$(printf '%s\n' "$out" | reps | head -1)
# 关掉降级答复后，回复里**不该再复述用户的词**（「这个我 不 太 懂」是拒答池里的
# 通用说法，不算降级答复；降级答复的特征是把用户的词拼回去）。
case "$r" in
    *"不 太 懂 北京"*) chk "关掉后不再是降级答复（$r）" 0 ;;
    *)                 chk "关掉后不再是降级答复（$r）" 1 ;;
esac
printf '%s\n' "$out" | stat | grep -q '降级0' && chk "关掉后 降级=0" 1 || chk "关掉后 降级=0" 0
[ -z "$(step_of "$out" 降级)" ] && chk "关掉后链上也没有降级步" 1 \
                                || chk "关掉后链上不该有降级步" 0

echo "④ 链是「这一轮」的，不是攒起来的"
out=$(run "" "你好呀" "你知道北京吗")
n=$(printf '%s\n' "$out" | sed -n 's/.*上一轮 \([0-9]*\) 步.*/\1/p' | head -1)
[ -n "$n" ] && [ "$n" -le 10 ] && chk "只报上一轮（$n 步，不是两轮之和）" 1 \
                              || chk "只报上一轮（实得 ${n:-?} 步）" 0
printf '%s\n' "$out" | grep -q '累计' && chk "累计步数另算" 1 || chk "累计步数另算" 0

echo "⑤ 降级答复里的碎词要拼回整词"
out=$(run "" "你知道北京吗")
printf '%s\n' "$out" | grep -q '我 不 太 懂 北京' \
    && chk "输出是「我 不 太 懂 北京」而不是「北 京」" 1 \
    || chk "输出是「我 不 太 懂 北京」而不是「北 京」" 0

echo
[ "$fail" -eq 0 ] && echo "PASS cot（思维链：骨架→校验→降级 咬合，降级可关，碎词拼回）" \
                  || { echo "FAIL cot"; exit 1; }
