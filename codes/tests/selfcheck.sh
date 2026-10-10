#!/bin/sh
# tests/selfcheck.sh — **看护的看护**：清单/脚本自身那三类"整包会骗人"的坑
#
# 为什么单有一份：这一轮（2026-10-10）连着扫出两个真洞，都是**汇总看着是绿的、其实没测到**：
#   ① 6 个 C 级套件写成 `./build_xxx | tail -3` —— 管道的退出码是 `tail` 的（永远 0），
#      于是 C 测试自己报的失败（实测：delibtest「125 项通过，5 项失败」）在整包里**隐形**；
#   ② `set -e` 下 `x=$(…grep…)` 没兜住 —— grep 没匹配时退出码是 1，那个赋值会把**整套**
#      静默弄死（连一行 FAIL 都不打，看起来像"跑了"）。本轮扫出 5 处（all.sh/cmdtable/logic4）。
# 修一次不等于修好：**这三类必须能自己红**，所以放进第 0 步，跟编译/探针一个规格。
#
# 查的是"清单与脚本的写法"，不是产品行为 —— 秒级，纯 grep/awk。
# 用法：sh tests/selfcheck.sh        （tests/all.sh 第 0 步会自动调它）
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

bad=0
say() { printf '  [%s] %s\n' "$1" "$2"; }

echo "== ① 套件命令不许用管道收尾（退出码会被 tail/head 吃掉）=="
# 只看清单里的命令列（`名字|命令`），# 开头与空行不算
if grep -v '^[[:space:]]*#' tests/suites.txt | grep -v '^[[:space:]]*$' \
   | grep -qE '\|[^|]*\|[[:space:]]*(tail|head)\b'; then
    say FAIL "有套件命令以 「| tail」/「| head」 收尾（C 测试自己的失败会隐形）"
    grep -nE '\|[^|]*\|[[:space:]]*(tail|head)\b' tests/suites.txt || true
    bad=1
else
    say OK "没有以管道收尾的套件命令"
fi

echo "== ② C 级套件必须真的把退出码传出来（「rc=\$?」 + 「[ \"\$rc\" -eq 0 ]」）=="
for name in optest delibtest dertest biastest microtest dupcheck; do
    cmd=$(grep -E "^${name}\|" tests/suites.txt | head -1 | cut -d'|' -f2- || true)
    if [ -z "$cmd" ]; then
        say FAIL "清单里找不到 C 级套件 $name"
        bad=1
        continue
    fi
    ok_rc=0; ok_cmp=0
    case "$cmd" in *'rc=$?'*)      ok_rc=1 ;; esac
    case "$cmd" in *'[ "$rc" -eq 0 ]'*) ok_cmp=1 ;; esac
    if [ "$ok_rc" -eq 1 ] && [ "$ok_cmp" -eq 1 ]; then
        say OK "$name 的退出码真的决定红绿"
    else
        say FAIL "$name 的退出码可能被吃掉：$cmd"
        bad=1
    fi
done

echo "== ③ \「set -e\」 下不许有没兜住的 \「x=\$(…grep…)\」 赋值（会让整套静默消失）=="
# 判据：赋值的命令替换里，**最后一段**以 grep 开头，而且没有 `|| true` / `|| echo` / `|| :`
hits=$(awk '
    /^[A-Za-z_][A-Za-z0-9_]*=\$\(/ {
        line = $0
        body = line
        sub(/^[^=]*=\$\(/, "", body)
        sub(/\)[[:space:]]*$/, "", body)
        if (body ~ /\|\|[[:space:]]*(true|echo|:)/) next
        n = split(body, seg, /\|/)
        last = seg[n]
        gsub(/^[[:space:]]+/, "", last)
        if (last ~ /^grep/) print FILENAME ":" FNR ": " line
    }' tests/*.sh || true)
if [ -n "$hits" ]; then
    say FAIL "有没兜住的 grep 赋值（加 \「|| true\」）"
    printf '%s\n' "$hits"
    bad=1
else
    say OK "没有没兜住的 grep 赋值"
fi

echo "== ④ 每个套件脚本都在清单里、清单里的脚本都存在（别漏进/漏出）=="
for f in tests/*.sh; do
    b=${f#tests/}; b=${b%.sh}
    case "$b" in
        all|pool|prebuilt|probes_build|switches_ab|selfcheck|*_ab|meas_*|train_model) continue ;;
    esac
    if ! grep -qE "^${b}\|" tests/suites.txt; then
        say FAIL "套件脚本 $f 没进 tests/suites.txt"
        bad=1
    fi
done
for cmd in $(grep -v '^[[:space:]]*#' tests/suites.txt | grep -v '^[[:space:]]*$' | cut -d'|' -f2-); do
    for tok in $cmd; do
        case "$tok" in
            tests/*.sh)
                if [ ! -f "$tok" ]; then say FAIL "清单里的脚本不存在：$tok"; bad=1; fi ;;
        esac
    done
done
[ "$bad" -eq 0 ] && say OK "清单与脚本对得上" || true

echo
if [ "$bad" -eq 0 ]; then
    echo "PASS selfcheck（看护的看护：清单与脚本写法没这三类坑）"
else
    echo "FAIL selfcheck（上面列的问题会让整包汇总骗人，先修它们）"
    exit 1
fi
