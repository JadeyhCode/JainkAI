#!/bin/sh
# tools/teach_skeleton.sh — **教它写一个新程序**：骨架 → ingest → synth → 编译 → 自测（§6.753）
#
# 机制（这是"能不能训练其写代码"的答案）：代码合成走
#   任务:<名字> --关键词--> 关键词  +  任务:<名字> --骨架--> 骨架:<名字>  +  骨架:<名字> --代码--> 可编译程序
# 程序里**自带自测**（打印 ok/FAIL，全过才 return 0），`::code synth` 编译并跑它 ⇒ "测试全绿" 才算会写。
#
# 用法：
#   sh tools/teach_skeleton.sh <任务名> <关键词,逗号分隔> <程序文件.c> [模型文件]
# 例：
#   sh tools/teach_skeleton.sh 温度转换 华氏,摄氏,温度 /tmp/f2c.c code/code_model_taught.bin
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TASK=${1:?用法: sh tools/teach_skeleton.sh <任务名> <关键词,逗号分隔> <程序.c> [模型]}
KEYS=${2:?}; PROG=${3:?}; MODEL=${4:-$ROOT/code/code_model_taught.bin}
[ -s "$PROG" ] || { echo "FAIL: 没有程序文件 $PROG"; exit 1; }
[ -s "$MODEL" ] || { echo "FAIL: 没有模型 $MODEL（先 sh code/build_code_model.sh）"; exit 1; }
TRIP=$(mktemp); trap 'rm -f "$TRIP"' EXIT
printf '任务:%s\t骨架\t骨架:%s\n' "$TASK" "$TASK" >> "$TRIP"
for k in $(echo "$KEYS" | tr ',' ' '); do printf '任务:%s\t关键词\t%s\n' "$TASK" "$k" >> "$TRIP"; done
printf '骨架:%s\t语言\tC11\n' "$TASK" >> "$TRIP"
printf '骨架:%s\t依赖\tstdio.h\n' "$TASK" >> "$TRIP"
# 程序按 `\` 续行格式写进去（和 skeletons.triples 一致）
first=1
while IFS= read -r line; do
    if [ "$first" -eq 1 ]; then
        printf '骨架:%s\t代码\t%s\\\n' "$TASK" "$line" >> "$TRIP"; first=0
    else
        printf '%s\\\n' "$line" >> "$TRIP"
    fi
done < "$PROG"
sed -i '$ s/\\$//' "$TRIP"     # 最后一行不要续行符
echo "== 教「$TASK」（$(wc -l < "$TRIP") 条三元组）⇒ $MODEL"
printf '::ingest %s\n::save %s\n::quit\n' "$TRIP" "$MODEL" | "$ROOT/build_aip" --model "$MODEL" 2>&1 | grep -a "导入" | head -1
echo "== 验证：::code synth 能不能写出来 =="
if printf '::act on\n::code synth %s\n::quit\n' "$TASK" | "$ROOT/build_aip" --model "$MODEL" 2>&1 | grep -qa '测试全绿'; then
    echo "   ✓ 写出来了，而且测试全绿"
else
    echo "   ✗ 还是写不出来（看 ::code 看痕）"
    exit 1
fi
