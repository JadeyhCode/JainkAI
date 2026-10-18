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


echo "== ⑤ 断言串别撞上**帮助文本**（本会话踩了 8 次：grep 命中帮助而不是痕）=="
# 做法：把 `::help` 的输出抓下来当"帮助语料"，再把 tests/ 里 `grep -q '…'` 的模式串抽出来比。
# 撞上的（且不在白名单里）= 假绿风险 ⇒ 报出来。白名单：`tests/help_overlap_ok.txt`（一行一个，
# 那些**故意**断言帮助文本的条目）。
if [ -x ./build_aip ]; then
    # ⚠ 只要**帮助文本**：整段跑完的输出里还有加载横幅和退出时的 `[统计]` 块 ——
    #   那些不是帮助（第一版把统计也算进去 ⇒ "现场造词0" 这类断言全被误报）。
    # ⚠ 两处都踩过：① 不带 `--model` 时帮助根本不打（那一次dump只有 1 行）；
    #   ② 输出里**统计块在最前面**（加载时就打了），所以不能"遇 [统计] 就停" ——
    #   改成"从第一条 `  ::` 开始抓，遇到下一个 `[` 开头的行停"。
    printf '::help\n::quit\n' | ./build_aip --model model.bin 2>&1 \
        | awk 'BEGIN{c=0} /^  ::/{c=1} c&&/^\[/{exit} c{print}' > _t/par/help_dump.txt || true
    awk '
      {
        line = $0
        if (line !~ /grep/ || line !~ /-q/) next
        # 只看 **&& 之前**那半句：断言串在 grep 里，后半句是 `ok "说明文字"`（说明不是模式串）。
        # 第一版没切 ⇒ 把"说明文字"也当成模式串比，白名单永远对不上（实测）。
        sub(/&&.*$/, "", line)
        # 抽出所有单引号串（模式串通常写在那里）
        n = split(line, parts, "\047")
        for (i = 2; i <= n; i += 2) {
            p = parts[i]
            # 门槛：**至少 4 个汉字**（12 字节）—— 短词（"校验""矛盾"）在帮助里到处都是，
            # 那不是"撞车"，是噪音（第一版没设门槛 ⇒ 42 条假阳性）。
            if (length(p) < 12) continue
            if (p ~ /^[ -~]+$/) continue          # 纯 ASCII 的（多为正则/路径）不算
            print FILENAME ":" FNR ":" p
        }
      }' tests/*.sh | sort -u > _t/par/assert_pats.txt || true
    : > _t/par/help_overlap.txt
    if [ -s _t/par/assert_pats.txt ]; then
        while IFS= read -r row; do
            pat=${row#*:*:}
            if grep -Fq -- "$pat" _t/par/help_dump.txt; then
                case "$pat" in
                    *::*) continue ;;                        # 显式提到命令的，算故意
                esac
                # 白名单：`模式<TAB>理由`（只比第一列）—— 故意断言帮助/文档格式的那几条
                if [ -f tests/help_overlap_ok.txt ] && \
                   awk -F'\t' -v p="$pat" '$1==p{found=1} END{exit found?0:1}' tests/help_overlap_ok.txt; then
                    continue
                fi
                printf '%s\n' "$row" >> _t/par/help_overlap.txt
            fi
        done < _t/par/assert_pats.txt
        n_over=$(grep -c . _t/par/help_overlap.txt || true)
        if [ "${n_over:-0}" -gt 0 ]; then
            say FAIL "有 $n_over 条断言的模式串**也出现在 ::help 里**（可能 grep 到帮助 = 假绿）"
            head -12 _t/par/help_overlap.txt
            say NOTE "确实是故意断言帮助文本的 → 加进 tests/help_overlap_ok.txt；否则改断言串"
            bad=1
        else
            say OK "没有"断言串撞帮助文本"（$(grep -c . _t/par/assert_pats.txt || true) 条模式串都比过了）"
        fi
    else
        say OK "没抽到可比的中文断言串"
    fi
else
    say NOTE "没有 build_aip ⇒ 跳过（第 0 步会先编，编完再跑这一项）"
fi

echo "== ⑥ 对外文件里不许出现**本机绝对路径 / 凭据形态**（借 §6.88 那条 check_local_paths）=="
# 为什么要有：仓库可能被推到公开处（本项目 2026-10-11 就推了 JainkAI）——
# 个人家目录路径会把用户名/机器布局暴露出去，凭据形态更是绝对不能有。
# 判据：扫**被跟踪的文件**（git ls-files；不是 git 仓库就退回 find）。
# ⚠ 只看**被跟踪的文件**（= 会被推出去的东西）。第一版在"不是 git 仓库"时退回 find，
#   于是把本机未跟踪的构建目录也算进来了（实测假红：cmake-build-debug 根本没进版本库）。
files=$(git ls-files 2>/dev/null)
if [ -z "$files" ]; then
    say NOTE "没有跟踪文件（不是 git 仓库？）⇒ 这一项跳过"
fi
hits=0
for pat in "/home/[A-Za-z0-9._-]*/" "/Users/[A-Za-z0-9._-]*/" "ghp_[A-Za-z0-9]\{20,\}" "github_pat_" "BEGIN [A-Z ]*PRIVATE KEY"; do
    out=$(printf '%s\n' "$files" | while IFS= read -r f; do
              [ -f "$f" ] || continue
              grep -nE -- "$pat" "$f" 2>/dev/null | sed "s|^|$f:|"
          done | grep -v '/\?selfcheck\.sh:' | head -6)   # 检查自己的文件里当然有那些"模式串"

    if [ -n "$out" ]; then
        say FAIL "对外文件里命中「$pat」："
        printf '%s\n' "$out" | head -6
        hits=1
    fi
done
if [ "$hits" -eq 0 ]; then
    say OK "没有本机绝对路径、没有凭据形态（扫了 $(printf '%s\n' "$files" | grep -c . ) 个被跟踪文件）"
fi

echo "== ④ 每个套件脚本都在清单里、清单里的脚本都存在（别漏进/漏出）=="
for f in tests/*.sh; do
    b=${f#tests/}; b=${b%.sh}
    case "$b" in
        # 这些是**工具**不是套件：不进 suites.txt（新加工具脚本记得加到这里）
        all|pool|prebuilt|probes_build|switches_ab|switches_sample|selfcheck|*_ab|meas_*|train_model|holdout|conv) continue ;;
        # holdout/conv 是**度量**不是套件：它们允许失败（失败清单就是待办），永远"PASS"。
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

echo "== ⑦ 文档里引用的 §6.x 小节必须真的存在（摘要会腐坏：引用指空最常见）=="
_tmp=$(mktemp -d)
{ grep -hoE "§6\.[0-9]+" docs/*.md CHANGELOG.md README.md 2>/dev/null || true; } \
    | sed -E 's/§//; s/^(6\.[0123456789]+).*/\1/' | sort -u > "$_tmp/refs"
{ grep -hoE "^#{2,4} §?6\.[0123456789]+" docs/audit*.md 2>/dev/null || true; } \
    | sed -E 's/^#+ //; s/^§//' | sort -u > "$_tmp/secs"
orph=$(comm -23 "$_tmp/refs" "$_tmp/secs" || true)
if [ -n "$orph" ]; then
    say FAIL "有引用了但审计里找不到的小节：$(printf '%s ' $orph)"
    bad=1
else
    say OK "文档里 $(wc -l < "$_tmp/refs" | tr -d ' ') 个 §6.x 引用都有对应小节"
fi
rm -rf "$_tmp"

echo "== ⑧ 文档里写的 \「::命令\」必须在命令表里（改名/删命令时文档会指空）=="
_tmp2=$(mktemp -d)
# 前面**不能紧挨标识符**：`Interp::stmt_start` 这种 C++ 作用域不是命令（§6.197 踩过）
{ grep -rhoE "(^|[^A-Za-z0-9_])::[a-z_0-9]+" docs/*.md README.md CHANGELOG.md 2>/dev/null || true; } \
    | sed -E 's/.*::([a-z_0-9]+)/\1/' | sort -u > "$_tmp2/doc"
# 名字与别名分开抽：表项形如 { "name", alias|NULL, arg, group, help, CMD }
{
    grep -oE '^[[:space:]]*\{ "[a-z_0-9]+"' main.c 2>/dev/null | sed -E 's/.*"([a-z_0-9]+)"/\1/' || true
    grep -oE '^[[:space:]]*\{ "[a-z_0-9]+", "[a-z_0-9]+"' main.c 2>/dev/null | sed -E 's/.*, "([a-z_0-9]+)"/\1/' || true
} | sort -u > "$_tmp2/real"
# 白名单：历史名（改名前的）/ 示例 / 文档里明确说"没有这个命令"的
# 白名单（每条都写清为什么）：
#   affect / aidump —— 改名前的历史名（文档里明说"改名为 drive.h / 已移除"）
#   forget          —— 旧文里写的旧命令（现为 ::gc）
#   note            —— `::note 删` 指的是**补丁操作**，不是元命令
#   pron            —— 旧文写作 `::pron last`（现为 `::others pron`；历史小节点不动）
#   delo            —— §6.198 记录里**引用过这个错拼**（真命令是 ::delib）；记录本身不改
#   lease / prune / relx / x / xxx —— 文档里明确说"没有这个命令"或纯示例
#   drift           —— §6.299 删掉了 `::drift`（hv_prev 恒等 ⇒ 漂移恒 0）；文档里是**历史记录**，不改
cat > "$_tmp2/ok" <<'OK'
affect
aidump
delo
drift
forget
lease
note
pron
prune
relx
x
xxx
OK
orp=$(comm -23 "$_tmp2/doc" "$_tmp2/real" | comm -23 - "$_tmp2/ok" || true)
if [ -n "$orp" ]; then
    say FAIL "文档里引用了命令表里没有的命令：$(printf '%s ' $orp)"
    bad=1
else
    say OK "文档里 $(wc -l < "$_tmp2/doc" | tr -d ' ') 个 ::命令 引用都在命令表里（或白名单里）"
fi
rm -rf "$_tmp2"

echo
if [ "$bad" -eq 0 ]; then
    echo "PASS selfcheck（看护的看护：清单/脚本/文档引用的 8 类坑都没踩）"
else
    echo "FAIL selfcheck（上面列的问题会让整包汇总骗人，先修它们）"
    exit 1
fi
