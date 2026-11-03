#!/bin/sh
# tests/wdcorpus.sh — **公开语料（Wikidata NT）转三元组**是对的（§6.712）
#
# 三件事：
#   ① NT → 三元组：实体-实体事实留下、**字面量/值跳过**（并报数，不静默丢）；
#   ② 进图能查：`::ingest` 之后 `::rel 实体:Q31` 能看到事实；
#   ③ 空输入必须报 FAIL（不许"成功转了 0 条"）。
# 夹具在 `tests/fixtures/wd_sample.nt`（5 行：2 条实体-实体、1 条字面量、2 条本体头）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/wdcorpus"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }

pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

echo "== ① NT → 三元组（字面量跳过要报数）=="
sh "$ROOT/tools/wikidata2triples.sh" "$ROOT/tests/fixtures/wd_sample.nt" "$WORK/wd.triples" > "$WORK/conv.log" 2>&1
N=$(wc -l < "$WORK/wd.triples")
if [ "$N" -eq 3 ]; then
    ok "5 行 NT ⇒ 3 条实体-实体事实（字面量 1 条跳过、本体头 1 条不认识）"
else
    bad "转换条数不对：$N（应为 3）" "$(cat "$WORK/conv.log" | tr '\n' ' ')"
fi
grep -qa '实体-实体事实' "$WORK/conv.log" && ok "丢了多少条有报数（$(grep -a '实体-实体事实' "$WORK/conv.log" | sed 's/^ *//' | cut -c1-80)）" || bad "没有报"字面量跳过"的数" ""
grep -qa '实体:Q31	属于	实体:Q3624078' "$WORK/wd.triples" && ok "P31 映射成中文关系「属于」" || bad "P31 没映射" "$(head -3 "$WORK/wd.triples" | tr '\n' ' ')"
grep -qa '实体:Q31	在	实体:Q55' "$WORK/wd.triples" && ok "P17 映射成「在」" || bad "P17 没映射" ""

echo "== ② 进图能查 =="
printf '::ingest %s\n::save %s/wd_model.bin\n::quit\n' "$WORK/wd.triples" "$WORK" | ./build_aip --model m.bin > ing.log 2>&1
grep -qa '写进去 [1-9]' ing.log && ok "::ingest 写进去了（$(grep -a '导入' ing.log | head -1 | sed 's/.*：//')" || bad "::ingest 没写进去" "$(grep -a '导入' ing.log | head -1)"
printf '::rel 实体:Q31\n::quit\n' | ./build_aip --model wd_model.bin 2>&1 | grep -qa -- '实体:Q31 --\[属于\]--> 实体:Q3624078' \
    && ok "::rel 实体:Q31 查得到「属于 实体:Q3624078」" || bad "查不到事实" ""

echo "== ②b v2（节点名 + 每片属性表）：绕开引擎 64 个关系上限 =="
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o "$WORK/wd2c7b" "$ROOT/tools/wd2c7b.c" -lm 2> wd2c7b.err \
    || bad "wd2c7b 编不过" "$(head -2 wd2c7b.err | tr '\n' ' ')"
if [ -x "$WORK/wd2c7b" ]; then
    ./wd2c7b "$WORK/wd.c7b2" < "$ROOT/tests/fixtures/wd_sample.nt" > v2.log 2>&1 \
        || bad "v2 转换失败" "$(tail -2 v2.log | tr '\n' ' ')"
    if [ -s "$WORK/wd.c7b2" ] && ./wd2c7b --probe "$WORK/wd.c7b2" Q31 > v2probe.log 2>&1; then
        ok "v2 分片：$(grep -a 'v2 分片' v2.log | sed 's/^ *//')"
        grep -qa -- 'Q31 --\[P31\]--> Q3624078' v2probe.log \
            && ok "probe 打出真事实：Q31 --[P31]--> Q3624078（属性名在**片里**，不占引擎关系表）" \
            || bad "probe 没打出 P31 事实" "$(tail -2 v2probe.log | tr '\n' ' ')"
        grep -qa '✔' v2probe.log && ok "v2 头/长度自洽（✔）" || bad "v2 长度校验不过" "$(head -2 v2probe.log | tr '\n' ' ')"
    else
        bad "v2 probe 失败" "$(tail -2 v2.log | tr '\n' ' ')"
    fi
fi

echo "== ③ 空输入必须报 FAIL =="
: > empty.nt
if sh "$ROOT/tools/wikidata2triples.sh" "$WORK/empty.nt" "$WORK/empty.triples" > empty.log 2>&1; then
    bad "空输入居然返回成功" "$(tail -2 empty.log | tr '\n' ' ')"
else
    ok "空输入返回失败（说清了原因：$(grep -a 'FAIL' empty.log | head -1 | cut -c1-60)）"
fi

echo
printf '公开语料转换：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS wdcorpus（NT→三元组 + 字面量跳过 + 进图可查 + 空输入报错）\n' || printf 'FAIL wdcorpus\n'
[ "$fail" -eq 0 ]
