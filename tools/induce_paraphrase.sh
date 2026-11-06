#!/bin/sh
# tools/induce_paraphrase.sh — **从例句里归纳"说法→规范关系"**（§6.760）
#
# 输入：每行 `原句<TAB>规范事实`（规范事实 = `主体 关系 客体`，空格分隔）
#   企鹅擅长游泳	企鹅 会 游泳
#   老虎擅长奔跑	老虎 会 奔跑
# 归纳：原句去掉"规范主语"前缀、"规范宾语"后缀 ⇒ 中间剩下的就是**原说法**；
#       它对应规范事实的关系词 ⇒ 得到 `擅长<TAB>会`。
# 输出：追加进 `paraphrase.map`（运行时加载，不用重编译）。
#
# 用法：sh tools/induce_paraphrase.sh <例句文件> [映射文件，默认 paraphrase.map]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EX=${1:?用法: sh tools/induce_paraphrase.sh <例句文件> [paraphrase.map]}
MAP=${2:-$ROOT/paraphrase.map}
: >> "$MAP"
n=0; tried=0
while IFS='	' read -r raw canon; do
    [ -n "${raw:-}" ] || continue
    tried=$((tried + 1))
    raw=$(printf '%s' "$raw" | sed 's/[。！？]$//; s/^ *//; s/ *$//')
    sub=$(printf '%s' "$canon" | awk '{print $1}')
    rel=$(printf '%s' "$canon" | awk '{print $2}')
    obj=$(printf '%s' "$canon" | awk '{print $3}')
    [ -n "$sub" ] && [ -n "$obj" ] && [ -n "$rel" ] || continue
    # 原句必须以主语开头、以宾语结尾（否则这条例句对归纳没用）
    case "$raw" in "$sub"*) ;; *) continue ;; esac
    mid=${raw#"$sub"}
    case "$mid" in *"$obj") mid=${mid%"$obj"} ;; *) continue ;; esac
    mid=$(printf '%s' "$mid" | tr -d ' ')
    [ -n "$mid" ] || continue
    [ "$mid" = "$rel" ] && continue
    if grep -q "^$mid	" "$MAP" 2>/dev/null; then continue; fi
    printf '%s\t%s\n' "$mid" "$rel" >> "$MAP"
    printf '  归纳：%s → %s（来自「%s」）\n' "$mid" "$rel" "$raw"
    n=$((n + 1))
done < "$EX"
echo "== 读完 $tried 条例句，新增 $n 条映射 ⇒ $MAP（共 $(wc -l < "$MAP") 行）=="
