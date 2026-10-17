#!/bin/sh
# tools/asan_check.sh — 手工跑一遍 AddressSanitizer / UBSan / LeakSanitizer（§6.192）
#
# **为什么不进常规整包**：sanitizer 构建慢（约 10 s）、依赖本机有 libasan/libubsan，
# 而且"有泄漏就非零退出"会让整包在某些环境里假红。它是**按需复查**的工具：
#   sh tools/asan_check.sh            # 构建 + 生命周期 + 随机句 + 病态输入
#   CC=clang sh tools/asan_check.sh   # 换编译器
#
# 会跑三组输入：
#   ① 生命周期：load → 开开关 → 教/问/纠正 → 各种 :: 命令 → save → load 回来 → 再问
#   ② 确定性随机句 120 条（和 tests/fuzz.sh 同一个种子/词表）
#   ③ 病态输入：空行 / 空白 / 裸括号 / 保留记号 / 600 词长句 / 300 字长词 / 连串不
#
# 判据：每组 rc=0，且输出里没有
#   `ERROR: AddressSanitizer` / `runtime error:` / `SUMMARY: AddressSanitizer`（泄漏也会打 SUMMARY）
# 注意：`-Werror` **不能带** —— 项目里有一批故意的 `format-truncation` 截断，
# 在 -O1+sanitizer 下会变成硬错误（§6.190 踩过）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-gcc}
OUT=${OUT:-/tmp/aip_asan}
MODEL=${MODEL:-model.bin}
TMP=${TMP:-/tmp/asan_check}
[ -f "$MODEL" ] || { echo "FAIL: 缺 $MODEL"; exit 1; }
command -v "$CC" > /dev/null 2>&1 || { echo "SKIP: 没有编译器 $CC"; exit 0; }

rm -rf "$TMP"; mkdir -p "$TMP"

echo "== 1/4 构建 sanitizer 版（$CC，-O1 -g -fsanitize=address,undefined，不带 -Werror）"
if ! "$CC" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
        -Wall -Wextra -Wno-error -o "$OUT" main.c -lm 2> "$TMP/build.log"; then
    echo "SKIP: sanitizer 构建失败（本机可能没有 libasan/libubsan）—— 见 $TMP/build.log"
    exit 0
fi

echo "== 2/4 生命周期"
cat > "$TMP/life.txt" <<'EOF'
::load model.bin
::askback on
::natcoin on
猫 属于 动物 吗
麒麟 属于 神兽
企鹅 不 属于 鸟
狗 会 唱歌 吗
不 对
恐龙 会 飞 吗
会
鲸鱼 属于 哺乳动物 吗
::rel 鲸鱼
::edges 猫
::why 猫 有 毛
::der
::think
::stats
::save /tmp/asan_check/life.bin
::load /tmp/asan_check/life.bin
猫 属于 动物 吗
::askback
::teach 月亮 喵 鱼
::rel 月亮
::world closed
::world open
::hypo
::prims
::lex 你好
::help
::quit
EOF

echo "== 3/4 确定性随机句 120 条"
awk -v N=120 'BEGIN{
    w[1]="猫"; w[2]="狗"; w[3]="鸟"; w[4]="鱼"; w[5]="桌子"; w[6]="水"; w[7]="太阳";
    w[8]="手机"; w[9]="电脑"; w[10]="老师"; w[11]="学生"; w[12]="学校"; w[13]="家"; w[14]="北京";
    w[15]="属于"; w[16]="会"; w[17]="不"; w[18]="有"; w[19]="在"; w[20]="喜欢"; w[21]="怕";
    w[22]="吗"; w[23]="什么"; w[24]="谁"; w[25]="飞"; w[26]="游泳"; w[27]="唱歌"; w[28]="跑";
    w[29]="吃"; w[30]="毛"; w[31]="翅膀"; w[32]="红色"; w[33]="大"; w[34]="小"; w[35]="很";
    w[36]="都"; w[37]="的"; w[38]="了"; w[39]="是"; w[40]="一种"; w[41]="动物"; w[42]="植物";
    w[43]="工具"; w[44]="朋友"; w[45]="你好"; w[46]="谢谢"; w[47]="我"; w[48]="你"; w[49]="他";
    w[50]="这个"; w[51]="那个"; w[52]="为什么"; w[53]="怎么"; w[54]="几个"; w[55]="多少";
    w[56]="爬树"; w[57]="声音"; w[58]="颜色"; w[59]="工作"; w[60]="学习";
    seed = 12345
    for (i = 0; i < N; ++i) {
        seed = (seed * 1103515245 + 12345) % 2147483648
        k = 1 + int(seed / 2147483648 * 10)
        line = ""
        for (j = 0; j < k; ++j) {
            seed = (seed * 1103515245 + 12345) % 2147483648
            idx = 1 + int(seed / 2147483648 * 59.999)
            line = line (j ? " " : "") w[idx]
        }
        print line
    }
}' > "$TMP/rnd.txt"

echo "== 4/4 病态输入"
{
    printf '\n'; printf '   \n'; printf '[\n'; printf '[原语:分割]\n'; printf '吗\n'
    printf '不 不 不 不 不 不\n'
    awk 'BEGIN{s="";for(i=0;i<600;i++)s=s"猫 ";print s}'
    awk 'BEGIN{s="";for(i=0;i<300;i++)s=s"猫";print s}'
} > "$TMP/patho.txt"

fail=0
run_group() {
    name=$1; input=$2
    set +e
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 timeout 900 "$OUT" "$MODEL" \
        < "$input" > "$TMP/$name.out" 2>&1
    rc=$?
    set -e
    bad=$(grep -aE "ERROR: AddressSanitizer|runtime error:|SUMMARY: AddressSanitizer" "$TMP/$name.out" | head -3 || true)
    if [ "$rc" -ne 0 ] || [ -n "$bad" ]; then
        echo "  [FAIL] $name（rc=$rc）"
        [ -n "$bad" ] && printf '%s\n' "$bad" | sed 's/^/    /'
        fail=1
    else
        echo "  [OK]   $name（rc=0，无 ASan/UBSan/泄漏报告）"
    fi
}

run_group "life"   "$TMP/life.txt"
run_group "random" "$TMP/rnd.txt"
run_group "patho"  "$TMP/patho.txt"

if [ "$fail" -ne 0 ]; then
    echo "FAIL asan_check（看上面的报告；输出在 $TMP/*.out）"
    exit 1
fi
echo "PASS asan_check（生命周期 + 120 条随机句 + 8 条病态输入：ASan/UBSan/LSan 全干净）"
