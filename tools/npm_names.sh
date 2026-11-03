#!/bin/sh
# tools/npm_names.sh — 拉 **npm 全量包名**（4 564 295 个；replicate.npmjs.com 的 `_all_docs` 分页；§6.722）
#
# 为什么需要它：npmmirror 的 `/-/all` 已 404、搜索 API 封顶 1 万条；而 npm 官方复本库的
# `_all_docs` 能**按 id 顺序分页**吐出全部 456 万个包名（实测 110 KB/s，1 万条/页 ≈ 1 MB/页）。
# 拿到名单后，包本身从**快的** npmmirror（1.7 MB/s）下 —— 慢的只有名单这一趟。
#
# 断点续跑：`$OUT/npm_names.txt` 逐页追加；`$OUT/.last` 记最后一条 id（重跑从它继续）。
#
# 用法：sh tools/npm_names.sh <输出目录> [目标条数，默认 4600000]
set -u
OUT=${1:?用法: sh tools/npm_names.sh <输出目录> [目标条数]}
TARGET=${2:-4600000}
mkdir -p "$OUT" || exit 1
NAMES="$OUT/npm_names.txt"
LASTF="$OUT/.last"
: >> "$NAMES"
LAST=$(cat "$LASTF" 2>/dev/null || echo "")

got=$(wc -l < "$NAMES" 2>/dev/null || echo 0)
echo "== npm 包名：已有 $got / 目标 $TARGET =="
t0=$(date +%s)
while [ "$got" -lt "$TARGET" ]; do
    if [ -n "$LAST" ]; then
        # ⚠ `startkey` + `skip=1` 会 **400 Bad Request**（实测）；改成 startkey 含首条、本地去掉重复
        url="https://replicate.npmjs.com/registry/_all_docs?limit=10000&startkey=%22$(printf '%s' "$LAST" | sed 's/"/%22/g')%22"
    else
        url="https://replicate.npmjs.com/registry/_all_docs?limit=10000"
    fi
    if ! timeout 120 curl -k -s -o "$OUT/page.json" "$url"; then
        echo "  取页失败（重试）"; sleep 5; continue
    fi
    : > "$OUT/page.txt"
    grep -o '"id":"[^"]*"' "$OUT/page.json" 2>/dev/null | sed 's/^"id":"//; s/"$//' > "$OUT/page.txt"
    n=$(wc -l < "$OUT/page.txt" 2>/dev/null || echo 0)
    if [ "$n" -eq 0 ]; then
        echo "  没有更多（本页 0 条）⇒ 完"; break
    fi
    # 第一行是上一页的最后一条（startkey 含端点）⇒ 丢掉
    if [ -n "$LAST" ] && [ "$(head -1 "$OUT/page.txt")" = "$LAST" ]; then
        sed '1d' "$OUT/page.txt" > "$OUT/page2.txt" && mv "$OUT/page2.txt" "$OUT/page.txt"
        n=$((n - 1))
    fi
    cat "$OUT/page.txt" >> "$NAMES"
    LAST=$(tail -1 "$NAMES")
    printf '%s' "$LAST" > "$LASTF"
    got=$((got + n))
    el=$(( $(date +%s) - t0 )); [ "$el" -gt 0 ] || el=1
    [ $((got % 100000)) -lt "$n" ] && printf '  %d / %d（%.1f%%）；%.0f 个/秒；最后 %s\n' \
        "$got" "$TARGET" "$(awk -v a="$got" -v b="$TARGET" 'BEGIN{printf "%.1f", 100*a/b}')" \
        "$(awk -v a="$got" -v t="$el" 'BEGIN{printf "%.0f", a/t}')" "$LAST"
    rm -f "$OUT/page.json" "$OUT/page.txt"
done
echo "== 名单：$NAMES（$(wc -l < "$NAMES") 行）=="
