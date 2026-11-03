#!/bin/sh
# tests/fragknow.sh — **.c 片段知识是好的**（阶段四扩图 §6.710）
#
# 三件事，缺一条这条扩图就是"堆垃圾"：
#   ① **抽得出来**：`tools/c2triples.sh` 在夹具上抽出函数片段（`main` 不算片段）；
#   ② **门挡得住**：用了不存在类型的那个函数**必须**被编译门挡在图外（不许"看起来像代码"就收）；
#   ③ **查得到、能编过**：`::code frag <关键词>` 能查到；抽出来的 `代码` 单独 gcc 编得过。
#
# 夹具在 `tests/fixtures/c2src/`：`util.c`（两个自包含函数 + main）、`broken.c`（不存在的类型名）。
# 用法：sh tests/fragknow.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/fragknow"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }

pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

echo "== ① 抽取 + 编译门（夹具：2 个能编、1 个坏）=="
sh "$ROOT/tools/c2triples.sh" "$WORK/frag.triples" "$ROOT/tests/fixtures/c2src" > "$WORK/extract.log" 2>&1
grep -a '抽到片段' "$WORK/extract.log" | head -1 | sed 's/^/  /'
[ -s "$WORK/frag.triples" ] || { bad "抽取没产出" "$(tail -3 "$WORK/extract.log" | tr '\n' ' ')"; }

NFRAG=$(grep -c '	签名	' "$WORK/frag.triples" || true)
NFRAG=${NFRAG:-0}
if [ "$NFRAG" -eq 2 ]; then
    ok "抽到 2 个片段（main 不算、坏的那个被门挡住）"
else
    bad "片段数不对：$NFRAG（应为 2）" "$(grep -a '	签名	' "$WORK/frag.triples" | cut -f1,3 | tr '\n' ' ')"
fi
if grep -qa 'broken_make_thing' "$WORK/frag.triples"; then
    bad "编译门没挡住「用了不存在类型」的那个片段" "grep broken_make_thing 有命中"
else
    ok "坏片段（不存在的类型名）被编译门挡在图外"
fi
if [ "$(grep -c '	独立	是' "$WORK/frag.triples" || true)" -eq 2 ]; then
    ok "两个片段都标了「独立 是」（只用常用头就能编过）"
else
    bad "独立标记不对" "$(grep -a '	独立	' "$WORK/frag.triples" | tr '\n' ' ')"
fi

echo "== ② 灌图 + 查得到（::code frag）=="
printf '::ingest %s\n::save %s/frag_model.bin\n::quit\n' "$WORK/frag.triples" "$WORK" | ./build_aip --model m.bin > ing.log 2>&1
grep -qa '写进去 [1-9][0-9]* 条' ing.log && ok "::ingest 写进去了（$(grep -a '导入' ing.log | head -1 | sed 's/.*：//')" || bad "::ingest 没写进去" "$(grep -a '导入' ing.log | head -1)"
q() { printf '%s\n::quit\n' "$2" | ./build_aip --model frag_model.bin 2>&1 | grep -a '\[片段\]' | tail -1; }
if q . '::code frag' | grep -qa '图里共 2 个片段'; then
    ok "::code frag 报得出总数（2 个片段）"
else
    bad "::code frag 的总数不对" "$(q . '::code frag')"
fi
if q . '::code frag clamp' | grep -qa '命中 1 个'; then
    ok "::code frag clamp 命中 1 个（按片段名/签名子串）"
else
    bad "::code frag clamp 没命中" "$(q . '::code frag clamp')"
fi
if q . '::code frag 绝对不存在的关键词' | grep -qa '没有匹配'; then
    ok "::code frag <无关键词> 照实说「没有匹配」"
else
    bad "查不到时没照实说" "$(q . '::code frag 绝对不存在的关键词')"
fi
printf '::code frag clamp\n::quit\n' | ./build_aip --model frag_model.bin 2>&1 | grep -qa '片段:util_clamp_int' && ok "命中的是夹具里那个自包含片段" || bad "命中项不对" ""

echo "== ③ 抽出来的代码**单独能编过**（和合成器的编译参数一致）=="
awk -F'\t' 'BEGIN {
        print "#include <stdio.h>"; print "#include <stdlib.h>"; print "#include <string.h>"
        want = 0
    }
    /	签名	/ { want = ($1 == "片段:util_clamp_int") }
    want && /	代码	/ { b = $3; sub(/\\$/, "", b); print b; next }
    want && $0 !~ /^片段:/ { b = $0; sub(/\\$/, "", b); print b }' "$WORK/frag.triples" > one.c
# ⚠ 先确认真的抽到了那个函数：不然"只有前导头"也能编过，③ 就是个假绿（第一版差点这样）
grep -qa 'clamp_int' one.c || bad "没从 triples 里抽出片段代码（③ 会是假绿）" "one.c 里没有 clamp_int"
gcc -std=c11 -Wall -Werror=implicit-function-declaration -fsyntax-only one.c > one.err 2>&1
if [ $? -eq 0 ] && grep -qa 'clamp_int' one.c; then
    ok "片段代码 + 常用头 ⇒ gcc -fsyntax-only 过（$(wc -l < one.c) 行）"
else
    bad "抽出来的片段单独编不过" "$(head -2 one.err | tr '\n' ' ')"
fi

echo
printf '片段知识：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS fragknow（抽取 + 编译门 + ::code frag + 代码能编）\n' || printf 'FAIL fragknow\n'
[ "$fail" -eq 0 ]
