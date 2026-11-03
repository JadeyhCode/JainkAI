#!/bin/sh
# tests/corpus7b.sh — **7B 分片语料（.c7b）是可用的**（阶段四扩图 §6.711）
#
# 五件事，缺一条"7B"就只是堆文件：
#   ① 生成器**确定性**：同参数跑两次，两份文件逐字节相同（换机器/重跑能复现）；
#   ② 每片**自洽**：索引合计 == 头里的边数、文件长度 == 头+索引+边×8、段内**无重复边**；
#   ③ 断点续跑：已存在的完整片**跳过**，清单写全量且 `status COMPLETE`；
#   ④ 引擎能**挂上并查**：`::parts corpus <片>` 只读头；`::parts corpus rel w000042` 按需读那一段；
#   ⑤ 坏文件**拒**：截断的、非 C7B 的都必须打不开（不猜、不静默）。
#
# 用**小片**（5 万词 / 20 万边 = 2.4 MB），秒级；不是拿 7B 全量跑。
# 用法：sh tests/corpus7b.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/corpus7b"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }

pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

NW=50000; NE=200000
echo "== ① 生成器确定性（$NW 词 / $NE 边，两次逐字节比）=="
sh "$ROOT/tools/gen_corpus7b.sh" "$NE" "$NE" "$NW" "$WORK/a" 7 > "$WORK/gen_a.log" 2>&1 \
    || bad "生成 A 失败" "$(tail -3 "$WORK/gen_a.log" | tr '\n' ' ')"
sh "$ROOT/tools/gen_corpus7b.sh" "$NE" "$NE" "$NW" "$WORK/b" 7 > "$WORK/gen_b.log" 2>&1 \
    || bad "生成 B 失败" "$(tail -3 "$WORK/gen_b.log" | tr '\n' ' ')"
A="$WORK/a/shard_001.c7b"; B="$WORK/b/shard_001.c7b"
if [ -s "$A" ] && [ -s "$B" ] && cmp -s "$A" "$B"; then
    ok "同参数两次生成**逐字节相同**（$(wc -c < "$A") 字节）"
else
    bad "两次生成不一致" "a=$(wc -c < "$A" 2>/dev/null || echo 缺) b=$(wc -c < "$B" 2>/dev/null || echo 缺)"
fi

echo "== ② 每片自洽（索引合计 / 文件长度 / 段内无重复）=="
"$ROOT/_t/c7b/mkcorpus7b" --probe "$A" > "$WORK/probe.log" 2>&1
if [ $? -eq 0 ] && grep -qa '✔' "$WORK/probe.log"; then
    ok "probe 通过：$(sed -n '2p' "$WORK/probe.log" | sed 's/^  //')"
else
    bad "probe 校验不过" "$(head -3 "$WORK/probe.log" | tr '\n' ' ')"
fi
if "$ROOT/_t/c7b/mkcorpus7b" --probe "$A" w000042 > "$WORK/probe_w.log" 2>&1; then
    ok "整段读回无重复边：$(grep -a '重复' "$WORK/probe_w.log" | sed 's/^  //' | cut -c1-60)"
else
    bad "段内有重复边（或读失败）" "$(tail -2 "$WORK/probe_w.log" | tr '\n' ' ')"
fi

echo "== ③ 断点续跑（同参数再跑一次 ⇒ 全都跳过）=="
if sh "$ROOT/tools/gen_corpus7b.sh" "$NE" "$NE" "$NW" "$WORK/a" 7 > "$WORK/gen_a2.log" 2>&1; then
    if grep -qa '已用 0 s' "$WORK/gen_a2.log" && grep -q '^status COMPLETE' "$WORK/a/manifest.txt"; then
        ok "重跑同一目标：0 s 跳过，清单 status COMPLETE（总边数 $(sed -n 's/^total_edges //p' "$WORK/a/manifest.txt")）"
    else
        bad "重跑没有跳过" "$(grep -a '已用' "$WORK/gen_a2.log" | tail -1)"
    fi
else
    bad "重跑失败" "$(tail -3 "$WORK/gen_a2.log" | tr '\n' ' ')"
fi

echo "== ④ 引擎挂上并查（只读头 + 按需读段）=="
printf '::parts corpus %s\n::parts corpus rel w000042\n::parts corpus stats\n::quit\n' "$A" > q.in
./build_aip --model m.bin < q.in > q.out 2>&1
grep -qa "挂上 $A" q.out && ok "::parts corpus <片> 挂上了（$(grep -a '挂上' q.out | sed 's/.*挂上 //' | cut -c1-80)）" \
    || bad "挂不上" "$(grep -a '\[语料\]' q.out | head -2 | tr '\n' ' ')"
if grep -qa "w000042 的边" q.out && grep -qa -- 'w000042 --\[' q.out; then
    ok "::parts corpus rel w000042 查到了邻居（$(grep -ac -- '--\[' q.out) 行边）"
else
    bad "查不到邻居" "$(grep -a '\[语料\]' q.out | tail -2 | tr '\n' ' ')"
fi
# 每次只读一小段：读回的字节数远小于文件（这是"局部加载"的核心，不能悄悄整片读进内存）
_read=$(grep -a '共 [0-9]* 字节' q.out | tail -1 | sed 's/.*共 \([0-9]*\) 字节.*/\1/')
_fsz=$(wc -c < "$A")
if [ -n "${_read:-}" ] && [ "$_read" -lt $((_fsz / 10)) ]; then
    ok "只读了 ${_read} 字节（文件 ${_fsz} 字节的 $(awk -v a="$_read" -v b="$_fsz" 'BEGIN{printf "%.1f%%", 100.0*a/b}')）"
else
    bad "读回字节数不明显小于文件（读 ${_read:-?} / 文件 ${_fsz}）" ""
fi

echo "== ④b 挂到脑上（attach 后 ::rel 直接能查，邻居必须是语料的词名）=="
printf '::parts corpus attach %s\n::rel w000042\n::parts corpus close\n::rel w000042\n::quit\n' "$A" > q2.in
./build_aip --model m.bin < q2.in > q2.out 2>&1
grep -qa '挂到脑上' q2.out && ok "attach 成功（$(grep -a '挂到脑上' q2.out | sed 's/.*挂到脑上：//' | cut -c1-70)）" \
    || bad "attach 失败" "$(grep -a '\[语料\]' q2.out | head -2 | tr '\n' ' ')"
# 邻居必须都是 w%06u：以前直接用 pool_token_by_id(b_id)，会打出一堆出厂模型里的中文词
_badname=$(grep -a -- '--\[' q2.out | sed 's/.*--> //' | grep -cv '^w[0-9][0-9][0-9][0-9][0-9][0-9]$' || true)
_nedge=$(grep -ac -- '--\[' q2.out || true)
if [ "${_nedge:-0}" -gt 0 ] && [ "${_badname:-1}" -eq 0 ]; then
    ok "::rel w000042 走语料：$_nedge 条边，邻居全是 w%06u（id 映射对）"
else
    bad "::rel 的邻居不是语料的词（边 $_nedge 条，异常名 ${_badname:-?} 个）" "$(grep -a -- '--\[' q2.out | head -3 | tr '\n' ' ')"
fi
# close 之后**同一个进程**里不该还查得到（DISK 标签必须摘干净，不然是 use-after-free 的温床）
_before_close=$(awk '/关掉/{exit} /w000042 --\[/{n++} END{print n+0}' q2.out)
_after_close=$(awk 'seen{ if ($0 ~ /w000042 --\[/) n++ } /关掉/{seen=1} END{print n+0}' q2.out)
if [ "${_before_close:-0}" -gt 0 ] && [ "${_after_close:-1}" -eq 0 ]; then
    ok "close 之后同一个进程里不再有语料的边（close 前 $_before_close 条 / 后 0 条）"
else
    bad "close 没把 DISK 标签摘干净（close 前 ${_before_close:-?} / 后 ${_after_close:-?}）" ""
fi

echo "== ⑤ 坏文件必须拒 =="
head -c 1000 "$A" > trunc.c7b
printf '::parts corpus %s\n::quit\n' "$WORK/trunc.c7b" | ./build_aip --model m.bin 2>&1 | grep -qa '打不开' \
    && ok "截断文件被拒" || bad "截断文件居然打开了" ""
printf '::parts corpus %s\n::quit\n' "$WORK/m.bin" | ./build_aip --model m.bin 2>&1 | grep -qa '打不开' \
    && ok "非 C7B 文件（model.bin）被拒" || bad "非 C7B 文件居然打开了" ""

echo
printf '语料分片：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS corpus7b（确定性 + 自洽 + 续跑 + 引擎可挂可查 + 坏文件拒）\n' || printf 'FAIL corpus7b\n'
[ "$fail" -eq 0 ]
