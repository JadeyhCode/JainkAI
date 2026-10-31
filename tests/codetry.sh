#!/bin/sh
# tests/codetry.sh — **写-跑-看错-改 的机械闭环**（CLI 级，§6.617）
#
# 为什么要单列一套：这条路的入口是 `::code try <文件>`（要 `::act on`，会 fork gcc、读写 `_t/work`）。
# `optest` 只测了**修法表**（用假错误串），从没在真机上"真的编一次"；`asan_check` 也不跑它
# （它碰外部世界）。于是"闭环真的走了一遍"这件事一直没人验 —— 这轮把它补上。
#
# 判据（三条形状各有各的期望）：
#   ① 本来就对的源码 ⇒ **编译 1 次、首次 rc=0**，而且产物 `<名>.out` 真的出来了；
#   ② 少 `#include` ⇒ **编译 2 次、首次 rc=1 → 最终 rc=0**（修法表②生效）；
#   ③ 认不出的错（`int x = ;`）⇒ **编译 1 次、最终 rc=1**，并且**明说没有已知修法**（不瞎改）。
# 另外验"沙箱边界"：编译只在工作区里发生（`_t/work`），别的地方不许出现产物。
#
# 用法：sh tests/codetry.sh [模型文件]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/codetry"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.base
for f in patterns.txt frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

# §6.622 依赖本机有 gcc/cc：没有就**显式 SKIP**（不假装通过，也不红）
if ! command -v gcc >/dev/null 2>&1; then
    echo "SKIP codetry（本机没有 gcc —— 这条闭环要真编译才能验）"
    exit 0
fi

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }

mkdir -p _t/work
cat > _t/work/ok.c <<'EOF'
#include <stdio.h>
int main(void) { printf("hi\n"); return 0; }
EOF
cat > _t/work/noinc.c <<'EOF'
int main(void) { printf("hi\n"); return 0; }
EOF
cat > _t/work/bad.c <<'EOF'
int main(void) { int x = ; return 0; }
EOF
# §6.622 修法表还有两条：补分号 / 补声明 —— 真机各跑一次（以前只有假错误串单测）
cat > _t/work/semi.c <<'EOF'
int main(void) {
  int x = 1
  return x;
}
EOF
cat > _t/work/undecl.c <<'EOF'
int main(void) {
  x = 1;
  return x;
}
EOF

run() { cp model.base m.bin; { printf '::notepad off\n::char off\n::act on\n'; printf '%b' "$1"; } | ./aip --model m.bin > "$2" 2>&1 || true; }

echo "== ① 本来就对的源码：一次编过、产物出来 =="
run '::code try ok.c\n::code\n' c1
grep -qa 'ok.c：编译 1 次，首次 rc=0 → 最终 rc=0' c1 \
    && ok "ok.c：编译 1 次、rc=0" \
    || bad "ok.c-编译" "$(grep -a '写代码闭环' c1 | head -1)"
[ -x _t/work/ok.c.out ] && ok "产物「ok.c.out」真出来了（在 _t/work 里）" \
    || bad "ok.c-产物" "$(ls -1 _t/work 2>/dev/null | tr '\n' ' ')"

echo "== ② 少 #include：修法表②生效（第一次错、改一次、第二次过）=="
run '::code try noinc.c\n::code\n' c2
grep -qa 'noinc.c：编译 2 次，首次 rc=1 → 最终 rc=0' c2 \
    && ok "noinc.c：编译 2 次、首次 rc=1 → 最终 rc=0（补 #include 生效）" \
    || bad "noinc.c-闭环" "$(grep -a '写代码闭环' c2 | head -1)"
[ -x _t/work/noinc.c.out ] && ok "改完真的编出了「noinc.c.out」" \
    || bad "noinc.c-产物" "$(ls -1 _t/work 2>/dev/null | tr '\n' ' ')"
grep -qa '改了什么' c2 && ok "把「改了什么」说出来了（不是只给 rc）" \
    || bad "noinc.c-说明" "$(grep -a '写代码闭环' c2 | head -1)"

echo "== ③ 认不出的错：停在原地、明说没有已知修法（不瞎改）=="
run '::code try bad.c\n::code\n' c3
grep -qa 'bad.c：编译 1 次' c3 && grep -qav '最终 rc=0' c3 \
    && ok "bad.c：只编了 1 次、没「假装修好」（最终 rc≠0）" \
    || bad "bad.c-停手" "$(grep -a '写代码闭环' c3 | head -1)"
grep -qa '没有已知的机械修法\|补不进去' c3 \
    && ok "如实说「没有已知的机械修法 / 补不进去」（降级版不猜）" \
    || bad "bad.c-说明" "$(grep -a '改了什么' c3 | head -1)"

echo "== ⑤ 补分号（修法表①）：真机上真的补上、再编过 =="
run '::code try semi.c\n::code\n' c5
grep -qa 'semi.c：编译 2 次，首次 rc=1 → 最终 rc=0' c5 \
    && ok "semi.c：缺分号 ⇒ 编译 2 次、rc 1→0（在上一行末尾补了一个分号）" \
    || bad "semi.c-补分号" "$(grep -a '写代码闭环' c5 | head -1)"
grep -qa '按规则在\*\*上一行\*\*末尾补了' c5 \
    && ok "说清是「在上一行末尾补」，不是含糊地说「改了什么」" \
    || bad "semi.c-说明" "$(grep -a '改了什么' c5 | head -1)"

echo "== ⑥ 补声明（修法表③）：gcc 的弯引号也认得出 =="
run '::code try undecl.c\n::code\n' c6
grep -qa 'undecl.c：编译 2 次，首次 rc=1 → 最终 rc=0' c6 \
    && ok "undecl.c：未声明的名字 ⇒ 补声明后编过（gcc 报的是弯引号）" \
    || bad "undecl.c-补声明" "$(grep -a '写代码闭环' c6 | head -1)"
grep -qa '按规则补了声明' c6 \
    && ok "说清「类型只能猜 int」（机械降级的代价照实说）" \
    || bad "undecl.c-说明" "$(grep -a '改了什么' c6 | head -1)"

echo "== ⑦ 回滚（§6.641）：改完还编不过 ⇒ 把文件**还原** =="
cat > _t/work/two.c <<'EOF'
int main(void) {
  int x = 1
  foo();
  return x;
}
EOF
run '::code try two.c\n::code\n' c7
grep -qaE '编译 2 次.*最终 rc=1' c7 \
    && ok "two.c：改一次、还编不过 ⇒ 最终 rc=1（不假装修好）" \
    || bad "two.c-闭环" "$(grep -a '写代码闭环' c7 | head -1)"
grep -qaF '已把文件还原' c7 \
    && ok "说明里写了「已把文件还原」" \
    || bad "two.c-说明" "$(grep -a '改了什么' c7 | head -1)"
grep -qaF 'int x = 1' _t/work/two.c && ! grep -qaF 'int x = 1;' _t/work/two.c \
    && ! grep -qaF 'int x;' _t/work/two.c \
    && ok "**文件真的还原了**（补的分号没了 —— 不给你留半坏的源码）" \
    || bad "two.c-回滚" "$(sed -n 2p _t/work/two.c)"

echo "== ⑧ 多文件工程（§6.642）：::code build 一次编整个目录 =="
mkdir -p _t/work/proj
cat > _t/work/proj/a.h <<'EOF'
int add(int a, int b);
EOF
cat > _t/work/proj/a.c <<'EOF'
#include "a.h"
int add(int a, int b) { return a + b; }
EOF
cat > _t/work/proj/main.c <<'EOF'
#include <stdio.h>
#include "a.h"
int main(void) { printf("%d\n", add(2, 3)); return 0; }
EOF
run '::code build proj\n' c8
grep -qaF '2 个 .c 一次编过' c8 \
    && ok "proj：2 个 .c 一次编过（自己列目录、不用通配符；a.h 不算）" \
    || bad "proj-构建" "$(grep -a '多文件' c8 | head -1)"
[ -x _t/work/proj/a.out ] && ok "产物 proj/a.out 出来了" \
    || bad "proj-产物" "$(ls -1 _t/work/proj 2>/dev/null | tr '\n' ' ')"
cat > _t/work/proj/bad.c <<'EOF'
int broken(void) { return 不存在; }
EOF
run '::code build proj\n' c9
grep -qaE '编不过（rc=1）' c9 \
    && ok "坏文件 ⇒ 明确报 rc=1（不假装修好）" \
    || bad "proj-坏" "$(grep -a '多文件' c9 | head -1)"
rm -f _t/work/proj/bad.c

echo "== ⑨ 测试驱动（§6.645）：::code test —— 产物退出码当判据 =="
mkdir -p _t/work/tgreen _t/work/tred
cat > _t/work/tgreen/main.c <<'EOF'
#include <stdio.h>
static int add(int a, int b) { return a + b; }
int main(void) { if (add(2, 3) != 5) { printf("FAIL add\n"); return 1; } printf("ok\n"); return 0; }
EOF
cat > _t/work/tred/main.c <<'EOF'
#include <stdio.h>
static int add(int a, int b) { return a - b; }
int main(void) { if (add(2, 3) != 5) { printf("FAIL add\n"); return 1; } printf("ok\n"); return 0; }
EOF
run '::code test tgreen\n' c10
grep -qaF '**测试全绿**' c10 \
    && ok "tgreen：编过且产物 rc=0 ⇒ **测试全绿**" \
    || bad "test-绿" "$(grep -a '测试' c10 | head -1)"
run '::code test tred\n' c11
grep -qaF '测试不过**（rc=1）：FAIL add' c11 \
    && ok "tred：编过但 rc=1 ⇒ **测试不过**，并带出输出（FAIL add）" \
    || bad "test-红" "$(grep -a '测试' c11 | head -1)"
run '::act run a.out\n' c12
grep -qaF '不在白名单里' c12 \
    && ok "产物入口**没有**放宽白名单（::act run a.out 仍被拒）" \
    || bad "test-白名单" "$(grep -a '白名单' c12 | head -1)"

echo "== ⑩ 单文件产物也能跑（§6.646）：::code test <文件.c> =="
cat > _t/work/sfok.c <<'EOF'
int main(void) { return 0; }
EOF
cat > _t/work/sfbad.c <<'EOF'
int main(void) { return 3; }
EOF
run '::code test sfok.c\n' c13
grep -qaF 'sfok.c：编过、**测试全绿**' c13 \
    && ok "sfok.c：闭环编过 + 产物 rc=0 ⇒ 测试全绿" \
    || bad "test-单文件-绿" "$(grep -a '测试' c13 | head -1)"
run '::code test sfbad.c\n' c14
grep -qaF 'rc=3；断言' c14 \
    && ok "sfbad.c：编过但产物 rc=3 ⇒ 测试不过" \
    || bad "test-单文件-红" "$(grep -a '测试' c14 | head -1)"
cat > _t/work/nc.c <<'EOF'
int main(void) { return 不存在的名字; }
EOF
run '::code test nc.c\n' c15
grep -qaF '**没编过**' c15 \
    && ok "nc.c：编不过 ⇒ **不跑**（先编过才跑）" \
    || bad "test-单文件-没编过" "$(grep -a '测试' c15 | head -1)"

echo "== ⑪ 断言计数（§6.660）：产物输出里 ok/FAIL 行各算一条 =="
run '::code test tgreen\n' c16
grep -qaF '断言 1 ok / 0 FAIL' c16 \
    && ok "tgreen 打印 1 个 ok ⇒ 计数「1 ok / 0 FAIL」" \
    || bad "assert-计数" "$(grep -a '测试' c16 | head -1)"
cat > _t/work/twofail.c <<'EOF'
#include <stdio.h>
int main(void) { printf("FAIL a\n"); printf("FAIL b\n"); return 1; }
EOF
run '::code test twofail.c\n' c17
grep -qaF '2 FAIL' c17 \
    && ok "两条 FAIL ⇒ 计数「2 FAIL」" \
    || bad "assert-红计数" "$(grep -a '测试' c17 | head -1)"

echo "== ⑫ 严格判据（§6.661）：rc=0 但打了 FAIL ⇒ 严格模式下算红 =="
cat > _t/work/tmix.c <<'EOF'
#include <stdio.h>
int main(void) { printf("ok a\n"); printf("FAIL b\n"); return 0; }
EOF
run '::code test tmix.c\n' c18
grep -qaF '测试全绿' c18 \
    && ok "默认（只看退出码）：rc=0 ⇒ **测试全绿**（尽管打了 1 条 FAIL）" \
    || bad "strict-默认" "$(grep -a '测试' c18 | head -1)"
run '::code strict on\n::code test tmix.c\n' c19
grep -qaF '打了 1 条 FAIL（严格模式）⇒ **测试不过**' c19 \
    && ok "严格模式：rc=0 但有 FAIL 行 ⇒ **测试不过**（且不谎报 rc）" \
    || bad "strict-开" "$(grep -a '测试' c19 | head -1)"

echo "== ④ 沙箱边界：编译只发生在工作区 =="
[ -f _t/work/ok.c.out ] && [ ! -f ./ok.c.out ] && [ ! -f ./noinc.c.out ] \
    && ok "产物只在「_t/work/」里（cwd 边界没破）" \
    || bad "沙箱" "$(ls -1 *.out 2>/dev/null | tr '\n' ' ')"

echo
echo "写代码闭环：通过 $pass / 失败 $fail"
[ "$fail" -eq 0 ] && echo "PASS codetry（真机跑一遍：一次过 / 改一次过 / 认不出就停手）" \
                  || echo "FAIL codetry（$fail 项）"
[ "$fail" -eq 0 ] || exit 1
exit 0
