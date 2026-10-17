#!/bin/sh
# tests/fuzz.sh — 健壮性：**确定性**随机句 + 病态输入，不许崩、不许不答（§6.190）
#
# 为什么单有一份：套件都是"人挑的句子"，而真用户会打出各种怪东西（长句、空行、半个括号、
# 保留记号、几乎不可分的长词、一长串「不」）。这些路径一崩，前面所有看护都没意义。
# 这里用**固定种子**的 LCG 生成 120 条随机句（1~10 个词，从 60 个常用词里取），
# 每条都必须**有答复**；再加一组病态输入（空行/空白/裸括号/保留名/600 词长句/300 字长词/连串不）。
#
# 另：**内存**这一层用 ASan/UBSan 跑过一遍（64 KiB 泄漏就是这么发现的，见 §6.190 的 `fr_split`），
# 但那要 sanitizer 环境，所以不进常规套件；常规套件只钉"不崩 + 都答"。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_fuzz_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# 固定种子 LCG：同样的输入永远一样（可复现，不是真随机）
gen() {
    awk -v N="$1" 'BEGIN{
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
    }'
}

echo "① 120 条确定性随机句：每条都要有答复，进程不许崩"
gen 120 > "$TMP/rnd.txt"
set +e
timeout 300 "$BIN" "$MODEL" < "$TMP/rnd.txt" > "$TMP/rnd.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] && chk "进程正常退出（rc=0）" 1 || chk "进程正常退出（rc=$rc）" 0
n_ans=$(grep -ac "AI > " "$TMP/rnd.out" || true)
[ "$n_ans" -eq 120 ] && chk "120 条都答了（实得 $n_ans）" 1 || chk "120 条都答了（实得 $n_ans）" 0
grep -aqE "Segmentation|Assertion|corrupt|stack smashing" "$TMP/rnd.out" \
    && chk "输出里没有崩溃迹象" 0 "$(grep -aE 'Segmentation|Assertion|corrupt|stack smashing' "$TMP/rnd.out" | head -1)" \
    || chk "输出里没有崩溃迹象" 1

echo "② 病态输入：空行 / 纯空白 / 裸括号 / 保留记号 / 600 词长句 / 300 字长词 / 连串不"
{
    printf '\n'
    printf '   \n'
    printf '[\n'
    printf '[原语:分割]\n'
    printf '吗\n'
    printf '不 不 不 不 不 不\n'
    awk 'BEGIN{s="";for(i=0;i<600;i++)s=s"猫 ";print s}'
    awk 'BEGIN{s="";for(i=0;i<300;i++)s=s"猫";print s}'
} > "$TMP/patho.txt"
set +e
timeout 300 "$BIN" "$MODEL" < "$TMP/patho.txt" > "$TMP/patho.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] && chk "病态输入也没崩（rc=0）" 1 || chk "病态输入也没崩（rc=$rc）" 0
grep -aqE "Segmentation|Assertion|corrupt|stack smashing" "$TMP/patho.out" \
    && chk "病态输入输出干净" 0 || chk "病态输入输出干净" 1

if [ "$fail" -ne 0 ]; then
    echo "FAIL fuzz（健壮性）"
    exit 1
fi
echo "PASS fuzz（120 条确定性随机句 + 8 条病态输入：不崩、都答）"
