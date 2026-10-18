#!/bin/sh
# tests/corpusprobe.sh — **训练语料里真实问句**的三分类（§6.346）
#
# 从 `_corpus.txt` 抽自然语句（去掉 `[原语:…]`/`[对话:…]` 标记、按 `[对话:结束]` 切段），
# 只留 ≤16 字、以疑问词结尾的问句，去重后取前 N 条，逐条问一遍，分三类：
#   F=兜底（不 太 懂 / 还 没 学 过 / 答 不 上）  A=给事实  U=诚实未知（没 记 过 / 不 知 道）
# 这不是回归套件：它是**分布外**探针。
# 用法：sh tests/corpusprobe.sh [模型] [N，默认 100]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
MODEL=${1:-session.bin}
N=${2:-100}
TMP=${TMPDIR:-/tmp}/cp.$$
trap 'rm -f "$TMP.q" "$TMP.fails"' EXIT INT TERM

python3 - "$N" > "$TMP.q" <<'PY'
import re,sys
n=int(sys.argv[1])
seg=[]
for line in open("_corpus.txt",encoding="utf-8",errors="ignore"):
    for p in line.rstrip("\n").split("[对话:结束]"):
        t=re.sub(r"\[[^\]]*\]"," ",p)
        t=re.sub(r"[ \t]+"," ",t).strip()
        if t and len(t)<=16: seg.append(t)
qs=[s for s in seg if re.search(r"(吗|什么|谁|哪个|哪|怎么|为什么|多少)$", s)]
seen=set(); out=[]
for q in qs:
    if q in seen: continue
    seen.add(q); out.append(q)
print("\n".join(out[:n]))
PY

fallback=0; fact=0; unknown=0; n=0; : > "$TMP.fails"
while IFS= read -r q; do
    n=$((n+1))
    a=$(printf '%s\n::quit\n' "$q" | timeout 20 "$BIN" --model "$MODEL" 2>&1 | sed -n 's/.*AI > //p' | head -1)
    case "$a" in
    *"不 太 懂"*|*"还 没 学 过"*|*"答 不 上"*|"")
        fallback=$((fallback+1)); printf 'F|%s => %s\n' "$q" "$a" >> "$TMP.fails" ;;
    *"没 记 过"*|*"不 知 道"*) unknown=$((unknown+1)) ;;
    *) fact=$((fact+1)) ;;
    esac
done < "$TMP.q"
pct() { awk -v a="$1" -v b="$n" 'BEGIN{printf "%.1f", b?100*a/b:0}'; }
echo "语料问句 $n 条：给事实 $fact（$(pct "$fact")%）／诚实未知 $unknown（$(pct "$unknown")%）／兜底 $fallback（$(pct "$fallback")%）"
[ "$fallback" -gt 0 ] && { echo "兜底样例："; head -8 "$TMP.fails"; }
