#!/bin/sh
# code/build_code_model_full.sh — **扩展版**代码知识模型（阶段四扩图；§6.709）
#
# 和 `build_code_model.sh` 的区别：这份吃**更多离线真知识**，产出也更大（136 MB 量级）：
#   · `code/knowledge/skeletons.triples` + `compose.triples`  手写骨架/片段/模板（同默认）
#   · `code/knowledge/capi.triples`       `/usr/include` 的 C API（同默认；「头」关系 = 公共 API）
#   · `code/knowledge/capi_kern.triples`  内核头文件（**只用最新一版**；「声明于」关系，不冒充公共 API）
#   · `code/knowledge/man.triples`        man 手册（**用途** 一句话 + 第二来源的原型/头）
#
# 为什么单有一份、不直接改默认模型：默认模型是回归基线（73 套 + codebench 的门都按它的规模定的），
# 扩到 1.5×10⁶ 边以后载入 **2.6 s** / RSS **1.1 GB** —— 已经超过 M3 的 ≤2 s / ≤1 GB，
# 要等阶段三 M2-3 的「边打包 + 字符串池」才谈得上当默认。这一份是"能扩到多大、代价多少"的证据。
#
# 用法：sh code/build_code_model_full.sh [输出文件] [--rebuild]
#   默认输出 `code/code_model_full.bin`（生成物，不进仓库）。
#   环境变量 KERN_ROOT 可换内核头文件根（默认最新一版 6.8.0-138）。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# ⚠ 参数顺序随便写：`--rebuild` 不是文件名（第一版把它当成了输出路径，
#   结果存出一个叫 `--rebuild` 的 128 MB 文件 —— 顺手把这条坑记在这儿）
OUT=code/code_model_full.bin
REBUILD=0
for a in "$@"; do
    case "$a" in
    --rebuild) REBUILD=1 ;;
    ""|-) ;;
    *) OUT=$a ;;
    esac
done
KERN_ROOT=${KERN_ROOT:-/usr/src/linux-hwe-6.8-headers-6.8.0-138}

[ -s model.bin ] || { echo "FAIL: 缺 model.bin"; exit 1; }
[ -s code/knowledge/skeletons.triples ] || { echo "FAIL: 缺 skeletons.triples"; exit 1; }
[ -s code/knowledge/compose.triples ] || { echo "FAIL: 缺 compose.triples"; exit 1; }

# ---- 1. 公共 C API（/usr/include） ----
if [ "$REBUILD" = 1 ] || [ ! -s code/knowledge/capi.triples ]; then
    echo "== 1/4 抽 /usr/include =="
    sh tools/headers2triples.sh code/knowledge/capi.triples 100000000
fi

# ---- 2. 内核头文件（只用**最新一版**：/usr/src 里有 5.15 + 6.8×3 四份几乎一样的拷贝） ----
if [ "$REBUILD" = 1 ] || [ ! -s code/knowledge/capi_kern.triples ]; then
    if [ -d "$KERN_ROOT" ]; then
        echo "== 2/4 抽内核头文件（$KERN_ROOT）=="
        INC="$KERN_ROOT" sh tools/headers2triples.sh code/knowledge/capi_kern.raw.triples 100000000
        # ⚠ 关键的一步：内核头文件里的 `函数:printf 头 arch/um/.../kern.h` **不是**"printf 属于那个头"
        #   —— 那只是内核自己声明了一遍。直接灌进去会把「头」这个关系污染成"谁声明过就归谁"。
        #   所以这里把「头」降级成「声明于」：公共 API 的归属仍然只认 /usr/include。
        awk -F'\t' 'BEGIN{OFS="\t"} $2=="头"{$2="声明于"} {print}' \
            code/knowledge/capi_kern.raw.triples > code/knowledge/capi_kern.triples
        rm -f code/knowledge/capi_kern.raw.triples
        printf '  内核知识：%s 行（「头」已降级成「声明于」）\n' "$(wc -l < code/knowledge/capi_kern.triples)"
    else
        echo "== 2/4 跳过（没有 $KERN_ROOT；KERN_ROOT=… 可指定）=="
        : > code/knowledge/capi_kern.triples
    fi
fi

# ---- 3. man 手册（用途 + 第二来源） ----
if [ "$REBUILD" = 1 ] || [ ! -s code/knowledge/man.triples ]; then
    echo "== 3/4 抽 man 手册 =="
    sh tools/man2triples.sh code/knowledge/man.triples
fi

# ---- 4. 灌图 + 存盘 ----
echo "== 4/4 灌图 + 存盘 =="
mkdir -p _t
cp model.bin "$OUT.work"
printf '::ingest code/knowledge/skeletons.triples\n::ingest code/knowledge/compose.triples\n::ingest code/knowledge/capi.triples\n' > _t/fullbuild.in
[ -s code/knowledge/capi_kern.triples ] && printf '::ingest code/knowledge/capi_kern.triples\n' >> _t/fullbuild.in
[ -s code/knowledge/man.triples ] && printf '::ingest code/knowledge/man.triples\n' >> _t/fullbuild.in
printf '::save %s\n::stats\n::quit\n' "$OUT" >> _t/fullbuild.in
/usr/bin/time -f '  灌图+存盘墙钟 %e s（峰值 RSS %M KB）' ./build_aip --model "$OUT.work" < _t/fullbuild.in 2>&1 \
    | grep -aE '导入|符号节点|墙钟|已保存'
rm -f "$OUT.work" _t/fullbuild.in

echo "== 生成的模型 =="
ls -l "$OUT" | awk '{printf "  %s：%s 字节\n", $9, $5}'
echo "== 载入 / 存盘 / 查询 =="
printf '::quit\n' | /usr/bin/time -f '  载入（惰性）：%e s，RSS %M KB' ./build_aip --model "$OUT" 2>&1 >/dev/null | tail -1
printf '::save /tmp/_full_rt.bin\n::quit\n' | /usr/bin/time -f '  存盘：%e s' ./build_aip --model "$OUT" 2>&1 >/dev/null | tail -1
rm -f /tmp/_full_rt.bin
printf '::rel 函数:printf\n::quit\n' | ./build_aip --model "$OUT" 2>&1 | grep -a '函数:printf' | head -6 | sed 's/^/  /'
