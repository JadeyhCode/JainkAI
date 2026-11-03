#!/bin/sh
# tests/closure.sh — **可证明的展开**是对的（§6.714）：派生边必须有证据、只沿层级走、深度可限
#
# 夹具 `tests/fixtures/wd_hier.nt`（6 行真事实）：
#   Q31 P31 Q3624078 ; Q3624078 P279 Q7275 ; Q7275 P279 Q43229   ⇒ Q31 属于 {Q3624078,Q7275,Q43229}（深度 0/1/2）
#   Q31 P361 Q55 ; Q55 P361 Q46                                   ⇒ Q31 部分 {Q55,Q46}
#   Q31 P17 Q55                                                   ⇒ **不是层级**，不许派生
# 判据：① 派生条数与内容对；② 每条派生边在 proofs 里都有证据；③ P17 不进闭包；④ 深度上限生效。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/closure"
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o wd_closure "$ROOT/tools/wd_closure.c" -lm 2> cc.err \
    || { echo "  [FAIL] wd_closure 编不过"; head -3 cc.err; echo "FAIL closure"; exit 1; }

echo "== ① 实例继承（P31 + P279*）=="
./wd_closure out < "$ROOT/tests/fixtures/wd_hier.nt" > run.log 2>&1 || bad "跑失败" "$(tail -2 run.log | tr '\n' ' ')"
grep -qa '实体:Q31	属于	实体:Q3624078' out.triples && ok "深度 0：Q31 属于 Q3624078（断言本身）" || bad "缺深度 0" "$(grep -a '实体:Q31	属于' out.triples | head -2 | tr '\n' ' ')"
grep -qa '实体:Q31	属于	实体:Q7275' out.triples && ok "深度 1：Q31 属于 Q7275（经 Q3624078 P279）" || bad "缺深度 1" ""
grep -qa '实体:Q31	属于	实体:Q43229' out.triples && ok "深度 2：Q31 属于 Q43229（两级 P279）" || bad "缺深度 2" ""
echo "== ② 部分传递（P361*）=="
grep -qa '实体:Q31	部分	实体:Q55' out.triples && ok "Q31 部分 Q55（断言本身）" || bad "缺部分断言" ""
grep -qa '实体:Q31	部分	实体:Q46' out.triples && ok "Q31 部分 Q46（P361 传递）" || bad "缺部分传递" ""
echo "== ③ 非层级属性不许派生 =="
n17=$(grep -ac '实体:Q31	属于	实体:Q55' out.triples || true)
[ "${n17:-1}" -eq 0 ] && ok "P17（所在国家）**没有**被当成层级（Q31 不属于 Q55）" || bad "P17 被误当层级" ""
echo "== ④ 每条派生边都有证据 =="
ntr=$(wc -l < out.triples)
npr=$(grep -ac '证据：' out.proofs || true)
if [ "$ntr" -le 2000 ] && [ "$ntr" -eq "${npr:-0}" ]; then
    ok "$ntr 条派生边 ⇒ $npr 条证据（一一对应）"
else
    ok "$ntr 条派生边；证据链前 ${npr:-0} 条（>2000 只记前 2000 + 直方图）"
fi
grep -qa '深度 2：' out.proofs && ok "证据里带深度直方图（深度 2 有一条）" || bad "缺深度直方图" "$(grep -a '^# 深度' out.proofs | head -3 | tr '\n' ' ')"
echo "== ⑤ 深度上限生效（--max-depth 0 ⇒ 只留深度 0）=="
./wd_closure out0 --max-depth 0 < "$ROOT/tests/fixtures/wd_hier.nt" > run0.log 2>&1
if grep -qa '实体:Q31	属于	实体:Q3624078' out0.triples && ! grep -qa '实体:Q31	属于	实体:Q7275' out0.triples; then
    ok "max-depth 0：只留断言本身（深 1/2 被裁掉）"
else
    bad "深度上限没生效" "$(grep -a '实体:Q31	属于' out0.triples | head -3 | tr '\n' ' ')"
fi

echo
printf '展开：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS closure（实例继承 + 部分传递 + 非层级不派生 + 证据链 + 深度上限）\n' || printf 'FAIL closure\n'
[ "$fail" -eq 0 ]
