#!/bin/sh
# tests/action.sh — **受审的外部执行 + 写文件**（`action.h` 层）
#
# 用户 2026-10-08 拍板：白名单 `echo / cat / wc / gcc` 再加 `grep / head / tail / sed / ls`
# （**不含 `sh`**）；写文件固定在 `_t/work/`；不联网；`AIP_ACT=1` / `::act on` 打开，**默认关**。
#
# 这一套盯七件事（每条都要能证伪）：
#   ① 默认关：**不执行**，而且**留理由**；
#   ② 白名单：`rm` 被拒，理由里写明只许哪些；
#   ③ 边界：绝对路径 / `..` / 写文件出工作区 → 拒（并且真的没写出去）；
#   ④ **没有 shell**：`;` `|` `&` 只是普通参数（原样进 argv，不会被解释）；
#   ⑤ 超时：卡住的命令被 SIGKILL（约定 rc = -9）——`popen` 做不到这件事；
#   ⑥ 输出算**观测**：落一个 `[运行:rc]` 标记节点；**不是知识**（关系表/::rel 里没有它）；
#   ⑦ 审计账对得上：跑 / 成功 / 非零 / 超时 / 拒 / 写 六个计数与上面一一对应。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/_t/action"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

# aip 从哪来只有一处实现：tests/prebuilt.sh（第 0 步编好的二进制优先，否则自己编）
sh "$ROOT/tests/prebuilt.sh" aip "$ROOT/main.c" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
[ -x aip ] || { echo "FAIL: aip 没编出来"; exit 1; }

mkdir -p _t/work
mkfifo _t/work/卡住的管道        # ⑤ 用：没人写它，`cat` 会一直等在 open() 上

pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; fail=$((fail+1)); }
has() { grep -qF -- "$1" out.log; }

echo "== ① 默认关 + ② 跑通 + ③ 拒（白名单/路径） + ④ 元字符 + ⑤ 超时 + ⑦ 账 =="
{
  printf '::act run echo 不该出现的东西\n'      # ① 层关着 → 拒
  printf '::act on\n'
  printf '::act run echo 你好\n'                # ② 跑通（输出进日志）
  printf '::act run rm -rf x\n'                 # ③ 白名单
  printf '::act run cat /etc/passwd\n'          # ③ 绝对路径
  printf '::act run cat ../../etc/passwd\n'     # ③ ..
  printf '::act run echo ";" "|" "&"\n'         # ④ 没有 shell：原样进 argv
  printf '::act write hello.c int main(void){return 0;}\n'
  printf '::act run cat hello.c\n'              # 写进去的能读回来
  printf '::act run gcc -c hello.c\n'           # 写→编译（"会写代码"那条路）
  printf '::act run gcc 没有这个文件.c\n'        # 非零退出也算观测
  printf '::act timeout 300\n'
  printf '::act run cat 卡住的管道\n'            # ⑤ 超时被杀
  printf '::act write ../越界.c x\n'             # ③ 写文件越界（..）
  printf '::act write /tmp/越界.c x\n'           # ③ 写文件越界（绝对路径）
  printf '::act\n'                              # ⑦ 账 + 白名单 + 最近一次
  printf '::quit\n'
} > in.txt
./aip < in.txt > out.log 2>&1 || true

# ① 默认关：留理由 + 真的没执行
#    ⚠ 开关反向测量（AIP_ACT=1，见 tests/switches_ab.sh）时这两条**自动跳过** ——
#    那时层是开着的，"默认关"的行为本来就不该出现（这是项目里成对开关的通用写法）。
if [ -z "${AIP_ACT:-}" ]; then
    has '这一层**默认关**' && ok "默认关：拒了，而且**留了理由**" \
        || bad "默认关的理由" "$(grep -m1 '没跑' out.log)"
    if grep -qE '^    不该出现的东西$' out.log; then bad "默认关时居然执行了（echo 的产物出现在日志里）"
    else ok "默认关时**没有执行**（echo 的产物一个字都没出现）"; fi
else
    has 'rc=0' && grep -qE '^    不该出现的东西$' out.log \
        && ok "AIP_ACT=1（反向测量）：层开着，那条 echo 真的跑了（行为与默认跑法**故意不同**）" \
        || bad "AIP_ACT=1 跑法" "$(grep -m2 '跑了' out.log)"
fi

# ② 跑通 + 输出是观测
has 'rc=0' && ok "打开后跑通（rc=0）" || bad "跑通" "$(grep -m2 '跑了' out.log)"
has '你好' && ok "输出被收下来（echo 的结果在日志里）" || bad "输出" "$(grep -m2 '跑了' out.log)"

# ③ 三类拒绝都要有理由
has '首词 rm 不在白名单里' && ok "白名单：rm 被拒，理由里写明只许哪些命令" \
    || bad "白名单理由" "$(grep -m2 '没跑' out.log)"
has '不许绝对路径：/etc/passwd' && ok "参数不许绝对路径（读也关在工作区里）" \
    || bad "绝对路径理由" "$(grep -m3 '没跑' out.log | tail -1)"
has '不许 ..（爬出工作区）：../../etc/passwd' && ok "参数不许 ..（爬出工作区）" \
    || bad ".. 理由" "$(grep -m4 '没跑' out.log | tail -1)"
has '不许 ..（爬出工作区）：../越界.c' && ok "写文件越界（..）也被拒" \
    || bad "写越界理由" "$(grep -m1 '没写' out.log)"
has '不许绝对路径：/tmp/越界.c' && ok "写文件越界（绝对路径）也被拒" \
    || bad "写绝对路径理由" "$(grep -m2 '没写' out.log | tail -1)"
[ ! -e 越界.c ] && [ ! -e /tmp/越界.c ] && ok "越界的那两个文件**真的没被写出来**" \
    || bad "越界写" "有文件落到工作区外面了"

# ④ 没有 shell：元字符只是普通参数
has '";" "|" "&"' && ok "没有 shell：; | & 原样当参数传下去（不会被解释成管道/串联）" \
    || bad "元字符" "$(grep -A2 'echo' out.log | tail -3)"

# ⑤ 写→读→编译：外部世界真的被用起来了
[ -f _t/work/hello.c ] && ok "写文件落在 _t/work/ 里（hello.c）" || bad "写文件" "$(ls -la _t/work)"
has 'int main(void){return 0;}' && ok "写进去的内容读得回来（cat hello.c）" || bad "读回" "$(grep -A2 'cat' out.log | tail -2)"
[ -f _t/work/hello.o ] && ok "gcc 真的把 hello.c 编成了 hello.o（写→跑→看结果）" || bad "编译产物" "$(ls -la _t/work)"
has 'gcc 退出码 1' && ok "编译失败也照实记成观测（rc=1，不是崩溃）" || bad "非零退出" "$(grep -m3 '跑了' out.log | tail -1)"

# ⑤ 超时
has 'SIGKILL' && ok "卡住的命令被 SIGKILL（理由里写明超时多少）" || bad "超时理由" "$(grep -m1 '卡住' out.log)"
has 'rc=-9' && ok "超时用约定退出码 -9 记账" || bad "超时 rc" "$(grep -m1 '卡住' out.log)"

# ⑦ 账：六个计数与上面一一对应
if [ -z "${AIP_ACT:-}" ]; then
    has '账：跑 6（成功 4 / 非零退出 1 / 超时 1）｜拒 6｜写文件 1（失败 0）｜观测标记 3 个' \
        && ok "审计账对得上（跑 6 = 成功 4 + 非零 1 + 超时 1；拒 6；写 1；标记 3）" \
        || bad "审计账" "$(grep -m1 '账：' out.log)"
else
    has '账：跑 7（成功 5 / 非零退出 1 / 超时 1）｜拒 5｜写文件 1（失败 0）｜观测标记 3 个' \
        && ok "审计账对得上（AIP_ACT=1 跑法：跑 7 = 成功 5 + 非零 1 + 超时 1；拒 5；写 1）" \
        || bad "审计账（AIP_ACT=1）" "$(grep -m1 '账：' out.log)"
fi
has 'echo cat wc gcc grep head tail sed ls' && ok "白名单表就是那一份（九个首词，没有 sh）" \
    || bad "白名单表" "$(grep -m1 '白名单' out.log)"

echo
echo "== ⑥ 输出变观测，但**不是知识**（关系表里不许有它） =="
{ printf '::act on\n::act run echo 观测检查\n::edges [运行:0]\n::rel 我\n::quit\n'; } \
    | ./aip > obs.log 2>&1 || true
grep -qF '落图：[运行:0]' obs.log && ok "跑完落了一个 [运行:0] 标记节点（观测）" \
    || bad "观测标记" "$(grep -m2 '跑了\|落图' obs.log)"
grep -qF '[运行:0]' obs.log && grep -q '→ 我' obs.log && ok "标记挂在「我」名下（归属，不是语义）" \
    || bad "标记挂靠" "$(grep -A2 '运行:0' obs.log | head -3)"
if grep -qE -- '--\[[^]]*\]-->.*运行' obs.log; then
    bad "观测标记漏进了关系表（::rel 里有它）" "$(grep -E -- '--\[[^]]*\]-->.*运行' obs.log | head -2)"
else
    ok "::rel 里**没有**它（观测不是知识 —— 不建关系边、不进关系表）"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS action（受审的外部执行 + 写文件：$pass 项；默认关/白名单/边界/无 shell/超时/观测/账）"
else
    echo "FAIL action（$fail 项失败，$pass 项通过）"; exit 1
fi
