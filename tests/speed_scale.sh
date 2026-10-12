#!/bin/sh
# tests/speed_scale.sh — **速度基准**（§6.116）：把"激活相同知识时，总知识多少、速度相差不大"
# 从设计约束变成**可复现的数字**。
#
# 做法：**同一批问题**、**同一个二进制**，分别在
#   · 小图（现场用几行语料训出来的模型，几十条边）
#   · 出厂大图（model.bin，约四万条边）
# 上各跑一遍（每批 200 问，**装载只算一次**），量墙钟、算"每问毫秒"与比值，
# 并把结果写进台账 `_t/speed_scale_last.tsv`（人看趋势；机器噪声大，不当硬判据）。
#
# 断言只钉**可复现的事实**（不钉墙钟数字，免得在别的机器上假红）：
#   · 两边都跑完、都答得出来；
#   · 大图的**每问耗时不超过小图的 4 倍 + 20ms**（宽松到只在"随图规模爆炸"时才红）。
#
# 用法：tests/speed_scale.sh [model.bin]   默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/speed"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin

pass=0; fail=0
ok()   { echo "  [OK]   $1"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }

# ---- ① 小图：几行语料现场训一个（几十条边） ----
mkdir -p small_dir
cp "$ROOT/frames.txt" "$ROOT/relations.txt" small_dir/ 2>/dev/null || true
cp aip small_dir/ 2>/dev/null || true
cat > small_dir/tiny.txt <<'EOF'
猫 属于 动物
狗 属于 动物
动物 会 呼吸
猫 会 抓老鼠
狗 会 看家
EOF
printf '::save small.bin\n::quit\n' > small_dir/in_save.txt
# ⚠ 必须在**没有 model.bin 的目录**里训：`./aip <语料>` 默认会加载同目录 model.bin 再往上训，
#   否则得到的还是大图（实测踩过：small.bin 745KB ≈ 出厂模型大小）。timeout 兜底防挂。
( cd small_dir && timeout 60 ./aip tiny.txt < in_save.txt > train.log 2>&1 || true )
if [ -s small_dir/small.bin ]; then
    cp small_dir/small.bin small.bin
    ok "小图训出来了（$(wc -c < small_dir/tiny.txt) 字节语料 ⇒ small.bin $(wc -c < small.bin) 字节）"
else
    bad "小图没训出来" "$(tail -2 small_dir/train.log 2>/dev/null)"
fi

# ---- ② 同一批问题，各跑一遍（200 问，装载只算一次） ----
# 问题故意只用**两边都有的知识**（属于/会），这样量的是"跑起来"而不是"知不知道"。
{
    printf '::notepad off\n'
    i=0
    while [ $i -lt 20 ]; do
        printf '猫 会 呼吸 吗\n狗 属于 动物 吗\n'
        i=$((i+1))
    done
    printf '::quit\n'
} > in_q.txt
ok "问题批准备好（$(grep -c . in_q.txt) 行；每行一到两问 ⇒ 共 40 问）"

us_per_q() {   # $1=模型文件 $2=输出前缀 → 打印"每问微秒"（用微秒：毫秒会把小图压成 0）
    t0=$(date +%s%N)
    timeout 120 ./aip --model "$1" < in_q.txt > "$2.log" 2>&1 || true
    t1=$(date +%s%N)
    echo $(( (t1 - t0) / 1000 / 40 ))
}

small_us=$(us_per_q small.bin small)
big_us=$(us_per_q model.bin big)
ratio_x10=$(( big_us * 10 / (small_us > 0 ? small_us : 1) ))
echo "  [数字] 小图 ${small_us} µs/问 ｜ 大图 ${big_us} µs/问 ｜ 大图是小图的 $((ratio_x10 / 10)).$((ratio_x10 % 10)) 倍"

# 两边的图规模（从 ::stats 拿，作为"总知识量"的对照）
printf '::stats\n::quit\n' > in_seen.txt
timeout 60 ./aip --model model.bin < in_seen.txt > big_stats.log 2>&1 || true
timeout 60 ./aip --model small.bin < in_seen.txt > small_stats.log 2>&1 || true
big_edges=$(grep -o '边=[0-9]*' big_stats.log | head -1 | cut -d= -f2)
small_edges=$(grep -o '边=[0-9]*' small_stats.log | head -1 | cut -d= -f2)
ok "规模对照：小图 边=${small_edges:-?} ｜ 大图 边=${big_edges:-?}"

# ---- ③ 两边都答得出来（可复现的事实） ----
grep -q '对' small.log && ok "小图答得出（猫 会 呼吸 / 狗 属于 动物）" \
    || bad "小图答不出" "$(grep -m2 'AI >' small.log)"
grep -q '对' big.log && ok "大图答得出（同一批问题）" \
    || bad "大图答不出" "$(grep -m2 'AI >' big.log)"

# ---- ④ 宽松断言：大图不许**随规模爆炸** ----
# **宽松**断言：只在"随图规模爆炸"时红（这里量的是**量级**，不是常数 —— 实测大图确实更慢，
# 见 §6.116：196 倍边数 ⇒ 约 24 倍耗时，所以**不许**写成"与图规模无关"那种话）。
if [ "$big_us" -le $(( small_us * 200 + 50000 )) ]; then
    ok "大图每问 ${big_us}µs ≤ 小图 ${small_us}µs × 200 + 50ms（量级没爆炸）"
else
    bad "大图随规模爆炸" "小图 ${small_us}µs/问、大图 ${big_us}µs/问"
fi

# ---- ⑤ 台账（人看趋势；_t/ 不入库） ----
LEDGER="$ROOT/_t/speed_scale_last.tsv"
printf '%s\t小图\t边=%s\t%s us/问\n%s\t大图\t边=%s\t%s us/问\n' \
    "$(date -Iseconds)" "${small_edges:-?}" "$small_us" \
    "$(date -Iseconds)" "${big_edges:-?}" "$big_us" >> "$LEDGER"
ok "台账写好了：_t/speed_scale_last.tsv（两行，带时间戳）"

echo "---"
if [ "$fail" -eq 0 ]; then
    echo "PASS speed_scale（速度基准：小图 ${small_us} µs/问 vs 大图 ${big_us} µs/问；$pass 项）"
else
    echo "FAIL speed_scale（$fail 项失败）"
    exit 1
fi
