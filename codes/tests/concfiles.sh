#!/bin/sh
# tests/concfiles.sh — **跨进程**并发写同一份文件（§6.583）
#
# 并发阶段剩下的那一半：§6.582 修的是"同一进程里多线程"，但两个 `aip` 进程
# 同时 `::save m.bin`（或同时 `::parts save p.parts`）照样会互相踩。
#
# 这一套要抓的就是那个 bug：**写临时文件用的名字必须是唯一的**。
#   旧实现：临时名固定 `<路径>.tmp` ⇒ 两个进程同时写**同一个**临时文件，
#           互相截断/覆盖 ⇒ rename 出来的是一份混着两边的坏文件。
#   新实现：`<路径>.tmp.<pid>.<序号>` ⇒ 各写各的，rename 原子替换 ⇒ 读的人
#           永远看到"旧的完整"或"新的完整"。
#
# 判据（四条，都不看"看起来跑完了"）：
#   ① 每个写进程都报"已保存 N 字节"（N > 0）；
#   ② 每个读进程 8 次问答**都**有答复（没有一次卡在半个文件上）；
#   ③ 全部结束后**没有残留临时文件**（`*.tmp.*` 必须 0 个）；
#   ④ 最终文件能读、能答对（`猫 属于 动物 吗` ⇒ 对）。
#
# 用法：sh tests/concfiles.sh            （用 $ROOT/build_aip）
#       BIN=/tmp/旧版 sh tests/concfiles.sh   （用别的二进制 —— 用来证明这一套真的抓得到旧 bug）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT/build_aip}
MODEL=${1:-$ROOT/model.bin}
WORK=${WORK:-$ROOT/_t/concfiles}
NW=4      # 写进程数
NR=4      # 读进程数
IT=6      # 每个进程写/读几轮

rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" m.bin
for f in patterns.txt frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

echo "== ① ${NW} 个进程同时写 m.bin、${NR} 个进程同时读它 =="
i=0
while [ "$i" -lt "$NW" ]; do
    i=$((i+1))
    ( { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'
        printf '窗 户 属于 家具\n'
        k=0; while [ "$k" -lt "$IT" ]; do k=$((k+1)); printf '::save m.bin\n'; done
      } | "$BIN" --model m.bin > "w$i.log" 2>&1 ) &
done
i=0
while [ "$i" -lt "$NR" ]; do
    i=$((i+1))
    ( { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'
        k=0; while [ "$k" -lt "$IT" ]; do k=$((k+1)); printf '猫 属于 动物 吗\n'; done
      } | "$BIN" --model m.bin > "r$i.log" 2>&1 ) &
done
wait

fails=0
say_fail() { echo "  [FAIL] $1"; echo "         证据: $2"; fails=$((fails+1)); }
say_ok()   { echo "  [OK]   $1"; }

# ① 写进程都报告保存成功
i=0; wsaved=0
while [ "$i" -lt "$NW" ]; do
    i=$((i+1))
    n=$(grep -c '已保存 [0-9]* 字节' "w$i.log" 2>/dev/null || echo 0)
    [ "$n" -ge "$IT" ] && wsaved=$((wsaved+1))
done
[ "$wsaved" -eq "$NW" ] \
    && say_ok "① 每个写进程 ${IT} 次保存都报成功（$NW/$NW）" \
    || say_fail "① 写进程保存次数不够" "只有 $wsaved/$NW 个写进程报满 $IT 次保存"

# ② 读进程 8 次都有答复
i=0; rok=0
while [ "$i" -lt "$NR" ]; do
    i=$((i+1))
    n=$(grep -c '^你 > AI > \|AI > ' "r$i.log" 2>/dev/null || echo 0)
    [ "$n" -ge "$IT" ] && rok=$((rok+1))
done
[ "$rok" -eq "$NR" ] \
    && say_ok "② 每个读进程 ${IT} 次问答都有答复（$NR/$NR）—— 没有一次读到半个文件" \
    || say_fail "② 读进程有问答丢失" "只有 $rok/$NR 个读进程拿满 $IT 条答复（第二个参数：grep AI >）"

# ③ 没有残留临时文件
left=$(ls m.bin.tmp.* 2>/dev/null | wc -l | tr -d ' ')
[ "$left" = "0" ] \
    && say_ok "③ 没有残留临时文件（m.bin.tmp.* = 0）" \
    || say_fail "③ 有残留临时文件" "$(ls m.bin.tmp.* 2>/dev/null | head -3 | tr '\n' ' ')"

# ④ 最终文件能读、能答对
out=$( { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n猫 属于 动物 吗\n'; } | "$BIN" --model m.bin 2>&1 || true )
printf '%s' "$out" | grep -q '对 ，' \
    && say_ok "④ 并发写完之后 m.bin 还能读、还能答对（「猫 属于 动物 吗」⇒ 对）" \
    || say_fail "④ 最终文件读不出来/答错" "$(printf '%s' "$out" | tail -3 | tr '\n' ' ')"

# ⑤ 段文件同样：并发写 p.parts，然后 attach 必须成功
echo "== ② ${NW} 个进程同时写 p.parts，再各自 attach 读回来 =="
i=0
while [ "$i" -lt "$NW" ]; do
    i=$((i+1))
    ( { printf '::notepad off\n::char off\n::hypo cache off\n::rel analogy off\n'
        k=0; while [ "$k" -lt "$IT" ]; do k=$((k+1)); printf '::parts save p.parts\n'; done
        printf '::parts attach p.parts\n::parts stats\n猫 属于 动物 吗\n'
      } | "$BIN" --model m.bin > "pw$i.log" 2>&1 ) &
done
wait
pok=0; i=0
while [ "$i" -lt "$NW" ]; do
    i=$((i+1))
    n=$(grep -c '索引已挂上' "pw$i.log" 2>/dev/null || echo 0)
    [ "$n" -ge 1 ] && pok=$((pok+1))
done
[ "$pok" -eq "$NW" ] \
    && say_ok "⑤ 每个写进程都 attach 成功（$NW/$NW）—— 段索引没有写出半截" \
    || say_fail "⑤ 有进程 attach 不上并发写出来的 p.parts" "$pok/$NW 成功"
left=$(ls p.parts.tmp.* 2>/dev/null | wc -l | tr -d ' ')
[ "$left" = "0" ] && say_ok "⑤ 段文件也没有残留临时文件" || say_fail "⑤ 段文件有残留" "$(ls p.parts.tmp.* | head -3 | tr '\n' ' ')"

if [ "$fails" -eq 0 ]; then
    echo "PASS concfiles（跨进程并发写：文件不坏、读得到、无残留）"
    exit 0
fi
echo "FAIL concfiles（$fails 项）"
exit 1
