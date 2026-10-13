#!/bin/sh
# tests/embed.sh — L3「神经表示」（emb）的回归
#
# 看护这条能力：模型里现在有一份**学出来的**分布式表示（PPMI 随机索引），
# 而不是只有「名字的随机投影」。
#
# 断言分两类，第二类是重点：
#   ① 能力本身：emb 有值、无关词对≈0、相关词对显著高于随机
#   ② **它的边界**：emb 撑不起细粒度分类、撑不起关系类比 ——
#      这两条也要钉住，免得以后有人误以为可以拿它当槽位门槛用
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
PROBE=${PROBE:-./build_slotprobe}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -x "$PROBE" ] || { echo "FAIL: 缺 $PROBE（先 gcc -O2 -o build_slotprobe tools/slotprobe.c -lm）"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }

TMP=./_emb_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi }

neuro=$($PROBE "$MODEL" neuro)
# hv 段和 emb 段里都有「猫 ↔ 狗」这样的行，必须**分段**找，
# 否则会拿到 hv 的那个数（第一次写这个测试就踩了）。
hvsec=$(printf '%s\n' "$neuro" | sed -n '/② VSA/,/③ hv/p')
embsec=$(printf '%s\n' "$neuro" | sed -n '/④ emb/,$p')
pct() {   # $1=段落 $2=词A $3=词B
    printf '%s\n' "$1" | sed -n "s/.*$2 *↔ *$3 *\([-0-9.]*\)%.*/\1/p" | head -1
}

echo "① emb 存在，且相关/无关分得开"
nz=$(printf '%s\n' "$neuro" | sed -n 's/.*emb（PPMI 随机索引，1024 维）非零的 Token = \([0-9]*\).*/\1/p')
[ -n "$nz" ] && [ "$nz" -ge 500 ] && chk "有 emb 的 Token = $nz（≥500）" 1 \
                                 || chk "有 emb 的 Token 太少（${nz:-?}）" 0
cat_dog=$(pct "$embsec" 猫 狗)
cat_q=$(pct "$embsec" 猫 量子)
hv_cat_dog=$(pct "$hvsec" 猫 狗)
echo "     猫↔狗 emb=$cat_dog%  hv=$hv_cat_dog%   猫↔量子 emb=$cat_q%"
# 相关对（猫↔狗）要明显高
awk -v a="$cat_dog" 'BEGIN{exit !(a+0 >= 40)}' && chk "猫↔狗 的 emb 余弦 ≥40%（实得 $cat_dog%）" 1 \
                                                 || chk "猫↔狗 的 emb 余弦 ≥40%（实得 $cat_dog%）" 0
# 无关对（猫↔量子）要接近 0
awk -v a="$cat_q" 'BEGIN{exit !(a+0 <= 5)}' && chk "猫↔量子 的 emb 余弦 ≤5%（实得 $cat_q%）" 1 \
                                            || chk "猫↔量子 的 emb 余弦 ≤5%（实得 $cat_q%）" 0
# 旧 hv 对同一对词基本没区分度（这是当初做 L3 的理由，一起钉住）
awk -v a="$hv_cat_dog" 'BEGIN{exit !(a+0 <= 20)}' && chk "旧 hv 对同一对词仍无区分度（$hv_cat_dog%）" 1 \
                                                  || chk "旧 hv 对同一对词无区分度（$hv_cat_dog%）" 0

echo "② emb 不写进模型文件，读盘后重算 == 原地算"
{ printf '::load %s\n::char off\n::near 猫\n::save %s/m.bin\n::load %s/m.bin\n::char off\n::near 猫\n::quit\n' \
      "$MODEL" "$TMP" "$TMP"; } | "$BIN" 2>&1 | sed -n 's/^   \(..*\) *\([0-9.]*\)%$/\1 \2/p' > "$TMP/near.txt"
"$BIN" > /dev/null 2>&1 || true
# 前后各 8 行，必须逐字一致
head -8 "$TMP/near.txt" > "$TMP/a.txt"
tail -8 "$TMP/near.txt" > "$TMP/b.txt"
if [ -s "$TMP/a.txt" ] && cmp -s "$TMP/a.txt" "$TMP/b.txt"; then
    chk "读盘前/后的语义邻居逐字一致（emb 是确定性函数）" 1
else
    chk "读盘前/后的语义邻居逐字一致" 0
    echo "     前:"; sed 's/^/       /' "$TMP/a.txt"
    echo "     后:"; sed 's/^/       /' "$TMP/b.txt"
fi

echo "③ 边界：emb 撑不起细粒度分类（所以不能当槽位门槛）"
# 判据不能写死一个数（加了关系知识之后这个数会漂）：真正要钉住的是
# 「**类外词**也能拿到类内量级的一半以上」—— 那就没有可用的阈值。
cls=$($PROBE "$MODEL" emb 饭,面,米饭,菜,水果,苹果,茶,咖啡,汤,肉,鱼,面包,粥 老师 狗 树 猫)
out_max=$(printf '%s\n' "$cls" | sed -n 's/^  [^ ]* .*最相似= *\([0-9.]*\)%.*/\1/p' | sort -rn | head -1)
echo "     食物类的类外词最高分 = ${out_max}%（类内词自己是 100%）"
awk -v a="$out_max" 'BEGIN{exit !(a+0 >= 50)}' \
    && chk "类外词也能拿到 ${out_max}% —— 没有可用的阈值，不能用来放行槽位" 1 \
    || chk "类外词最高只有 ${out_max}% —— 那也许可以当门槛，需重新评估" 0

echo "④ 边界：关系类比撑不起来时会**诚实拒答**，不是编一个"
{ printf '::load %s\n::char off\n::analogy 猫 动物 狗\n::quit\n' "$MODEL"; } | "$BIN" 2>&1 \
    | grep -q '推不出来' \
    && chk "::analogy 在表示不够时不编答案" 1 || chk "::analogy 该拒答时却给了答案" 0

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS embed（L3：emb 真学出来了；同时钉住它撑不起细粒度分类与关系类比）" \
                  || { echo "FAIL embed"; exit 1; }
