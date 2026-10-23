#!/bin/sh
# tests/patcount.sh — **数据桶计数看护**（§6.352；§6.379/§6.388 两次加固）
#
# 五次踩过"桶满了静默丢弃"（shape 8/32/64、reply 64…）。这条把"文件里有几行"
# 和"加载后有几条"钉在一起：任何一个桶对不上就红。
#
# 检查项：
#   ① 每个出现在数据里的桶：加载数 == 文件行数（shape 含自动变体，只查不小于）；
#   ② 每个桶在 `tests/bucketload.sh` 里必须有一条"关掉就变"的探针；
#   ③ `[shape]` 的 **rhs** 不许指向"只有改写之前才认"的形状（§6.388）：
#      量/尺度（多大/多重…）、名字反查（是谁/是啥）、保证请求（你确定/真的吗）。
#      —— 那些判据排在 `[shape]` 之前，把句子改写成它们是白改（§6.384/§6.386 踩过两次）。
#
# `--selftest`：**用一份故意写坏的数据验证 ③ 真的会红**（§6.389）。
# 第一版 ③ 因为"数据里改写是空格分隔"而漏判，是手工加坏行才发现的；现在自动化了。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
BIN=${BIN:-./build_aip}
[ -x "$BIN" ] || { echo "FAIL: 缺 $BIN"; exit 1; }
[ -f patterns.txt ] || { echo "FAIL: 缺 patterns.txt"; exit 1; }

# ---- ③ 的扫描逻辑（抽成函数：主流程和自检共用一份）----
scan_bad_rhs() {   # $1 = 数据文件；有违规行 → 打印并返回 1
    _bad=0
    while IFS= read -r line; do
        case "$line" in \[shape\]*) ;; *) continue ;; esac
        rhs=${line#*=}
        rhs=$(printf '%s' "$rhs" | tr -d ' ')   # 数据里的改写是空格分隔的，比较前去掉
        for m in 多大 多高 多重 多长 多远 多久 多快 多贵 多深 是谁 是啥 你确定 真的吗; do
            case "$rhs" in
            *"$m"*)
                echo "  FAIL rhs 里出现「$m」：那类判据看的是**改写前**的输入（§6.384/§6.388）→ $line"
                _bad=$((_bad + 1)) ;;
            esac
        done
    done < "$1"
    [ "$_bad" -eq 0 ] && return 0 || return 1
}

check_probes() {   # $1=数据文件 $2=bucketload 文件；缺探针 → 打印并返回 1
    _miss=0
    for k in $(grep -oE '^\[[a-z]+\]' "$1" | tr -d '[]' | sort -u); do
        if grep -qE "^${k}\|" "$2" 2>/dev/null; then :; else
            echo "  FAIL 桶 [$k] 没有探针：往 $(basename "$2") 的表里补一行「${k}|<探针输入>」"
            _miss=$((_miss + 1))
        fi
    done
    [ "$_miss" -eq 0 ] && return 0 || return 1
}

# §6.396 词类词覆盖（**提示**，不算失败）：类里哪些词从没在任何测例/探针里出现过
class_word_coverage() {   # 无参数；打印"未出现"的词
    _blob=$(mktemp)
    cat tests/*.txt tests/*.sh 2>/dev/null > "$_blob" || true
    while IFS= read -r line; do
        case "$line" in \[class\]*) ;; *) continue ;; esac
        nm=${line#\[class\] }; nm=${nm%%=*}
        words=${line#*=}
        miss=""
        for w in $(printf '%s' "$words" | tr ',' ' '); do
            grep -Fq -- "$w" "$_blob" || miss="$miss $w"
        done
        [ -n "$miss" ] && echo "  [提示] 类 $nm 里这些词没在任何测例里出现：$miss"
    done < patterns.txt
    rm -f "$_blob"
}

check_class_refs() {   # $1=数据文件；有未定义 @类名 → 打印并返回 1
    _cls=$(grep -oE '^\[class\][^=]*=' "$1" | sed 's/^\[class\] *//; s/=$//' | tr -d ' ' | sort -u)
    _bad=0
    while IFS= read -r line; do
        case "$line" in \[reply\]*) ;; *) continue ;; esac
        lhs=${line%%=*}
        lhs=${lhs#\[reply\] }
        for tok in $(printf '%s' "$lhs" | tr ',|' '  '); do
            case "$tok" in
            @*)
                nm=${tok#@}
                if ! printf '%s\n' "$_cls" | grep -qx -- "$nm"; then
                    echo "  FAIL [reply] 引用了不存在的词类 @$nm：$line"
                    _bad=$((_bad + 1))
                fi ;;
            esac
        done
    done < "$1"
    [ "$_bad" -eq 0 ] && return 0 || return 1
}

scan_hard_entity() {   # $1=数据文件；lhs 里写死实体 → 打印并返回 1（§6.490）
    _hard=$(awk '/^\[shape\]/{line=$0; sub(/#.*/,"",line); sub(/=.*/,"",line);
                          if (line ~ /猫|狗|鲸鱼|鸟|鱼|猪/) print line}' "$1")
    if [ -z "$_hard" ]; then return 0; fi
    printf '%s\n' "$_hard" | sed 's/^/  FAIL 写死实体: /'
    return 1
}

scan_space_kw() {   # $1=数据文件；有内部空格 → 打印并返回 1
    _n=$(awk -F'=' '
        /^\[reply\]/ {
            lhs = $1
            sub(/^\[reply\][ \t]*/, "", lhs)
            n = split(lhs, groups, ",")
            for (i = 1; i <= n; i++) {
                m = split(groups[i], alts, "|")
                for (j = 1; j <= m; j++) {
                    t = alts[j]
                    gsub(/^[ \t]+|[ \t]+$/, "", t)
                    if (t ~ /[ \t]/) { print "  FAIL 关键词里有空格（永远不命中）：" $0 > "/dev/stderr"; bad++ }
                }
            }
        }
        END { print bad + 0 }
    ' "$1")
    [ "$_n" -eq 0 ] && return 0 || return 1
}

# ---- 自检：故意写坏一行，验证 ③ 会红 ----
if [ "${1:-}" = "--selftest" ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    cp patterns.txt "$tmp/clean.txt"
    cp patterns.txt "$tmp/bad.txt"
    printf '[shape] $1@k 的 体 重 = $1 有 多 重\n' >> "$tmp/bad.txt"
    if ! scan_bad_rhs "$tmp/clean.txt" >/dev/null; then
        echo "FAIL selftest：干净数据也被判违规（误报）"; exit 1
    fi
    if scan_bad_rhs "$tmp/bad.txt" >/dev/null; then
        echo "FAIL selftest：违规行**没被抓到**（这条检查是摆设）"; exit 1
    fi
    # ② 也要能红：从探针表里删掉 fill 那一行，必须报"没有探针"
    grep -v '^fill|' tests/bucketload.sh > "$tmp/bucketload_missing.sh" || true
    if check_probes patterns.txt "$tmp/bucketload_missing.sh" >/dev/null; then
        echo "FAIL selftest：删掉 fill 的探针后 ② 没红（检查是摆设）"; exit 1
    fi
    if ! check_probes patterns.txt tests/bucketload.sh >/dev/null; then
        echo "FAIL selftest：真实探针表被判缺探针（误报）"; exit 1
    fi
    # ⑨ 也要能红：临时加一条写死实体的 [shape]
    cp patterns.txt "$tmp/badent.txt"
    printf '[shape] 猫 算 动物 吗 = 猫 属于 动物 吗\n' >> "$tmp/badent.txt"
    if scan_hard_entity "$tmp/badent.txt" >/dev/null; then
        echo "FAIL selftest：写死实体的形状行没被抓到（⑨ 是摆设）"; exit 1
    fi
    if ! scan_hard_entity "$tmp/clean.txt" >/dev/null; then
        echo "FAIL selftest：干净数据被判写死实体（误报）"; exit 1
    fi
    # ④ 也要能红：临时加一条引用不存在词类的 [reply]
    cp patterns.txt "$tmp/badclass.txt"
    printf '[reply] @nope , 吗 = 不该 出现\n' >> "$tmp/badclass.txt"
    if check_class_refs "$tmp/badclass.txt" >/dev/null; then
        echo "FAIL selftest：引用不存在的词类没被发现（检查是摆设）"; exit 1
    fi
    if ! check_class_refs patterns.txt >/dev/null; then
        echo "FAIL selftest：真实数据被误判引用了不存在的词类"; exit 1
    fi
    # ⑥ 也要能红：临时加一条"关键词带空格"的行
    cp patterns.txt "$tmp/badspace.txt"
    printf '[reply] 有 什么 , 吗 = 测试\n' >> "$tmp/badspace.txt"
    if scan_space_kw "$tmp/badspace.txt" >/dev/null; then
        echo "FAIL selftest：关键词带空格没被发现（检查是摆设）"; exit 1
    fi
    if ! scan_space_kw patterns.txt >/dev/null; then
        echo "FAIL selftest：真实数据被判有关键词带空格（误报）"; exit 1
    fi
    echo "PASS patcount --selftest（干净数据通过 / 违规行被抓到 / 缺探针被抓到 / 坏词类引用被抓到 / 关键词空格被抓到）"
    exit 0
fi

got=$(AIP_PATDBG=1 timeout 30 "$BIN" --model model.bin </dev/null 2>&1 \
      | sed -n 's/.*\[patterns\] \(lead=.*\)/\1/p' | head -1)
[ -n "$got" ] || { echo "FAIL: AIP_PATDBG 没输出"; exit 1; }

fail=0
chk() {  # $1=桶名 $2=加载数 $3=文件行数
    if [ "$1" = "shape" ]; then
        [ "$2" -ge "$3" ] || { echo "  FAIL shape 加载 $2 < 文件 $3（被上限丢弃）"; fail=1; }
    else
        [ "$2" -eq "$3" ] || { echo "  FAIL $1 加载 $2 ≠ 文件 $3（被上限丢弃或解析失败）"; fail=1; }
    fi
}

echo "① 每个出现在数据里的桶：加载数 == 文件行数（shape 只查不小于）"
buckets=$(grep -oE '^\[[a-z]+\]' patterns.txt | tr -d '[]' | sort -u)
[ -n "$buckets" ] || { echo "FAIL: patterns.txt 里没读到任何桶"; exit 1; }
for k in $buckets; do
    n=$(printf '%s\n' "$got" | sed -n "s/.*\b$k=\([0-9]*\).*/\1/p" | head -1)
    if [ -z "$n" ]; then
        echo "  FAIL 桶 [$k] 在数据里有行，但 AIP_PATDBG 不报它（加载器不认识这个桶？）"
        fail=1
        continue
    fi
    f=$(grep -c "^\[$k\]" patterns.txt || true)
    chk "$k" "$n" "$f"
done

echo "② 每个桶在 bucketload.sh 里都要有探针（新桶必须补一行）"
if check_probes patterns.txt tests/bucketload.sh; then
    echo "  OK 每个桶都有探针"
else
    fail=1
fi

echo "③ [shape] 的 rhs 不许指向早期判据才认的形状"
if scan_bad_rhs patterns.txt; then
    echo "  OK 没有指向早期判据的改写"
else
    fail=1
fi

nb=$(printf '%s\n' "$buckets" | grep -c . || true)
echo "⑤ 词类词覆盖（提示，不算失败）"
class_word_coverage
echo "  （上面有 [提示] 说明那些词还没被任何测例走过，不代表坏；要验就补探针）"

# §6.414 [reply] 关键词里**不许有内部空格**：输入被去掉空格再比，关键词却按原文比 ⇒
# `有 什么` 这种永远匹配不上（踩过两次）。用 awk 按 , 和 | 切，**保留**组内空格再查。
echo "⑥ [reply] 关键词没有内部空格"
if scan_space_kw patterns.txt; then
    echo "  OK 没有带内部空格的关键词"
else
    fail=1
fi

echo "④ [reply] 引用的词类都真实存在"
if check_class_refs patterns.txt; then
    echo "  OK 所有 @类名 都有 [class] 行"
else
    fail=1
fi

# §6.471 **重复行会被静默丢弃**（同义词表 64 上限那次就是先出现重复，后来才发现）：
# 同一行写两遍不会"更有效"，只会让"文件行数 vs 加载数"对不上。这里直接查重复。
echo "⑦ 没有重复的数据行（重复会被静默丢掉一条）"
dups=$(awk '!/^#/ && NF {gsub(/[ \t]+$/, ""); if (seen[$0]++) print $0}' patterns.txt)
if [ -z "$dups" ]; then
    echo "  OK 没有重复行"
else
    printf '%s\n' "$dups" | sed 's/^/  FAIL 重复行: /'
    fail=1
fi

# §6.472 **容量余量提示**：`[syn]`(64→160) 与 `[shape]`(256→512→1024) 都被"上限太小"咬过。
# 这里从 agent.h 读出常量，算"已用/上限"，超过 85% 就提示（不判失败：还有空间，只是快满了）。
echo "⑧ 桶容量余量（超过 85% 提示，不判失败）"
# §6.478 形状 token 上限也列进来：它不是"桶行数"，而是"一行最多几个 token"，
# 超了会被**截断**（不是丢行），所以单独打印它 + 当前最长 lhs 的 token 数。
if [ -f agent.h ]; then
    stok=$(sed -n 's/^#define AIP_SHAPE_TOK \([0-9][0-9]*\).*/\1/p' agent.h | head -1)
    maxl=$(awk '/^\[shape\]/{line=$0; sub(/#.*/,"",line); sub(/=.*/,"",line); gsub(/[ \t]+/," ",line);
                              n=split(line,a," "); if(n>m){m=n}} END{print m+0}' patterns.txt)
    if [ -n "$stok" ]; then
        if [ "$maxl" -ge "$stok" ]; then
            echo "  [提示] shape 最长一行 $maxl token / 上限 $stok——超出的会被截断"
        else
            echo "  OK   shape 最长一行 $maxl token / 上限 $stok"
        fi
    fi
fi
for pair in "syn:AIP_SYN_MAX" "shape:AIP_SHAPE_MAX" "reply:AIP_PAT_MAX" "lead:AIP_PAT_MAX" "tail:AIP_TAIL_MAX"; do
    b=${pair%%:*}; macro=${pair##*:}
    cap=$(sed -n "s/^#define $macro \([0-9][0-9]*\).*/\1/p" agent.h | head -1)
    n=$(printf '%s\n' "$got" | sed -n "s/.*\b$b=\([0-9]*\).*/\1/p" | head -1)
    [ -n "$cap" ] && [ -n "$n" ] || continue
    pct=$(( n * 100 / cap ))
    if [ "$pct" -ge 85 ]; then
        echo "  [提示] $b=$n / $cap（${pct}%）——快撞上限了，超限会静默丢规则"
    else
        echo "  OK   $b=$n / $cap（${pct}%）"
    fi
done

# §6.490 **形状里不许写死实体**：写了"猫/狗/鲸鱼…"就等于把测试集当训练集（§6.488 犯过一次）。
# 只查 lhs（`=` 之前）里的实体字面；类别词（动物/哺乳动物）允许，但要人工确认不是写死。
echo "⑨ [shape] lhs 没有写死实体"
if [ -f patterns.txt ]; then
    if scan_hard_entity patterns.txt; then
        echo "  OK 没有写死实体的形状行"
    else
        fail=1
    fi
    # 类别词（动物/哺乳动物）不判失败，只报数：要人工看是"习语"还是"写死"
    cat_n=$(awk '/^\[shape\]/{line=$0; sub(/#.*/,"",line); sub(/=.*/,"",line);
                          if (line ~ /动物|哺乳/) c++} END{print c+0}' patterns.txt)
    [ "$cat_n" -gt 0 ] && echo "  [提示] 含类别词（动物/哺乳）的形状 $cat_n 条——确认是习语不是写死"
else
    echo "  NOTE 没有 patterns.txt"
fi

[ "$fail" -eq 0 ] && echo "PASS patcount（$nb 个桶全部对上，每个桶都有承重探针，且没有指向早期判据的改写）"
exit "$fail"
