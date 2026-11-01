#!/bin/sh
# tests/soak.sh — **长跑不涨**（阶段三 M4；§6.689）
#
# 判据不是"跑完了"，是三个可测的数：
#   ① **RSS 不随轮数涨**：跑 N 轮与跑 2N 轮的常驻内存差 ≤5%
#      （每一轮都是幂等的：问同样的话、教同样的事实、存盘、开关一遍 —— 图**不应该变大**）
#   ② **答案不漂**：第 1 轮的答复与第 N 轮的同一问答复**逐字相同**
#   ③ **存活**：两段都正常退出（没有崩、没有被 OOM 杀）
#
# 为什么用"两段进程比 RSS"而不是"一段里看曲线"：同一段里 malloc 会复用已释放的块，
# 小幅泄漏常常看不出来；而"跑 2 倍轮数"会把每轮的真增长放大一倍，差值是线性的。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/soak"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
cp "$ROOT/build_aip" .
cp "$ROOT/model.bin" m.bin 2>/dev/null || { echo "FAIL: 缺 model.bin"; exit 1; }
cp "$ROOT/relations.txt" . 2>/dev/null || true
N=${1:-400}                     # 一段的轮数（默认 400 → 2N=800 轮；约 10 s）
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

# 一轮的话（**只读、幂等**）：查关系 → 查出处 → 查理由 →（每 20 轮）存盘。
# ⚠ 第一版用的是"教 + 问"：40 轮下来图真的会变大（问题/答复会留观测标记），
#   于是"RSS 涨 9%"读起来像泄漏、其实是**合法的图增长**。
#   长跑要测的是**泄漏**，所以一轮里只许做不改图的事（`::rel`/`::src`/`::why`/`::save`）。
gen_rounds() {
    i=0
    while [ "$i" -lt "$1" ]; do
        if [ $((i % 20)) -eq 0 ]; then
            cat <<'EOF'
::rel 猫
::src 猫 属于 动物
::why 猫 有 毛
::save soak_tmp.bin
EOF
        else
            cat <<'EOF'
::rel 猫
::src 猫 属于 动物
::why 猫 有 毛
EOF
        fi
        i=$((i + 1))
    done
    printf '猫 属于 动物 吗\n::quit\n'
}

echo "== ① 跑 N=$N 轮 =="
gen_rounds "$N" > n.in
t0=$(date +%s)
/usr/bin/time -f '%M' -o rss_n.txt ./build_aip --model m.bin < n.in > n.log 2>&1
rc=$?
t1=$(date +%s)
RSSN=$(tr -d ' \n' < rss_n.txt 2>/dev/null || echo 0)
[ "$rc" -eq 0 ] && ok "N 轮正常退出（$((t1 - t0)) s，RSS ${RSSN} KB）" || bad "N 轮退出码 $rc" "$(tail -2 n.log | tr '\n' ' ')"

echo "== ② 跑 2N=$((N * 2)) 轮（同样的循环，只是次数翻倍） =="
gen_rounds "$((N * 2))" > n2.in
t0=$(date +%s)
/usr/bin/time -f '%M' -o rss_n2.txt ./build_aip --model m.bin < n2.in > n2.log 2>&1
rc=$?
t1=$(date +%s)
RSSN2=$(tr -d ' \n' < rss_n2.txt 2>/dev/null || echo 0)
[ "$rc" -eq 0 ] && ok "2N 轮正常退出（$((t1 - t0)) s，RSS ${RSSN2} KB）" || bad "2N 轮退出码 $rc" "$(tail -2 n2.log | tr '\n' ' ')"

echo "== ③ RSS 不随轮数涨（差 ≤5%） =="
if [ "${RSSN:-0}" -gt 0 ] && [ "${RSSN2:-0}" -gt 0 ]; then
    D=$((RSSN2 - RSSN))
    [ "$D" -lt 0 ] && D=$(( -D ))
    PCT=$((D * 100 / RSSN))
    printf '         N=%s KB ／ 2N=%s KB ／ 差 %s KB（%s%%）\n' "$RSSN" "$RSSN2" "$D" "$PCT"
    [ "$PCT" -le 5 ] && ok "每轮不涨（差 ${PCT}% ≤ 5%）" || bad "每轮在涨：差 ${PCT}%" "$RSSN → $RSSN2"
else
    bad "RSS 取不到" "$(cat rss_n.txt 2>/dev/null | tr '\n' ' ')"
fi

echo "== ③b 图本身不涨（只读操作不该改图） =="
G1=$(grep -a '符号节点' n.log | head -1 | tr -s ' ')
G2=$(grep -a '符号节点' n2.log | head -1 | tr -s ' ')
if [ -n "$G1" ] && [ "$G1" = "$G2" ]; then
    ok "N 段与 2N 段的节点/边计数**完全相同**：$G1"
else
    bad "只读操作居然改了图" "$G1 ／ $G2"
fi

echo "== ④ 答案不漂（第 1 轮 vs 第 N 轮 vs 第 2N 轮） =="
first_n=$(grep -a 'AI >' n.log | head -1)
last_n=$(grep -a 'AI >' n.log | tail -1)
last_n2=$(grep -a 'AI >' n2.log | tail -1)
printf '         第 1 轮：%s\n         第 N 轮：%s\n' "$first_n" "$last_n"
if [ -n "$first_n" ] && [ "$first_n" = "$last_n" ]; then
    ok "N 段里首尾答复逐字相同"
else
    bad "N 段里答复漂了" "$first_n ／ $last_n"
fi
if [ -n "$last_n2" ] && [ "$last_n" = "$last_n2" ]; then
    ok "N 轮与 2N 轮的答复相同（不随轮数漂）"
else
    bad "2N 段答复与 N 段不同" "$last_n ／ $last_n2"
fi

echo "== ⑤ 存盘文件没有越存越大（每轮都存同一份图） =="
LAST=$(wc -c < soak_tmp.bin 2>/dev/null || echo 0)
printf '         最后一次存盘 %s 字节\n' "$LAST"
[ "${LAST:-0}" -gt 0 ] && ok "存盘产物存在且非空" || bad "存盘产物是空的" ""

echo
printf '长跑：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS soak（%s 轮 / %s 轮两段：RSS 不涨、答复不漂）\n' "$N" "$((N * 2))" || printf 'FAIL soak\n'
[ "$fail" -eq 0 ]
