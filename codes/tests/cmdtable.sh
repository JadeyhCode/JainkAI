#!/bin/sh
# tests/cmdtable.sh — 元命令表：**命令名只有一份**
#
# 以前命令名写了两份：派发链一份（五十个 if）、::help 一份。两份会漂移，
# 症状是「帮助里有的命令其实不认」或反过来 —— 不是崩溃，是文档骗人。
# 现在派发按表里的 id，帮助由表生成。这一套看护盯的是**表和实现不许脱节**：
#
#   ① ::help 列出的条数与表自己报的条数一致
#   ② 帮助里列出的**每一条**命令都真的认得（打一遍，不许出现「未知命令」）
#   ③ 表里有而 switch 里没实现的，程序会自报「没有实现」—— 同样不许出现
#   ④ 参数形状真的在管：不收参数的给了参数要报用法；要参数的缺了也要报用法
#   ⑤ 别名与整词匹配：::fp 认、::rel 与 ::relinfer 不互相吃、::relx 不认
#
# 用法：sh tests/cmdtable.sh [模型文件]        默认 <repo>/model.bin
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${1:-$ROOT/model.bin}
WORK="$ROOT/_t/cmd"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

CC=${CC:-gcc}
$CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o aip "$ROOT/main.c" -lm
[ -s "$MODEL" ] || { echo "FAIL: 找不到模型 $MODEL"; exit 1; }
cp "$MODEL" model.bin
for f in frames.txt relations.txt; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" .; done

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; echo "         证据: $2"; fail=$((fail+1)); }

# ---- ① 帮助：条数与命令名清单 ----
printf '::help\n::quit\n' | ./aip --model model.bin > help.log 2>&1 || true
n_declared=$(sed -n 's/.*共 \([0-9]*\) 条.*/\1/p' help.log | head -1)
sed -n 's/^  ::\([A-Za-z0-9]*\).*/\1/p' help.log | sort -u > help.names
n_listed=$(wc -l < help.names)
[ -n "$n_declared" ] && ok "帮助自己报了条数（$n_declared 条）" || bad "帮助条数" "$(head -3 help.log)"
[ "$n_declared" = "$n_listed" ] \
    && ok "列出来的命令数（$n_listed）与表的条数一致" \
    || bad "条数一致" "声明 $n_declared / 实列 $n_listed"

echo "== ② 帮助里列出的每条命令都真的认得（一次会话里全打一遍） =="
# quit 会结束会话，跳过（它单独测）；load/save 会动文件，跳过（它们另有回归看护）
{ printf '::load model.bin\n'
  grep -v -x -e quit -e load -e save help.names | while read -r n; do printf '::%s\n' "$n"; done
  printf '::quit\n'
} | ./aip > all.log 2>&1 || true

if grep -q '未知命令' all.log; then
    bad "有帮助里列出却不认的命令" "$(grep -m3 '未知命令' all.log)"
else
    ok "帮助里列出的命令全部认得（没有「未知命令」）"
fi
if grep -q '没有实现' all.log; then
    bad "表里有但 switch 里没实现" "$(grep -m3 '没有实现' all.log)"
else
    ok "表与实现没脱节（没有「没有实现」）"
fi

echo "== ③ 别名 / 整词匹配 =="
printf '::fp\n::relx\n::quit\n' | ./aip --model model.bin > alias.log 2>&1 || true
grep -q '\[指纹\]' alias.log && ok "别名 ::fp 认" || bad "别名" "$(grep -m2 'fp\|指纹' alias.log)"
grep -q '未知命令 relx' alias.log \
    && ok "::relx 不认（::rel 与 ::relinfer 不互相吃）" || bad "整词匹配" "$(grep 'relx' alias.log)"

echo "== ④ 参数形状真的在管 =="
printf '::stats 多余参数\n::edges\n::quit\n' | ./aip --model model.bin > arity.log 2>&1 || true
grep -q 'stats 不收参数' arity.log \
    && ok "不收参数的命令给了参数 → 报用法" || bad "CARG_NONE" "$(grep -m2 'stats\|不收' arity.log)"
grep -q 'edges 要参数' arity.log \
    && ok "要参数的命令缺了参数 → 报用法" || bad "CARG_REQ" "$(grep -m2 'edges\|要参数' arity.log)"

echo "== ⑤ 帮助文本来自表（改表就变） =="
# 每一条帮助行都必须以 "  ::" 开头（格式由打印器统一，说明不是手抄的）
n_help_lines=$(grep -c '^  ::' help.log)
[ "$n_help_lines" = "$n_listed" ] \
    && ok "帮助里的每一行都来自表（$n_help_lines 行）" || bad "帮助格式" "行 $n_help_lines / 名 $n_listed"
grep -q '::save' help.log && grep -q '默认 model.bin' help.log \
    && ok "帮助里的默认文件名由 %M 替换（不是写死两份）" || bad "%M 替换" "$(grep -m2 'save' help.log)"

echo "== ⑥ 用法段也只有一份（来自 USAGE[] 表） =="
# 老坑：文件头注释里抄了一份用法、cmd_help() 里又打一份，两份已经漂移
#（`--batch --save` 只在其中一份、`--interval`/`-h` 两份都没有）。现在只有一张表。
./aip -h > usage.log 2>&1 || true
n_usage_src=$(sed -n '/^static const UsageDef USAGE\[\] = {/,/^};/p' "$ROOT/main.c" | grep -c '^    { "')
n_usage_out=$(grep -c '^  AIPoject' usage.log)
[ "$n_usage_src" -gt 0 ] && [ "$n_usage_src" = "$n_usage_out" ] \
    && ok "用法段由 USAGE[] 生成（表 $n_usage_src 行 / 打出 $n_usage_out 行）" \
    || bad "用法表" "表 $n_usage_src / 打出 $n_usage_out"
miss=0
for f in $(grep -o -- '--[a-z]*' usage.log | sort -u); do
    grep -q "\"$f\"" "$ROOT/main.c" || { miss=1; echo "        用法里写了 $f，main() 里没有"; }
done
grep -q '"-h"' "$ROOT/main.c" || { miss=1; echo "        用法里写了 -h，main() 里没有"; }
[ "$miss" -eq 0 ] && ok "用法里写的每个命令行开关都真的被 main() 认" || bad "开关脱节" "见上"
grep -q '^ \* 用法：见 ::help' "$ROOT/main.c" \
    && ok "文件头只留指针，不再抄第二份用法" \
    || bad "文件头用法" "$(grep -n '用法' "$ROOT/main.c" | head -3)"

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS cmdtable（元命令表：$n_listed 条命令，表与实现一致，$pass 项）"
else
    echo "FAIL cmdtable（$fail 项失败，$pass 项通过）"
    exit 1
fi
