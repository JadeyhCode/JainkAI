#!/bin/sh
# tests/patlong.sh — **长行 / 丢行自检**（§6.371）
#
# 为什么单有一份：`[reply]` 的关键词串有个按行的上限（512 字节），超了整行被丢；
# 这个坑在 §6.370 静默吞掉过一行（`reply=55` 而文件 56 行），当时只有 `patcount` 能发现。
# 这一套把三条固定住：
#   ① 512 以内的长关键词串**真的加载、真的能命中**；
#   ② 超过 512 的行**必须**打 ⚠「关键词串太长」，而加载自检**必须**报「文件里 2 行、实际加载 1 行」；
#   ③ 仓库自带的 `patterns.txt` **不许**触发这个警告（正常数据必须干净）。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
case "$BIN" in /*) ;; *) BIN="$ROOT/${BIN#./}" ;; esac   # ① 会 cd 到临时目录跑，必须是绝对路径
MODEL=${MODEL:-$ROOT/model.bin}   # 沙箱里有 model.bin；这一套只验证"加载/丢弃"，不依赖会话知识
case "$MODEL" in /*) ;; *) MODEL="$ROOT/${MODEL#./}" ;; esac
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "FAIL: 需要 python3"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi }

python3 - "$TMP/patterns.txt" <<'PY'
import sys
kw_ok = "|".join("触发词%02d" % i for i in range(1, 43))      # ~503 字节：必须能装下
kw_bad = "|".join("超长词%02d" % i for i in range(1, 62))     # ~731 字节：必须被拦
with open(sys.argv[1], "w", encoding="utf-8") as f:
    f.write("[reply] %s=长行 命中 了\n" % kw_ok)
    f.write("[reply] %s=永远 不该 出现\n" % kw_bad)
PY

echo "① 512 以内的长关键词串：加载 + 命中"
out=$(cd "$TMP" && printf '随便 触发词07 问一句\n::quit\n' | timeout 30 "$BIN" --model "$MODEL" 2>&1)
printf '%s\n' "$out" | sed -n 's/.*AI > //p' | grep -q "长行 命中 了" \
    && chk "长行（约 500 字节）加载并命中" 1 \
    || chk "长行没命中" 0 "$(printf '%s\n' "$out" | sed -n 's/.*AI > //p' | head -1)"

echo "② 超过 512 的关键词串：⚠ 警告 + 加载自检报丢行"
printf '%s\n' "$out" | grep -q "关键词串太长" \
    && chk "超长行打了「关键词串太长」的 ⚠" 1 \
    || chk "超长行没有警告（静默丢行又回来了）" 0
printf '%s\n' "$out" | grep -q "文件里 2 行、实际加载 1 行" \
    && chk "加载自检报了「文件里 2 行、实际加载 1 行」" 1 \
    || chk "加载自检没报丢行" 0

echo "③ 仓库自带的数据不许触发丢行警告"
out2=$(printf '::quit\n' | timeout 30 "$BIN" --model "$MODEL" 2>&1)
printf '%s\n' "$out2" | grep -q "\[patterns\] ⚠" \
    && chk "正常 patterns.txt 触发了丢行警告（有行被吞）" 0 \
    || chk "正常 patterns.txt 没有丢行" 1

[ "$fail" -eq 0 ] && echo "PASS patlong（长行加载 / 超长拦截 / 自检报丢行 / 正常数据干净）" \
                  || { echo "FAIL patlong"; exit 1; }
