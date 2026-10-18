#!/bin/sh
# tests/conv.sh — **逐轮判分**的多轮对话留出集（§6.302）
#
# 与 holdout.sh 的区别：holdout 只判最后一句；这里每一轮都判。
# 格式（tests/conv.txt）：
#     --- 会话名
#     输入<TAB>期望正则          （期望以 ! 开头 = 答复里不许出现）
#     输入<TAB>期望正则
#     --- 下一个会话
# 每个会话在**同一个进程**里跑完（上下文连续），每轮取当轮 AI 答复判分。
#
# 用法：sh tests/conv.sh [模型] [文件]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
SET=${2:-tests/conv.txt}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }

TMP=${TMPDIR:-/tmp}/conv.$$
trap 'rm -rf "$TMP"' EXIT INT TERM
rm -rf "$TMP"; mkdir -p "$TMP"
# 按 --- 切成块
awk -v dir="$TMP" '
/^--- / { n++; name=$0; sub(/^--- /,"",name); gsub(/[^A-Za-z0-9_]/,"_",name); fname=dir"/"n"_"name; print name > (fname".name"); next }
/^#/ { next }
{
  if (n == 0) next;
  split($0, a, "\t");
  print a[1] > (fname".in");
  print a[2] > (fname".want");
}
' "$SET"

pass=0; fail=0; bad=""
for namef in "$TMP"/*.name; do
    [ -f "$namef" ] || continue
    base=${namef%.name}
    name=$(cat "$namef")
    {
        cat "$base.in"
        printf '::quit\n'
    } | timeout 60 "$BIN" --model "$MODEL" > "$base.out" 2>&1
    sed -n 's/.*AI > //p' "$base.out" > "$base.got"
    i=0
    while IFS= read -r want; do
        i=$((i+1))
        q=$(sed -n "${i}p" "$base.in")
        got=$(sed -n "${i}p" "$base.got")
        case "$want" in
        !*) if printf '%s\n' "$got" | grep -qE "${want#!}"; then
                fail=$((fail+1)); bad="$bad
  ✗ [$name] $q ⇒ $got"
            else pass=$((pass+1)); fi ;;
        *)  if printf '%s\n' "$got" | grep -qE "$want"; then
                pass=$((pass+1))
            else fail=$((fail+1)); bad="$bad
  ✗ [$name] $q ⇒ $got"; fi ;;
        esac
    done < "$base.want"
done
echo "逐轮判分：$pass 通过 / $((pass+fail)) 轮"
[ -n "$bad" ] && printf '%s\n' "$bad"
echo "PASS conv（$pass/$((pass+fail)) 轮）"
