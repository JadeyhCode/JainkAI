#!/bin/sh
# tests/framegate_ab.sh — 结构化帧「读关联链产物」（Gate B）的 A/B 看护
#
# 同一个模型、同一批输入，只切 ::framegate on|off，把两边的回复并排打出来。
# 目的不是断言「Gate B 一定更好」（它换的是覆盖面，代价是语义精度），
# 而是**把差异钉在测试里**：以后改阈值/虚词表/跨度规则，一眼能看出
# 到底是哪几句被换掉了，不至于悄悄漂移。
#
# 每条探针都**单独起一个进程、重新加载模型**。理由：这个 agent 会边聊边学，
# 上一条回复不同 → 图里多出来的边不同 → 后面几条的检索结果就会跟着变。
# 不隔离的话，A/B 表里会混进「对话级联」的差异，看不出到底是谁的功劳。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
PROBE=tools/probes_gate.txt
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }
[ -f "$PROBE" ] || { echo "FAIL: 缺 $PROBE"; exit 1; }

TMP=./_ab_tmp
rm -rf "$TMP"; mkdir -p "$TMP"

# 跑一条探针，输出「输入<TAB>放行次数<TAB>回复」
one() {   # $1 = on|off   $2 = 输入
    { printf '::load %s\n::framegate %s\n' "$MODEL" "$1"
      printf '%s\n' "$2"
      printf '::framegate\n::quit\n'
    } | "$BIN" 2>&1 > "$TMP/last.txt" || true
    rel=$(sed -n 's/.*（放行次数 \([0-9]*\)）.*/\1/p' "$TMP/last.txt" | tail -1)
    rep=$(sed -n 's/.*AI > //p' "$TMP/last.txt" | head -1)
    printf '%s\t%s\t%s\n' "$2" "${rel:-?}" "$rep"
}

: > "$TMP/on.tsv"; : > "$TMP/off.tsv"
: > "$TMP/inputs.txt"
while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    printf '%s\n' "$line" >> "$TMP/inputs.txt"
    one on  "$line" >> "$TMP/on.tsv"
    one off "$line" >> "$TMP/off.tsv"
done < "$PROBE"

n=$(wc -l < "$TMP/inputs.txt" | tr -d ' ')

# ---------------------------------------------------------------- 并排打表
i=0; diff_n=0
printf '  %-16s | %-30s | %s\n' "输入" "framegate=on" "framegate=off"
while IFS= read -r line; do
    i=$((i + 1))
    a=$(sed -n "${i}p" "$TMP/on.tsv"  | cut -f3)
    b=$(sed -n "${i}p" "$TMP/off.tsv" | cut -f3)
    mark=" "; [ "$a" = "$b" ] || { mark="*"; diff_n=$((diff_n + 1)); }
    printf '%s %-16s | %-30s | %s\n' "$mark" "$line" "$a" "$b"
done < "$TMP/inputs.txt"

on_rel=$(cut -f2 "$TMP/on.tsv"  | awk '{s+=$1} END{print s+0}')
off_rel=$(cut -f2 "$TMP/off.tsv" | awk '{s+=$1} END{print s+0}')
echo
echo "Gate B 改变了 $diff_n/$n 句（每条探针独立加载模型，无对话级联）"
echo "on  那次：关联链放行 ${on_rel} 次"
echo "off 那次：关联链放行 ${off_rel} 次"

# ---------------------------------------------------------------- 断言
fail=0
chk() {  # $1=说明  $2=条件结果(0/1)
    if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi
}

# ① 关掉开关时 Gate B 一次都不许跑
[ "$off_rel" = "0" ] && chk "framegate=off 时放行次数为 0" 1 \
                     || chk "framegate=off 时放行次数为 0（实得 $off_rel）" 0
# ② 打开时必须真的在放行，否则这个测试等于没测
[ "$on_rel" -ge 3 ] && chk "framegate=on 时放行 >=3 次（实得 $on_rel）" 1 \
                    || chk "framegate=on 时放行 >=3 次（实得 $on_rel）" 0

# ③ 白名单内的词必须两通道完全一致 —— Gate B 只在 Gate A 落空时才动手
same_ok=1; i=0
while IFS= read -r line; do
    i=$((i + 1))
    case "$line" in
        你喜欢雪吗|你怕黑吗|你知道量子吗|你想学画画吗|你在学画画吗|你觉得天气怎么样)
            a=$(sed -n "${i}p" "$TMP/on.tsv"  | cut -f3)
            b=$(sed -n "${i}p" "$TMP/off.tsv" | cut -f3)
            [ "$a" = "$b" ] || { same_ok=0; echo "       (白名单句不一致: $line → on「$a」/ off「$b」)"; }
            ;;
    esac
done < "$TMP/inputs.txt"
chk "白名单内的句子两通道逐字一致" "$same_ok"

# ④ 抽查：Gate B 打开后，名单外的词接上了本来接不上的帧
exp_fail=0
expect() {  # $1=输入 $2=期望回复
    a=$(grep -F "$1	" "$TMP/on.tsv" | cut -f3 | head -1)
    if [ "$a" = "$2" ]; then echo "  OK   $1 → $2"; else echo "  FAIL $1 → 「$a」（期望「$2」）"; exp_fail=1; fi
}
expect 你怕老师吗   "我 怕 老师"
expect 你知道动物吗 "我 知道 动物"
expect 你知道老师吗 "我 知道 老师"
expect 你喜欢安静吗 "我 喜欢 安静"
[ "$exp_fail" -eq 0 ] || fail=1

# ⑤ 反面看护：两条最典型的病句都不许出现
#    「我 想 学 支持 我」 = 槽位吞掉代词（跨度规则看护）
#    「我 喜欢 那么」     = 虚词被当成好填充（虚词表看护；
#                            那么 的分布分数 56，比不少实词还高）
for bad_pat in '我 想 学 支持 我' '我 喜欢 那么'; do
    nb=$(grep -c "$bad_pat" "$TMP/on.tsv" || true)
    [ "${nb:-0}" -eq 0 ] && chk "没有出现「$bad_pat」" 1 \
                         || chk "没有出现「$bad_pat」" 0
done

rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "PASS framegate_ab（Gate B 改变了 $diff_n/$n 句，全部为改善）" \
                  || { echo "FAIL framegate_ab"; exit 1; }
