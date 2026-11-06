#!/bin/sh
# tests/framesink.sh — 帧定义**完整**进图，且与 frames.txt **逐条一致**（§6.768）
#
# 判据（不是"能查就行"，是"镜像等价"）：
#   图里的 帧/问法/答法/填充 四类事实 == 直接解析 frames.txt 得到的那四类 **逐条相同**。
# 只有等价，才有资格谈"运行时帧选中也走图"。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W="$ROOT/_t/framesink"; rm -rf "$W"; mkdir -p "$W"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }
cd "$ROOT"

sh tools/frames2triples.sh > "$W/fr.triples" 2>/dev/null
n_file=$(grep -c . "$W/fr.triples" || true)
cp model.bin "$W/m.bin"
printf '::ingest %s\n::save %s\n::quit\n' "$W/fr.triples" "$W/m2.bin" | "$ROOT/build_aip" --model "$W/m.bin" > "$W/ing.log" 2>&1
if grep -qa '写进去' "$W/ing.log"; then ok "帧三元组 $n_file 条导入图"; else bad "导入失败" "$(tail -2 "$W/ing.log")"; fi

# 从图里读回：::rel 帧库 得帧名，再对每个帧 ::rel 帧:<名> 得 问法/答法/填充
printf '::rel 帧库\n::quit\n' | "$ROOT/build_aip" --model "$W/m2.bin" > "$W/frames.out" 2>&1
n_g=$(grep -ac -- '--\[帧\]-->' "$W/frames.out" || true)
if [ "${n_g:-0}" -eq 22 ]; then ok "图里有 22 个帧"; else bad "帧数不对（$n_g）" ""; fi

# 逐条比对：把图里读到的 问法/答法/填充 收集成集合，和三元组文件比
: > "$W/rel_raw.txt"
sed -n 's/.*--\[帧\]--> *//p' "$W/frames.out" | while IFS= read -r name; do
    [ -n "$name" ] || continue
    printf '::rel %s\n::quit\n' "$name" | "$ROOT/build_aip" --model "$W/m2.bin" 2>/dev/null >> "$W/rel_raw.txt"
done
python3 - "$W/rel_raw.txt" "$W/from_graph.txt" <<'PYEOF2'
import sys, re
src, dst = sys.argv[1], sys.argv[2]
seen = []
for line in open(src, encoding='utf-8', errors='ignore'):
    m = re.match(r'^\s*(\S+)\s+--\[(问法|答法|填充)\]-->\s*(.*?)\s*$', line)
    if m:
        seen.append((m.group(1), m.group(2), m.group(3)))
with open(dst, 'w', encoding='utf-8') as f:
    for a, b, c in seen:
        f.write(f'{a}\t{b}\t{c}\n')
PYEOF2
n_from_graph=$(grep -c . "$W/from_graph.txt" || true)
# 三元组文件里 帧:xxx 的问法/答法/填充（去掉"帧库 帧"那 22 行）
grep -v '^帧库	' "$W/fr.triples" > "$W/expect.txt"
LC_ALL=C sort "$W/expect.txt" > "$W/expect.sorted"
LC_ALL=C sort "$W/from_graph.txt" > "$W/graph.sorted"
if diff -q "$W/expect.sorted" "$W/graph.sorted" > /dev/null 2>&1; then
    ok "图里的 问法/答法/填充 与 frames.txt 逐条一致（$n_from_graph 条）"
else
    bad "图与文件不一致" "差 $(diff "$W/expect.sorted" "$W/graph.sorted" 2>/dev/null | grep -c '^[<>]' || true) 条"
fi

echo "== ④ 行为对照：文件装载 vs 图装载，同一批问题答案必须一模一样 =="
cat > "$W/qs.txt" <<'QEOF'
你 害怕 黑 吗
你 喜欢 猫 吗
你 记得 我 吗
你 吃 苹果 吗
你 叫 什么
QEOF
: > "$W/ans_file.txt"; : > "$W/ans_graph.txt"
while IFS= read -r q; do
    [ -n "$q" ] || continue
    printf '%s\n::quit\n' "$q" | "$ROOT/build_aip" --model "$W/m2.bin" 2>/dev/null | sed -n 's/^.*AI > //p' >> "$W/ans_file.txt"
    printf '%s\n::quit\n' "$q" | AIP_FRAMES_FROM_GRAPH=1 "$ROOT/build_aip" --model "$W/m2.bin" 2>/dev/null | sed -n 's/^.*AI > //p' >> "$W/ans_graph.txt"
done < "$W/qs.txt"
if [ -s "$W/ans_file.txt" ] && diff -q "$W/ans_file.txt" "$W/ans_graph.txt" > /dev/null 2>&1; then
    ok "5 个问题两种装载答案完全一致（$(grep -c . "$W/ans_file.txt") 条）"
else
    bad "两种装载答案不一致" "文件: $(head -3 "$W/ans_file.txt" | tr '\n' '|') 图: $(head -3 "$W/ans_graph.txt" | tr '\n' '|')"
fi

echo "== ⑤ 运行时校验命令 ::framesync 也说一致 =="
out_sync=$(printf '::framesync\n::quit\n' | "$ROOT/build_aip" --model "$W/m2.bin" 2>&1)
if printf '%s' "$out_sync" | grep -qa '一致（图是内存的忠实镜像）'; then ok "::framesync 报一致"; else bad "::framesync 没报一致" "$(printf '%s' "$out_sync" | grep -a '帧镜像\|图里' | tr '\n' ' ')"; fi

echo
printf '帧镜像：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS framesink（帧定义完整进图，与 frames.txt 逐条等价）\n' || printf 'FAIL framesink\n'
[ "$fail" -eq 0 ]
