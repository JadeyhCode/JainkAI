#!/bin/sh
# tools/frames2triples.sh — 把 `frames.txt`（帧/外壳）生成**图谱三元组**（§6.767）
#
# 为什么：帧现在是 `FrameSet` 结构（运行时快），图里查不到。生成三元组导进图后，
#   `::rel 帧库` 就能列出所有帧、`::rel 帧:喜欢X吗` 能看到它的模板 —— 帧知识也在图里。
#
# 产出（stdout）：
#   帧库\t帧\t<帧名>            每个帧一条
#   帧:<帧名>\t问法\t<模板行>    该帧的问法模板（去掉 <X> 占位符，空格换下划线）
#
# 用法：sh tools/frames2triples.sh > /tmp/frames.triples
set -u
F=${1:-frames.txt}
[ -s "$F" ] || { echo "FAIL: 没有 $F" >&2; exit 1; }
awk '
  /^\[帧:/ {
    name = $0
    sub(/^\[帧:/, "", name); sub(/\].*$/, "", name)
    print "帧库\t帧\t帧:" name
    cur = name; next
  }
  /^\[槽:/ { next }
  /^\[/ { cur = ""; next }
  cur != "" && $0 != "" && $0 != "---" {
    # 模板行：把空格换成下划线、去掉 <X>，避免三元组被空格打断
    t = $0
    gsub(/<X>/, "X", t)
    gsub(/ /, "_", t)
    print "帧:" cur "\t问法\t" t
  }
' "$F" | sort -u
