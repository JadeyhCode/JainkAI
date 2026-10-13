#!/bin/sh
# tests/meas_wgate_cand.sh — 量「写入分级闸门」真正要判的那批：**AI 自己想沉淀的候选**
#
# 为什么单独一个测量脚本：
#   `tools/wgateprobe.c` 扫的是**图里已有的关系边**，而出厂模型里那 167 条是
#   `relations.txt` 装载时打的**基准边**（role=INNATE）—— 闸门根本不管它们。
#   拿基准边当标尺会把阈值定偏（这事真发生过，见 docs/audit-vs-deepseek看这里.md §6.43）。
#   闸门管的是**候选**：`think.h` 里边 `pend_cache` 那一批「自己推出来、想沉淀成缓存」的事实。
#
# 做法：每条"能推出来"的问题开一个**干净会话**（模型从 pristine 复制一份），
#   `::wgate on` 后问一句，再 `::wgate` 读「最近一次关系写入的重要度」+「那条候选自己的证据」。
#
# 用法：sh tests/meas_wgate_cand.sh [输出文件]        默认 _t/meas/wgate_cand.tsv
#       AIP_WGATE_EV=0 sh tests/meas_wgate_cand.sh   量老公式（证据项关）
# 产物：每行 `<问题>\t<imp 或 NA>\t<链 N 跳 | 链底 M 分>`
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${1:-_t/meas/wgate_cand.tsv}
WORK=_t/wgate_cand
rm -rf "$WORK"; mkdir -p "$WORK"
mkdir -p "$(dirname "$OUT")"

[ -f model.bin ] || { echo "缺 model.bin（先训练）"; exit 1; }
CC=${CC:-gcc}
$CC -std=c11 -O2 -o "$WORK/aip" main.c -lm
cp model.bin "$WORK/"
cp frames.txt relations.txt "$WORK/" 2>/dev/null || true

: > "$OUT"
n_imp=0; n_tot=0
# 这些问句都能**推出来**（关系库里的传递/继承链），所以每条都会产生一个候选：
#   X 有 Y 吗（X 属于 动物 ⊂ 有 Y）、X 属于 Y 吗（传递闭包）
for q in \
  '猫 有 毛 吗' '狗 有 毛 吗' '老鼠 有 腿 吗' '鸟 有 眼睛 吗' '鱼 有 心 吗' \
  '猫 有 眼睛 吗' '狗 有 腿 吗' '老鼠 有 心 吗' '人 有 腿 吗' '鸟 有 心 吗' \
  '猫 属于 生物 吗' '狗 属于 生物 吗' '鸟 属于 生物 吗' '鱼 属于 生物 吗' \
  '老鼠 属于 生物 吗' '花 属于 生物 吗' '草 属于 生物 吗' '树 属于 生物 吗' \
  '苹果 属于 世界 吗' '米饭 属于 世界 吗' '面包 属于 世界 吗' '菜 属于 世界 吗' \
  '肉 属于 世界 吗' '茶 属于 世界 吗' '咖啡 属于 世界 吗' '牛奶 属于 世界 吗' \
  '火车 属于 工具 吗' '汽车 属于 工具 吗' '水果 属于 世界 吗' '饮料 属于 世界 吗' \
  '老师 属于 动物 吗' '学生 属于 动物 吗' '小孩 属于 动物 吗' '朋友 属于 动物 吗' \
  '猫 会 动 吗' '狗 会 动 吗' '老鼠 会 动 吗' '鱼 会 动 吗' '鸟 会 动 吗' \
  '猫 有 心 吗' '狗 有 眼睛 吗' '鱼 有 毛 吗' ; do
    ( cd "$WORK" && cp model.bin m.bin )
    out=$(cd "$WORK" && printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n::rel analogy solver off\n::wgate on\n%s\n::wgate\n::quit\n' "$q" \
          | ./aip --model m.bin 2>&1)
    imp=$(printf '%s\n' "$out" | sed -n 's/.*最近一次关系写入的重要度：\([0-9.]*\).*/\1/p' | tail -1)
    ev=$(printf '%s\n' "$out" | sed -n 's/.*那条候选自己的证据：链 \([0-9]*\) 跳 \/ 链底 \([0-9]*\) 分.*/\1| \2/p' | tail -1)
    n_tot=$((n_tot + 1))
    [ -n "$imp" ] && n_imp=$((n_imp + 1))
    printf '%s\t%s\t%s\n' "$q" "${imp:-NA}" "${ev:-?}" >> "$OUT"
done

echo "样本 $n_tot 条，其中闸门真评过的 $n_imp 条 → $OUT"
awk -F'\t' '$2 != "NA" { print $2 }' "$OUT" | sort -g \
| awk '{ v[NR]=$1; s+=$1 } END{ if (NR==0) { print "  （没有样本）"; exit }
        m=s/NR; for (i=1;i<=NR;i++) var+=(v[i]-m)^2
        printf "  均值 %.3f  标准差 %.4f  最小 %.2f  p25 %.2f  中位 %.2f  p75 %.2f  最大 %.2f\n",
               m, sqrt(var/(NR-1)), v[1], v[int(NR/4)+1], v[int(NR/2)+1], v[int(NR*3/4)+1], v[NR] }'
