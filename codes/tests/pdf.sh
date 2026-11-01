#!/bin/sh
# tests/pdf.sh — **PDF 取文字**（§6.678/§6.679）：自带 inflate + 取文字 + 一路到待证
#
# 判据分两层：
#   ① 两个探针（inflate / pdf）**全部项目通过**（它们自己会打印「结果：N 项通过，0 项失败」）；
#   ② CLI 长链：`::paper read x.pdf` → 抽断言 ⇒ 待证 → 用户确认 → 事实层能答对。
# 全程**离线**：夹具是本仓库 tests/fixtures 下自己造的小 PDF（字节入库，python 脚本留档）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/pdf"
rm -rf "$WORK"; mkdir -p "$WORK"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

echo "== ① 探针：inflate（自带 DEFLATE/zlib） =="
if gcc -std=c11 -O2 -Wall -Wextra -Werror -o "$WORK/build_inflateprobe" "$ROOT/tools/inflateprobe.c" -lm 2> "$WORK/infbuild.log"; then
    ok "inflateprobe 编译过（-Wall -Wextra -Werror）"
    if ( cd "$ROOT" && "$WORK/build_inflateprobe" tests/fixtures/inflate > "$WORK/inf.log" 2>&1 ); then
        tail -1 "$WORK/inf.log" | sed 's/^/         /'
        ok "inflateprobe 全部通过"
    else
        bad "inflateprobe 有失败项" "$(tail -3 "$WORK/inf.log" | tr '\n' ' ')"
    fi
else
    bad "inflateprobe 编译不过" "$(tail -3 "$WORK/infbuild.log" | tr '\n' ' ')"
fi

echo "== ② 探针：PDF 取文字 =="
if gcc -std=c11 -O2 -Wall -Wextra -Werror -o "$WORK/build_pdfprobe" "$ROOT/tools/pdfprobe.c" -lm 2> "$WORK/pdfbuild.log"; then
    ok "pdfprobe 编译过"
    if ( cd "$ROOT" && "$WORK/build_pdfprobe" > "$WORK/pdf.log" 2>&1 ); then
        tail -1 "$WORK/pdf.log" | sed 's/^/         /'
        ok "pdfprobe 全部通过"
    else
        bad "pdfprobe 有失败项" "$(tail -3 "$WORK/pdf.log" | tr '\n' ' ')"
    fi
else
    bad "pdfprobe 编译不过" "$(tail -3 "$WORK/pdfbuild.log" | tr '\n' ' ')"
fi

# 夹具必须存在（否则上面两条会"因为读不到夹具"而失败，看不出根因）
for f in simple.pdf flate.pdf unicode.pdf broken.pdf encrypted.pdf; do
    [ -s "$ROOT/tests/fixtures/pdf/$f" ] || bad "缺夹具 $f" "跑 python3 tests/fixtures/mkfix.py 重新生成"
done

echo "== ③ CLI：PDF → 断言 → 待证 → 确认 → 事实层 =="
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || bad "缺 model.bin（整包会先建；单跑要自己准备）" ""
cp "$ROOT/relations.txt" . 2>/dev/null || true
printf '::paper read %s\n::claim\n::quit\n' "$ROOT/tests/fixtures/pdf/unicode.pdf" \
    | timeout 120 ./build_aip --model m.bin > out1.txt 2>&1 || true
grep -qaF '企鹅 属于 鸟类' out1.txt && grep -qaF '企鹅 会 游泳' out1.txt \
    && ok "PDF 里的中文断言被抽出来（ToUnicode 那条路通了）" \
    || bad "抽断言" "$(grep -a 'PDF' out1.txt | head -1)"
grep -qaE '记为待证 [1-9]' out1.txt \
    && ok "记成待证（不是事实）" \
    || bad "待证" "$(grep -a '待证' out1.txt | head -1)"
grep -qaF '（来源：论文' out1.txt \
    && ok "来源记成「论文」（和 ::paper 一条链）" \
    || bad "来源" "$(grep -a '待证' out1.txt | sed -n 2p)"

printf '::paper read %s\n::claim accept 1\n企鹅 属于 鸟类 吗\n::quit\n' \
    "$ROOT/tests/fixtures/pdf/unicode.pdf" | timeout 120 ./build_aip --model m.bin > out2.txt 2>&1 || true
grep -qaF '按你说的算数' out2.txt \
    && ok "确认后升格成事实" \
    || bad "确认" "$(grep -a '待证' out2.txt | head -1)"
grep -qaE '对 ， 企鹅 -\[属于\]-> 鸟类' out2.txt \
    && ok "升格之后**真答对**（走的是事实边）" \
    || bad "答对" "$(grep -a 'AI >' out2.txt | tail -1)"

echo "== ④ 坏 PDF 不许「假装读到」 =="
printf '::paper read %s\n::quit\n' "$ROOT/tests/fixtures/pdf/broken.pdf" \
    | timeout 120 ./build_aip --model m.bin > out3.txt 2>&1 || true
grep -qaE '跳过 1 个流|跳过 [1-9]' out3.txt \
    && ok "broken.pdf：明说跳过了流" \
    || bad "跳过计数" "$(grep -a 'PDF' out3.txt | head -1)"
grep -qaE '一个字都没读出来|没抽出文字|一个三段式断言都没抽到|不是 PDF' out3.txt \
    && ok "broken.pdf：明说没抽到（没有编造内容）" \
    || bad "没抽到" "$(grep -a 'PDF' out3.txt | head -1)"

echo "== ⑤ 外部裁判（有 pdftotext 就逐词比一遍；没有就跳过） =="
if command -v pdftotext > /dev/null 2>&1; then
    pdftotext "$ROOT/tests/fixtures/pdf/flate.pdf" "$WORK/oracle.txt" 2>/dev/null || true
    printf '::paper text %s\n::quit\n' "$ROOT/tests/fixtures/pdf/flate.pdf" \
        | timeout 120 ./build_aip --model m.bin > "$WORK/mine.txt" 2>&1 || true
    # oracle 是 pdftotext 的整份输出；我这边只取「取到的文字」那一段（前面还有说明行）
    tok() { tr -cs 'A-Za-z' '\n' | tr 'A-Z' 'a-z' | sed '/^$/d' | sort -u; }
    tok < "$WORK/oracle.txt" > "$WORK/w_oracle.txt"
    sed -n '/取到的文字/,/---- 完 ----/p' "$WORK/mine.txt" | tok > "$WORK/w_mine.txt"
    miss=$(comm -23 "$WORK/w_oracle.txt" "$WORK/w_mine.txt" | wc -l | tr -d ' ')
    tot=$(wc -l < "$WORK/w_oracle.txt" | tr -d ' ')
    if [ "${tot:-0}" -gt 0 ] && [ "${miss:-1}" -eq 0 ]; then
        ok "pdftotext 认这两份夹具，且它抽出的 $tot 个词**一个不少**地出现在我这边"
    else
        bad "和 pdftotext 的逐词比对差 $miss/$tot" \
            "$(comm -23 "$WORK/w_oracle.txt" "$WORK/w_mine.txt" | head -5 | tr '\n' ' ')"
    fi
else
    printf '  [SKIP] 没有 pdftotext（外部裁判缺席；内部探针已经跑过）\n'
fi

echo
printf 'PDF 取文字：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS pdf（inflate + 取文字 + 待证 + 坏文件不撒谎）\n' || printf 'FAIL pdf\n'
[ "$fail" -eq 0 ]
