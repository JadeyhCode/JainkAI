#!/bin/sh
# tests/pdf_real.sh — **真论文**的逐词对照（手动跑，不进整包；需要 pdf 文件 + pdftotext）
#
# 为什么要它：夹具是"我自己造的 PDF"，只能证明"能读我这种写法"。真论文里有 Type1/TrueType
# 字体、按字体的 ToUnicode、多内容流、对象流、巨大图片流 —— 那才是它该干的活。
#
# 用法：sh tests/pdf_real.sh [/path/to/paper.pdf]
#   不给参数 = 跑**夹具**（`tests/fixtures/pdf/flate.pdf`，召回必须是 100% ⇒ 这条能进整包）；
#   给真论文 = 手动模式，门槛仍是「召回 ≥ 90%」但数字会低于 100%（断词/上下标，见 docs/联网与PDF.md）；
#   · 有 pdftotext 就用它当**外部裁判**；没有就 SKIP（自己跟自己比没有意义，所以明说裁判缺席）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PDF=${1:-$ROOT/tests/fixtures/pdf/flate.pdf}
[ -s "$PDF" ] || { echo "用法：sh tests/pdf_real.sh [pdf 文件]（不给就跑夹具）"; exit 2; }
WORK="$ROOT/_t/pdf_real"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" . ; cp "$ROOT/model.bin" m.bin 2>/dev/null || true
cp "$ROOT/relations.txt" . 2>/dev/null || true

printf '::paper text %s 1000000\n::quit\n' "$PDF" | timeout 300 ./build_aip --model m.bin > mine.txt 2>&1 || true
sed -n '/取到的文字/,/---- 完 ----/p' mine.txt > mine_body.txt
wc -c mine_body.txt | sed 's/^/  我抽到的文字字节：/'
grep -a 'PDF：' mine.txt | head -1 | sed 's/^/  /'

if ! command -v pdftotext > /dev/null 2>&1; then
    echo "  [SKIP] 本机没有 pdftotext：外部裁判缺席，不做重合率（自己做自己的裁判没有意义）"
    echo "PASS pdf_real（SKIP：没有外部裁判）"
    exit 0
fi
pdftotext "$PDF" oracle.txt 2>/dev/null || true
tok() { tr -cs 'A-Za-z' '\n' | tr 'A-Z' 'a-z' | sed '/^$/d' | sort -u; }
tok < oracle.txt > w_o.txt
tok < mine_body.txt > w_m.txt
NO=$(wc -l < w_o.txt | tr -d ' ')
NM=$(wc -l < w_m.txt | tr -d ' ')
MISS=$(comm -23 w_o.txt w_m.txt | wc -l | tr -d ' ')
EXTRA=$(comm -13 w_o.txt w_m.txt | wc -l | tr -d ' ')
echo "  词的种类：pdftotext $NO 个 ／ 我 $NM 个"
echo "  pdftotext 有、我没有：$MISS 个"
echo "  我有、pdftotext 没有：$EXTRA 个"
RC=0
if [ "${NO:-0}" -gt 0 ]; then
    awk -v m="$MISS" -v n="$NO" 'BEGIN{printf "  召回率 %.1f%%%s\n", 100.0*(n-m)/n, (m>0 ? "（差的多半是断词与上下标）" : "")}'
    # 门槛：夹具必须 100%（自造的小 PDF 没有断词/上下标）；真论文 ≥ 90%
    if [ "$MISS" -gt 0 ] && [ "${NO:-0}" -lt 100 ]; then
        echo "  [FAIL] 夹具都做不到逐词对齐（差 $MISS 个）"; RC=1
    elif [ "${NO:-0}" -ge 100 ]; then
        awk -v m="$MISS" -v n="$NO" 'BEGIN{exit !(100.0*(n-m)/n >= 90.0)}' || { echo "  [FAIL] 召回率低于 90%"; RC=1; }
        [ "$RC" -eq 0 ] && echo "  [OK]   召回率 ≥ 90%"
    else
        echo "  [OK]   夹具逐词对齐（差 0 个）"
    fi
fi
echo "  差的前 20 个（pdftotext 有、我没有）："
comm -23 w_o.txt w_m.txt | head -20 | tr '\n' ' ' | sed 's/^/    /'
echo
echo "  我有、pdftotext 没有的前 20 个："
comm -13 w_o.txt w_m.txt | head -20 | tr '\n' ' ' | sed 's/^/    /'
echo
[ "$RC" -eq 0 ] && echo "PASS pdf_real" || echo "FAIL pdf_real"
exit "$RC"
