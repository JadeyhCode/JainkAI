#!/bin/sh
# tests/assocaudit.sh — **边数组收口看护**（§6.565）
#
# 为什么要有这一条：多智能体 / 局部加载要求"边可以暂时不在内存里"（指针打 `AIP_PTR_DISK` 标签，
# 见 `ptrtag.h`）。只要还有代码**直接**摸 `t->assocs`，那段代码在"边在磁盘上"时就会解引用一个
# 带标签的指针 —— 直接崩。所以：**读边只许走 `token_assocs()` / `token_assocs_ro()`**。
#
# 例外只有两个（都是**存储层**，它们本来就该直接管那个指针）：
#   · `token.h`      —— 数组的分配/增长/释放；
#   · `model_io.h`   —— 存盘/读盘/装载期校验。
#
# 判据：除这两个文件外，`->assocs` 的**直接访问处数必须为 0**。新增代码破了就红。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

bad=0
total=0
for f in *.h *.c; do
    case "$f" in
    token.h|model_io.h) continue ;;   # 存储层：允许
    esac
    # 注释行（* / /* / //）里的 `->assocs` 不算 —— 那是文档。只认真代码。
    hits=$(grep -n '\->assocs' "$f" 2>/dev/null | while IFS=: read -r ln rest; do
               case "$rest" in
               *'->assocs'*) ;;
               *) continue ;;
               esac
               st=$(printf '%s' "$rest" | sed 's/^[ \t]*//')
               case "$st" in
               '* '*|'/*'*|'//'*) continue ;;
               esac
               printf '%s\n' "$ln"
           done)
    n=$(printf '%s\n' "$hits" | grep -c '[0-9]' || true)
    [ "${n:-0}" = "0" ] && continue
    total=$((total + n))
    echo "  FAIL $f 还有 $n 处直接访问 ->assocs（改用 token_assocs / token_assocs_ro）"
    printf '%s\n' "$hits" | head -5 | sed 's/^/         行 /'
    bad=1
done

if [ "$bad" -eq 0 ]; then
    # 顺便报一下"已经收口了多少处"，让进度可见（这个数字只许涨）
    ok=$(grep -o 'token_assocs\(_ro\)\?(' *.h *.c 2>/dev/null | wc -l | tr -d ' ')
    echo "  OK  除存储层（token.h / model_io.h）外，没有直接访问 ->assocs 的代码"
    echo "  OK  已经走访问器的处数：$ok（只许涨）"
    echo "PASS assocaudit（边数组收口：读边只走 token_assocs）"
    exit 0
fi
echo "FAIL assocaudit（$total 处直接访问没走访问器 —— 局部加载接不上）"
exit 1
