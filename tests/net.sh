#!/bin/sh
# tests/net.sh — **联网检索**（§6.680）：默认关 + 白名单 + 上限 + 账本
#
# 这个套件**默认不联网**：它把"该拒的全都拒掉"逐条钉死（这些路径在 fork 之前就返回，
# 所以离线也能跑）。真想跑一次真检索/真抓取，用：
#     AIP_NET_LIVE=1 sh tests/net.sh
# 联网那一段会真的打 arXiv 的 API（一次检索 + 一次正文，合计 ≤ 3 次抓取）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/net"
rm -rf "$WORK"; mkdir -p "$WORK/papers"
cd "$WORK"
cp "$ROOT/model.bin" m.bin 2>/dev/null || true
cp "$ROOT/relations.txt" . 2>/dev/null || true
[ -d "$ROOT/net" ] && cp -a "$ROOT/net" .
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  [FAIL] %s\n        证据: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
run() { printf '%b\n::quit\n' "$1" | timeout 60 ./build_aip --model m.bin > out.txt 2>&1 || true; }
cp "$ROOT/build_aip" .
LOG=papers/cache/_net.log

echo "== ① 默认关：不开就一步都不出去 =="
run '::net'
grep -qaF '现在是 **关**' out.txt && grep -qaF 'arxiv.org' out.txt \
    && ok "::net 报「关」并列出白名单" || bad "状态" "$(grep -a '联网' out.txt | head -1)"
run '::net search attention'
grep -qaF '先 ::net on' out.txt && ok "关着时 ::net search 直接拒（不出去）" \
    || bad "关着检索" "$(grep -a '联网' out.txt | head -1)"
run '::net get 1'
grep -qaE '先 ::net on|现在没有检索结果' out.txt && ok "关着时 ::net get 直接拒" \
    || bad "关着抓取" "$(grep -a '联网' out.txt | head -1)"
[ ! -f "$LOG" ] || bad "关着还写了账本" "$(head -1 "$LOG")"

echo "== ② 打开后：该拒的一条都不许出去 =="
BEFORE=$( [ -f "$LOG" ] && wc -l < "$LOG" | tr -d ' ' || echo 0 )
run '::net on
::net get http://arxiv.org/abs/1706.03762
::net get https://evil.example.com/x
::net get https://arxiv.org.evil.com/x
::net get https://arxiv.org:443/abs/1
::net get https://user@arxiv.org/abs/1
::net get notaurl'
grep -qaF '只认 https://' out.txt && ok "http:// 被拒（只走 https）" \
    || bad "http 拒绝" "$(grep -a '没抓成' out.txt | head -1)"
grep -qaF '白名单里没有主机 `evil.example.com`' out.txt && ok "非白名单主机被拒" \
    || bad "白名单拒绝" "$(grep -a '没抓成' out.txt | head -2 | tr '\n' ' ')"
grep -qaF 'arxiv.org.evil.com' out.txt \
    && ok "arxiv.org.evil.com 也被拒（**精确匹配**，后缀骗不过）" \
    || bad "后缀骗" "$(grep -a '没抓成' out.txt | head -3 | tr '\n' ' ')"
grep -qaF '不许带用户名/端口' out.txt && ok "带端口的网址被拒" \
    || bad "端口" "$(grep -a '没抓成' out.txt | head -4 | tr '\n' ' ')"
grep -qaF '用法：::net get' out.txt && ok "裸字符串不是网址 ⇒ 用法提示（不当网址猜）" \
    || bad "裸串" "$(grep -a '联网' out.txt | tail -2 | tr '\n' ' ')"
AFTER=$( [ -f "$LOG" ] && wc -l < "$LOG" | tr -d ' ' || echo 0 )
NEW=$((AFTER - BEFORE))
[ "$NEW" -ge 4 ] && ok "被拒的**也记账**（新增 $NEW 行）" || bad "拒绝没记账" "新增 $NEW 行"
grep -qa 'REFUSE' "$LOG" 2>/dev/null && ok "账本里有 REFUSE 行（含理由）" \
    || bad "REFUSE" "$(tail -2 "$LOG" 2>/dev/null | tr '\n' ' ')"
run '::net log'
grep -qaF '账本最后' out.txt && ok "::net log 能看账" || bad "log" "$(head -2 out.txt | tr '\n' ' ')"

echo "== ③ 关回去也生效 =="
run '::net on
::net off
::net search x'
grep -qaF '先 ::net on' out.txt && ok "::net off 之后又拒（开关是活的）" \
    || bad "off" "$(grep -a '联网' out.txt | tail -2 | tr '\n' ' ')"

echo "== ④ 真联网（默认跳过；AIP_NET_LIVE=1 才跑） =="
if [ "${AIP_NET_LIVE:-0}" = "1" ]; then
    run '::net on
::net search attention is all you need'
    if grep -qaE '^   [0-9]\) [0-9]{4}\.[0-9]{4,5}' out.txt; then
        ok "arXiv 检索返回了条目（编号 + 标题）"
        ID=$(grep -aE '^   [0-9]\) ' out.txt | head -1 | awk '{print $2}')
        printf '        第一条：%s\n' "$ID"
    else
        bad "检索没返回条目" "$(grep -a '联网' out.txt | tail -3 | tr '\n' ' ')"
        ID=""
    fi
    if [ -n "${ID:-}" ]; then
        run "::net on
::net search attention is all you need
::net get 2"
        grep -qaF '存到 papers/cache/' out.txt && ok "正文抓下来并落盘" \
            || bad "抓正文" "$(grep -a '没抓成' out.txt | head -1)"
        grep -qaE 'PDF：[0-9]+ 个内容流' out.txt && ok "抓到的 PDF 直接抽出文字（走 pdf.h）" \
            || bad "PDF 取文字" "$(grep -a 'PDF' out.txt | head -1)"
        grep -qa 'OK' "$LOG" 2>/dev/null && ok "账本里有 OK 行（字节数可查）" \
            || bad "OK 账" "$(tail -2 "$LOG" 2>/dev/null | tr '\n' ' ')"
    fi
else
    printf '  [SKIP] 没设 AIP_NET_LIVE=1（默认不联网；真抓一次要 ~30 s）\n'
fi

echo
printf '联网检索：通过 %d / 失败 %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ] && printf 'PASS net（默认关 / 白名单 / 只认 https / 被拒也记账）\n' || printf 'FAIL net\n'
[ "$fail" -eq 0 ]
