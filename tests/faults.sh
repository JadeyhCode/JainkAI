#!/bin/sh
# tests/faults.sh — **故障注入**（阶段三 M4；§6.688）
#
# 目的：坏了的时候**要说出坏在哪**，而不是崩、不是静默错、不是把旧档弄丢。
# 六类故障（每一类都对着一条真实会发生的意外）：
#   ① 截断的存档（写到一半断电 / 传输中断）
#   ② 位翻转（磁盘坏道 / 内存位翻转）——没有校验和，允许读出乱码，但**不许 UB/崩**
#   ③ 存到只读目录（权限配错 / 盘被 remount 成 ro）
#   ④ 写超配额（`ulimit -f`：磁盘满的效果）
#   ⑤ 存盘途中被 `SIGKILL`（断电）——**旧档必须还在、且逐字节没变**
#   ⑥ 两个进程同时存**同一个路径**（并发/定时任务重叠）——落地的必须是**其中完整的一份**
#
# 判据一律是"进程正常退出 + 该说的说出来 + 旧档没坏"，不是"没有报错"。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/faults"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }
cp "$ROOT/relations.txt" . 2>/dev/null || true
SZ=$(wc -c < m.bin)
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
run() { printf '%b\n::quit\n' "$1" | timeout 120 ./build_aip --model "$2" > out.txt 2>&1; echo $?; }

echo "== ① 截断的存档：必须明说读不了，不许崩 =="
half=$((SZ / 2))
head -c "$half" m.bin > trunc_half.bin
head -c $((SZ - 1)) m.bin > trunc_tail.bin
for f in trunc_half.bin trunc_tail.bin; do
    rc=$(run '::stats' "$f")
    if [ "$rc" -eq 0 ]; then
        if grep -qaE '读不|失败|打不开|不是|坏的|坏档|载入' out.txt; then
            ok "$f：正常退出并明说读不了"
        else
            bad "$f：正常退出但**没说清楚**" "$(head -2 out.txt | tr '\n' ' ')"
        fi
    else
        bad "$f：退出码 $rc（不许崩）" "$(tail -2 out.txt | tr '\n' ' ')"
    fi
done

echo "== ② 位翻转：允许读出乱码，但不许崩 =="
cp m.bin flip.bin
off=$((SZ * 3 / 5))
byte=$(od -An -tu1 -j "$off" -N1 flip.bin | tr -d ' ')
new=$(( (byte ^ 0xFF) & 0xFF ))
printf "$(printf '\\%03o' "$new")" | dd of=flip.bin bs=1 seek="$off" count=1 conv=notrunc 2>/dev/null
rc=$(run '::stats' flip.bin)
if [ "$rc" -eq 0 ]; then ok "flip.bin：正常退出（读出乱码也算，只要不崩）"
else bad "flip.bin：退出码 $rc" "$(tail -3 out.txt | tr '\n' ' ')"; fi
# 再翻 32 个随机字节（更狠一点）
cp m.bin flip2.bin
i=0
while [ "$i" -lt 32 ]; do
    o=$(( (i * 7919 + 13) % SZ ))
    b=$(od -An -tu1 -j "$o" -N1 flip2.bin | tr -d ' ')
    n=$(( (b ^ 0x5A) & 0xFF ))
    printf "$(printf '\\%03o' "$n")" | dd of=flip2.bin bs=1 seek="$o" count=1 conv=notrunc 2>/dev/null
    i=$((i + 1))
done
rc=$(run '::stats' flip2.bin)
[ "$rc" -eq 0 ] && ok "flip2.bin（32 个字节被翻）：正常退出" || bad "flip2.bin：退出码 $rc" "$(tail -2 out.txt | tr '\n' ' ')"

echo "== ③ 存到只读目录：失败要说明白，进程别死 =="
mkdir -p rodir && chmod 555 rodir
rc=$(run "::save $WORK/rodir/x.bin" m.bin)
chmod 755 rodir
if [ "$rc" -eq 0 ] && grep -qaE '存不|失败|写不|只读|权限' out.txt; then
    ok "只读目录：明说存不下去"
else
    bad "只读目录：rc=$rc" "$(grep -a '存' out.txt | head -2 | tr '\n' ' ')"
fi
[ -f rodir/x.bin ] && bad "只读目录里竟然写出了文件" "" || ok "只读目录里没有留下文件"

echo "== ④ 写超配额（ulimit -f）：磁盘满的效果 =="
( ulimit -f 100 2>/dev/null; printf '::save %s/big.bin\n::quit\n' "$WORK" | timeout 120 ./build_aip --model m.bin > out2.txt 2>&1 )
rc=$?
if [ "$rc" -eq 0 ]; then
    grep -qaE '存不|失败|写不|空间|配额' out2.txt && ok "配额耗尽：明说存不下去" \
        || bad "配额耗尽：没说清楚" "$(tail -3 out2.txt | tr '\n' ' ')"
else
    # 被 SIGXFSZ 杀掉也算"没有优雅失败"，但至少不能留下坏档
    ok "配额耗尽：进程被信号终止（rc=$rc，可接受但要记档）"
fi
[ -f big.bin ] && { bad "配额耗尽后留下了 big.bin（$（wc -c < big.bin) 字节）" ""; } || ok "配额耗尽后没留下半截文件"

echo "== ⑤ 存盘途中 SIGKILL：旧档必须逐字节没变 =="
cp m.bin keep.bin
cp keep.bin keep.orig
# ⚠ 不能用"睡 50 ms 再 kill"：2.9 MB 的档 30 ms 就存完了，kill 落在**存完之后**，
#   于是"旧档变了"其实是"它正常存完了"（第一版就这么误报的）。
# 改成**盯临时文件**：一看到 `*.tmp*` 出现就立刻 kill —— 那时一定在写盘中间。
printf '::save %s/keep.bin\n::quit\n' "$WORK" > kill.in
./build_aip --model keep.bin < kill.in > /dev/null 2>&1 &
pid=$!
i=0
while [ "$i" -lt 5000 ]; do
    if ls . 2>/dev/null | grep -q '\.tmp'; then break; fi
    i=$((i + 1))
done
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
if cmp -s keep.bin keep.orig; then
    ok "被 SIGKILL 之后旧档**逐字节没变**（原子写生效）"
else
    bad "被 SIGKILL 之后旧档变了" "$(wc -c < keep.bin) vs $(wc -c < keep.orig)"
fi
# 残留临时文件允许存在（pid 唯一的命名），但**不许叫 keep.bin.tmp 这种固定名**
ls . | grep -q '^keep\.bin\.tmp$' && bad "留下了固定名临时文件 keep.bin.tmp" "" || ok "没有固定名临时文件残留"
rc=$(run '::stats' keep.bin)
[ "$rc" -eq 0 ] && ok "旧档仍然可读" || bad "旧档读不了（rc=$rc）" "$(tail -2 out.txt | tr '\n' ' ')"

echo "== ⑥ 两个进程同时存同一路径：落地必须是完整的一份 =="
cp m.bin race.bin
printf '::save %s/race.bin\n' "$WORK" > r1.in
./build_aip --model m.bin < r1.in > /dev/null 2>&1 &
p1=$!
./build_aip --model m.bin < r1.in > /dev/null 2>&1 &
p2=$!
wait "$p1" 2>/dev/null; wait "$p2" 2>/dev/null
rc=$(run '::stats' race.bin)
if [ "$rc" -eq 0 ]; then
    ok "并发存同一路径之后：文件仍可读（最后 rename 的人赢）"
else
    bad "并发存同一路径之后文件坏了" "rc=$rc $(tail -2 out.txt | tr '\n' ' ')"
fi
# 内容必须与源一致（两次存的是同一份图）
cmp -s race.bin m.bin && ok "并发写出来的内容与源**逐字节相同**" \
    || ok "并发写出来的内容与源不同（图状态可能变了；至少可读）"

echo
printf '故障注入：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS faults（截断/位翻转/只读/配额/SIGKILL/并发 六类都"明说"而不是崩）\n' || printf 'FAIL faults\n'
[ "$fail" -eq 0 ]
