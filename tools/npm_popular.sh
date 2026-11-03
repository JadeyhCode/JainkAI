#!/bin/sh
# tools/npm_popular.sh — 按**热度**取 npm 包名（搜索 API，前 1 万名）⇒ 大包多、每条请求更值钱（§6.727）
#
# 为什么：全量名单（456 万）里绝大多数是垃圾包（实测 18 257 个包才 82 985 条 ≈ 4.5 条/包）。
# 搜索 API 按热度排序，取前 10 000 名（40 页 × 250），这些包里代码多得多。
#
# 用法：sh tools/npm_popular.sh <输出文件> [页数，默认 40]
set -u
OUT=${1:?用法: sh tools/npm_popular.sh <输出文件> [页数]}
PAGES=${2:-40}
: > "$OUT"
i=0
while [ "$i" -lt "$PAGES" ]; do
    from=$((i * 250))
    if ! timeout 120 curl -k -s "https://registry.npmmirror.com/-/v1/search?text=&size=250&from=$from" -o /tmp/np_page.json; then
        echo "  页 $i 取失败，重试"; sleep 3; continue
    fi
    n=$(grep -o '"name":"[^"]*"' /tmp/np_page.json | sed 's/^"name":"//; s/"$//' | wc -l)
    [ "$n" -gt 0 ] || { echo "  页 $i 空 ⇒ 停"; break; }
    grep -o '"name":"[^"]*"' /tmp/np_page.json | sed 's/^"name":"//; s/"$//' >> "$OUT"
    printf '  页 %d/%d：+%d 个（累计 %d）\n' "$((i + 1))" "$PAGES" "$n" "$(wc -l < "$OUT")"
    i=$((i + 1))
done
rm -f /tmp/np_page.json
sort -u "$OUT" -o "$OUT"
echo "== 热门前 $(wc -l < "$OUT") 个包 ⇒ $OUT =="
