#!/bin/sh
# tests/prebuilt.sh — 套件要用的二进制从哪来（**唯一一份实现**）
#
# 为什么有这一份：原先 9 个套件各自 `gcc -O2 "$ROOT/main.c"`（各 ~16 s，实测），
# 而 tests/all.sh 第 0 步刚刚编过同一个 main.c —— 套件相位 40~47 s 里大半是这 9 遍重复编译。
# 现在第 0 步把编好的二进制 + **源码内容哈希**传给套件（环境变量 AIP_PREBUILT / AIP_PREBUILT_HASH），
# 套件直接复制，不再各编一遍。
#
# 三条规矩（缺一条就会掉进"拿旧二进制量出新结论"这个已经踩过的坑）：
#   · **直接跑单套**（没设 AIP_PREBUILT）→ 行为不变：自己编。
#     不偷偷用仓库根可能过期的 `./build_aip` —— 单跑时"改了源码就编新的"这条必须保住。
#   · **哈希对不上就自己编**：预置件是拿旧源码编的、或头文件改过了 → 自己编并打一行说明。
#   · **FORCE_CC=1** 强制自己编（"改了源码，先确认它真的编得过"时用；也顺带看护这条路径）。
#
# 用法：sh tests/prebuilt.sh <输出名> <源文件> [编译选项...]
#   例：sh tests/prebuilt.sh aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
out=$1
src=$2
shift 2

if [ -z "${FORCE_CC:-}" ] && [ -n "${AIP_PREBUILT:-}" ] && [ -x "$AIP_PREBUILT" ]; then
    dir=$(dirname "$src")
    want=${AIP_PREBUILT_HASH:-}
    # 哈希口径与 tests/probes_build.sh 的探针缓存同一套：main.c + 全部头文件的内容
    have=$(cat "$dir"/*.h "$dir"/*.c 2>/dev/null | md5sum | cut -d' ' -f1)
    if [ -z "$want" ] || [ "$want" = "$have" ]; then
        if cp -f "$AIP_PREBUILT" "$out"; then
            echo "  （复用第 0 步编好的二进制：源码哈希 $have 未变）"
            exit 0
        fi
        echo "  （复制预置二进制失败：$AIP_PREBUILT → 自己编）"
    else
        echo "  （预置二进制与源码哈希不符：$want ≠ $have → 自己编）"
    fi
fi

CC=${CC:-gcc}
exec $CC "$@" -o "$out" "$src" -lm
