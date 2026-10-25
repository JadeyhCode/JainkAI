#!/bin/sh
# tests/holdout.sh — **新说法留出集**的判分（不是回归套件：它允许失败，失败清单才是待办）
#
# 用法：sh tests/holdout.sh [模型] [留出集]
# 每条输入**单独起一个进程**（避免上下文串味）；第三列是可选的"前置几句"（分号分隔）。
# 判分：最后一句的 AI 答复里出现期望正则就算"听懂"。
# §6.612 可选第四列 `chain`：按顺序判**最后 k 句**答复（`;` 分隔的正则），用于两轮的题。
# 规矩（很重要）：**不许为了让这里变绿去加 if 分支** —— 要么改数据（词表/关系表/
# 骨架表/答复模板），要么接受"这条还没听懂"写进局限。改代码必须单独说明理由。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
SET=${2:-tests/holdout.txt}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

# §6.324 `AIP_DUAL=1`：同一份题再跑另一个模型，**列出两个模型答复不一致的行** ——
# 用来查"分词/知识差异"（`属于` vs `属+于`、`鲸鱼` vs `鲸+鱼`）。不影响本模型的判分。
OTHER=model.bin
[ "$MODEL" = "model.bin" ] && OTHER=session.bin

pass=0; fail=0; bad=""; div=0
# §6.530 **临时文件名必须带本进程号**：这两个文件原来写死 `/tmp/holdout_one.log`，
# 两个量尺（比如 A/B 新旧二进制）**同时**跑就会互相覆盖 —— 实测两边都得出假数字
# （blind4 447/446、pert 208/209，而真值是 448/215）。这是"量尺不可信"级别的坑。
TMP1=/tmp/holdout_one.$$.log
TMP2=/tmp/holdout_two.$$.log
DUAL=/tmp/holdout_dual.$$.tsv
rm -f "$DUAL"
trap 'rm -f "$TMP1" "$TMP2" "$DUAL"' EXIT
# §6.612 第四列可选：`chain` —— **按顺序**判最后 k 句答复（正则用 `;` 分隔）。
# 用来测"两轮才成立"的东西（追问回话：先问 ⇒ 说不知道；再教 ⇒ 说记下了）。
while IFS='	' read -r q want setup chain; do
    case "$q" in '#'*|'') continue ;; esac
    [ -n "$want" ] || continue
    {
        if [ -n "$setup" ]; then
            printf '%s\n' "$setup" | tr ';' '\n'
        fi
        printf '%s\n::quit\n' "$q"
    } | timeout 30 "$BIN" --model "$MODEL" > "$TMP1" 2>&1
    out=$(sed -n 's/.*AI > //p' "$TMP1" | tail -1)
    # 这一轮所有答复（按顺序），给 `chain` 用
    all=$(sed -n 's/.*AI > //p' "$TMP1")
    if [ -n "${AIP_DUAL:-}" ] && [ -f "$OTHER" ]; then
        {   if [ -n "$setup" ]; then printf '%s\n' "$setup" | tr ';' '\n'; fi
            printf '%s\n::quit\n' "$q"
        } | timeout 30 "$BIN" --model "$OTHER" > "$TMP2" 2>&1
        out2=$(sed -n 's/.*AI > //p' "$TMP2" | tail -1)
        printf '%s\t%s\t%s\n' "$q" "$out" "$out2" >> "$DUAL"
        [ "$out" = "$out2" ] || div=$((div+1))
    fi
    if [ -n "${chain:-}" ]; then
        # 依次核对最后 k 句答复（k = chain 的分段数）；最后一段也可以写 `!片段`（不许出现）
        nch=$(printf '%s' "$chain" | awk -F';' '{print NF}')
        okc=1
        i=1
        while [ "$i" -le "$nch" ]; do
            pat=$(printf '%s' "$chain" | awk -F';' -v k="$i" '{print $k}')
            line=$(printf '%s\n' "$all" | tail -n "$((nch - i + 1))" | head -1)
            case "$pat" in
            !*) printf '%s\n' "$line" | grep -qE "${pat#!}" && okc=0 ;;
            *)  printf '%s\n' "$line" | grep -qE "$pat" || okc=0 ;;
            esac
            i=$((i + 1))
        done
        if [ "$okc" -eq 1 ]; then pass=$((pass+1))
        else fail=$((fail+1)); bad="$bad
  ✗ $q  ⇒ 链式断言不成立：$(printf '%s' "$all" | tr '\n' '|')"
        fi
        continue
    fi
    case "$want" in
    !*)  # `!片段` = 答复里**不许**出现（查"学出垃圾"）
        if printf '%s\n' "$out" | grep -qE "${want#!}"; then fail=$((fail+1)); bad="$bad
  ✗ $q  ⇒ $out"; else pass=$((pass+1)); fi
        continue ;;
    esac
    if printf '%s\n' "$out" | grep -qE "$want"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        bad="$bad
  ✗ $q  ⇒ $out"
    fi
done < "$SET"
echo "留出集：$pass 通过 / $((pass+fail)) 条"
[ -n "$bad" ] && printf '%s\n' "$bad"
echo "PASS holdout（$pass/$((pass+fail))）"
if [ -n "${AIP_DUAL:-}" ] && [ -f "$DUAL" ]; then
    echo "双模型对照（$MODEL vs $OTHER）：$div 行答复不同"
    awk -F'\t' -v m="$MODEL" -v o="$OTHER" \
        '$2 != $3 { printf "  ≠ %s\n      %s: %s\n      %s: %s\n", $1, m, $2, o, $3 }' \
        "$DUAL" | head -20
fi
