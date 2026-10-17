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

echo "== 2/4 生命周期（模型：$MODEL）"
cat > "$TMP/life.txt" <<'EOF'
::load __MODEL__
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
sed -i "s|__MODEL__|$MODEL|" "$TMP/life.txt"

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

echo "== 4b/4 非法/截断 UTF-8（§6.193 的两个越界都靠这组发现）"
{
    printf '\200\201\202\377\376\n'        # 非法字节
    printf '\347\214\n'                       # 截断的 UTF-8（「猫」的一半）
    printf '\347\n'                             # 单字节
    printf '\001\002\003\n'                   # 控制字符
    awk 'BEGIN{s="";for(i=0;i<20000;i++)s=s"猫";print s}'   # 6 万字节一行
} > "$TMP/bytes.txt"

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
run_group "bytes"  "$TMP/bytes.txt"

echo "== 4c/4 混合变异（合法词 + 随机字节，确定性 LCG）"
seed=987654321
rnd() { seed=$(( (seed * 1103515245 + 12345) % 2147483648 )); echo $(( seed / 65536 )); }
words="猫 狗 属于 会 吗 不 水 飞 游泳 唱歌 动物 植物 桌子 太阳 什么 谁"
: > "$TMP/mut.txt"
i=0
while [ "$i" -lt 120 ]; do
    line=""; n=$(( $(rnd) % 6 + 1 )); j=0
    while [ "$j" -lt "$n" ]; do
        if [ $(( $(rnd) % 3 )) -eq 0 ]; then
            b=$(( $(rnd) % 255 + 1 ))
            frag=$(printf "\\$(printf '%03o' "$b")")
        else
            k=$(( $(rnd) % 16 + 1 ))
            frag=$(printf '%s' "$words" | cut -d' ' -f"$k")
        fi
        line="$line$frag"; j=$((j + 1))
    done
    printf '%s\n' "$line" >> "$TMP/mut.txt"
    i=$((i + 1))
done
run_group "mutate" "$TMP/mut.txt"

echo "== 4d/4 语法级变异（换/复制/删词/插不/加吗/拼句，确定性）"
awk -v N=120 'BEGIN{
    S[1]="猫 属于 动物 吗"; S[2]="狗 会 唱歌 吗"; S[3]="鸟 会 飞"; S[4]="水 不 会 跑";
    S[5]="企鹅 属于 鸟 吗"; S[6]="桌子 上 有 书"; S[7]="我 在 学校 学习"; S[8]="麒麟 属于 神兽";
    S[9]="谁 不 会 唱歌"; S[10]="什么 属于 神兽"; S[11]="鲸鱼 属于 哺乳动物 吗"; S[12]="猫 有 毛 吗";
    S[13]="你 能 来 吗"; S[14]="太阳 会 唱歌 吗"; S[15]="蝙蝠 不 属于 鸟"; S[16]="龙 会 飞 吗";
    S[17]="手机 属于 工具 吗"; S[18]="不 不 不"; S[19]="吗 吗 吗"; S[20]="我 不 知道";
    seed = 424242
    for (i = 0; i < N; ++i) {
        seed = (seed * 1103515245 + 12345) % 2147483648; sidx = 1 + int(seed / 2147483648 * 19.999)
        n = split(S[sidx], w, " ")
        seed = (seed * 1103515245 + 12345) % 2147483648; op = 1 + int(seed / 2147483648 * 6.999)
        if (op == 1 && n >= 2) {
            seed = (seed * 1103515245 + 12345) % 2147483648; a = 1 + int(seed / 2147483648 * (n-0.001))
            seed = (seed * 1103515245 + 12345) % 2147483648; b = 1 + int(seed / 2147483648 * (n-0.001))
            t = w[a]; w[a] = w[b]; w[b] = t
        } else if (op == 2) {
            seed = (seed * 1103515245 + 12345) % 2147483648; a = 1 + int(seed / 2147483648 * (n-0.001))
            for (j = n + 1; j > a + 1; --j) w[j] = w[j-1]
            w[a+1] = w[a]; n++
        } else if (op == 3 && n >= 2) {
            seed = (seed * 1103515245 + 12345) % 2147483648; a = 1 + int(seed / 2147483648 * (n-0.001))
            for (j = a; j < n; ++j) w[j] = w[j+1]
            n--
        } else if (op == 4) {
            seed = (seed * 1103515245 + 12345) % 2147483648; a = 1 + int(seed / 2147483648 * n)
            for (j = n + 1; j > a; --j) w[j] = w[j-1]
            w[a] = "不"; n++
        } else if (op == 5) {
            w[++n] = "吗"
        } else {
            seed = (seed * 1103515245 + 12345) % 2147483648; sidx2 = 1 + int(seed / 2147483648 * 19.999)
            m = split(S[sidx2], v, " ")
            for (j = 1; j <= m; ++j) w[++n] = v[j]
        }
        line = w[1]
        for (j = 2; j <= n; ++j) line = line " " w[j]
        print line
    }
}' > "$TMP/syn.txt"
run_group "syntax" "$TMP/syn.txt"

if [ "$fail" -ne 0 ]; then
    echo "FAIL asan_check（看上面的报告；输出在 $TMP/*.out）"
    exit 1
fi
echo "PASS asan_check（生命周期 + 120 条随机句 + 8 条病态输入：ASan/UBSan/LSan 全干净）"
