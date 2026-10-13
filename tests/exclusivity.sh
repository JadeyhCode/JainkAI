#!/bin/sh
# tests/exclusivity.sh — 「独占」不是手写公理，而是**量**出来的
#
# 语义：「属于」这种关系是函数式的（一个主语最多一个宾语），有了它才能从
# 「猫 属于 动物」推出「猫 不属于 植物」。以前这张表是写死在 rel.h 里的
# （只给 属于/在 标了独占），数据只过一个「≥5 个主语且多宾语率 ≤20%」的门槛 ——
# 结果真实模型上「改口 0 项」，等于没量。
#
# 现在：判定 = 似然比（H_多值 vs H_独占），先验由**参照类**给出
# （参照类 = 观察到过多宾语行为的那些关系），AI_RELS 里的声明只当先验。
# 这个测试证明三件事：
#   ① 数据能**印证**声明（属于/在/吃 的判定有数据支持）；
#   ② 数据能**推翻**声明 —— 只能靠伪造数据证明：
#        让「属于」的主语个个带两个宾语 → 必须改口成「多值」；
#        让「喜欢」的主语个个只有一个宾语 → 必须改口成「独占」；
#        让「会」两边都有 → 老实说「证据不够→按声明」；
#   ③ 改口之后**推理跟着变**：多值的「属于」不许再靠独占推出否定
#      （健全性：宁可说不知道，也不能说错）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT/build_aip}
WORK="$ROOT/_t/excl"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }

# 小语料：只要能让模型跑起来（独占与语料无关，关系全来自 relations.txt）
cat > corpus.txt <<'EOF'
[原语:对象] 你好 [原语:分割]
[原语:对象] 猫 [原语:分割]
[原语:对象] 我 喜欢 猫 [原语:分割]
[原语:对象] 猫 属于 动物 [原语:分割]
EOF

# run <关系库内容> <提问>：训练 → 装载 → ::excl + ::stats + 提问
run() {
    printf '%s\n' "$1" > relations.txt
    printf '::save m0\n::quit\n' | "$BIN" corpus.txt > train.log 2>&1
    { printf '::load m0\n::char off\n::excl\n::stats\n%s\n::quit\n' "$2"; } | "$BIN" > run.log 2>&1
}
row()   { grep -a "  $1 " run.log | head -1; }          # ::excl 里的那一行
reps()  { sed -n 's/.*AI > //p' run.log | head -1; }     # 最后一句回复
# ::load 之后程序自己会打一次统计，我们自己的 ::stats 再打一次 —— 取最后一条
stat()  { grep -a '关系独占表：' run.log | tail -1; }

echo "① 用真实的 relations.txt：声明被数据印证"
run "$(cat "$ROOT/relations.txt")" "猫 属于 植物 吗"
r=$(row 属于)
case "$r" in *"独占"*"[数据印证]"*) chk "属于：数据支持独占（$r）" 1 ;;
                            *)       chk "属于：数据支持独占（实得「$r」）" 0 ;;
esac
r=$(row 在)
case "$r" in *"独占"*"[数据印证]"*) chk "在：数据支持独占（$r）" 1 ;;
                            *)       chk "在：数据支持独占（实得「$r」）" 0 ;;
esac
r=$(row 吃)
case "$r" in *"多值"*"[数据印证]"*) chk "吃：数据支持多值（$r）" 1 ;;
                            *)       chk "吃：数据支持多值（实得「$r」）" 0 ;;
esac
case "$(reps)" in *"不 对"*) chk "独占成立时，靠独占推出否定（$(reps)）" 1 ;;
                 *)          chk "独占成立时，靠独占推出否定（实得「$(reps)」）" 0 ;;
esac

echo "② 伪造数据推翻声明：属于 → 多值，喜欢 → 独占，会 → 证据不够"
# relations.txt 的格式是「<主语> <关系词> <宾语>」—— 别把关系词当主语写进去
run '猫 属于 动物
猫 属于 宠物
狗 属于 动物
狗 属于 宠物
鸟 属于 动物
鸟 属于 宠物
鱼 属于 动物
鱼 属于 宠物
猫 有 毛
猫 有 爪子
狗 有 毛
狗 有 尾巴
鸟 有 羽毛
鸟 有 翅膀
猫 会 跑
猫 会 跳
狗 会 跑
鸟 会 飞
我 喜欢 猫
你 喜欢 猫
他 喜欢 猫
她 喜欢 猫
' "猫 属于 植物 吗"
r=$(row 属于)
case "$r" in *"多值"*"[数据改口]"*) chk "属于被数据推翻成「多值」（$r）" 1 ;;
                             *)      chk "属于被数据推翻成「多值」（实得「$r」）" 0 ;;
esac
r=$(row 喜欢)
case "$r" in *"独占"*"[数据改口]"*) chk "喜欢被数据扶成「独占」（$r）" 1 ;;
                             *)      chk "喜欢被数据扶成「独占」（实得「$r」）" 0 ;;
esac
r=$(row 会)
case "$r" in *"[证据不够→按声明]"*) chk "样本不一边倒时老实说证据不够（$r）" 1 ;;
                            *)      chk "样本不一边倒时老实说证据不够（实得「$r」）" 0 ;;
esac
stat_line=$(stat)
case "$stat_line" in *"属于=多值"*"喜欢=独占"*) chk "统计行跟着改（$stat_line）" 1 ;;
                              *)                chk "统计行跟着改（实得「$stat_line」）" 0 ;;
esac
case "$(reps)" in *"不 对"*) chk "多值的属于**不许**再推否定（实得「$(reps)」）" 0 ;;
                 *)          chk "多值的属于**不许**再推否定（$(reps)）" 1 ;;
esac
grep -aq '改口 2 项' run.log && chk "统计里如实报了改口条数" 1 || chk "统计里如实报了改口条数" 0

echo
[ "$fail" -eq 0 ] && echo "PASS exclusivity（独占由数据判定：可印证、可改口、改口后推理跟着变）" \
                  || { echo "FAIL exclusivity"; exit 1; }
