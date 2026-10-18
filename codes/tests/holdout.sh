#!/bin/sh
# tests/holdout.sh — **新说法留出集**的判分（不是回归套件：它允许失败，失败清单才是待办）
#
# 用法：sh tests/holdout.sh [模型] [留出集]
# 每条输入**单独起一个进程**（避免上下文串味）；第三列是可选的"前置几句"（分号分隔）。
# 判分：最后一句的 AI 答复里出现期望正则就算"听懂"。
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

pass=0; fail=0; bad=""
while IFS='	' read -r q want setup; do
    case "$q" in '#'*|'') continue ;; esac
    [ -n "$want" ] || continue
    {
        if [ -n "$setup" ]; then
            printf '%s\n' "$setup" | tr ';' '\n'
        fi
        printf '%s\n::quit\n' "$q"
    } | timeout 30 "$BIN" --model "$MODEL" > /tmp/holdout_one.log 2>&1
    out=$(sed -n 's/.*AI > //p' /tmp/holdout_one.log | tail -1)
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
