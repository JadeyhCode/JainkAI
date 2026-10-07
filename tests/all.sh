#!/bin/sh
# tests/all.sh — 一把跑完全部回归
#
# 三十一套看护，各自盯一件事：
#   model_roundtrip  存/读无损，且加载后行为与原地训练等价（最重的一套）
#   features         对应 deepseek看这里.txt 的十项能力逐条核验
#   frames           结构化帧生成（语料里没写过的句子）
#   framegate_ab     帧读关联链产物（Gate B）的 A/B 与反面看护
#   coining          现场造词：没见过的复合词造成 Token 并现场接线
#   segmentation     切分可选：拆解 / 合并 / 中转词容错 / 碎片护栏
#   cot              思维链：骨架→校验→降级 咬合，降级答复可关
#   embed            L3 神经表示：emb 真学出来了 + 钉住它的边界
#   relations        L2 关系层：静态知识 / 传递推理 / 对话中现学知识 / 持久化
#   derivation       推导层：链结构化 / 验证器在验 / 缺口不断言 / 语言是投影
#   dertest          **验证器自身的测试**：伪造/非法/断链/假依据必须被拒
#   hypothesis       反事实：假设世界可推、不越界、能外推、**绝不进图**
#   logic4           四个局限：多层假设 / 独占健全否定 / 假设不被接管 / 嵌套+归属
#   exclusivity      「独占」由数据判定：可印证、可改口、改口后推理跟着变
#   belief           归属：每个人一份信念表（持久、不进我的知识、问他就查他的）
#   hypo_frames      假设进检索/帧层：与假设相反的话被否决，并记在链上
#   hypo_scope       假设的作用域回溯：按序号/主语/内容撤**指定**那层
#   prerequisite     缺前提 → 否定能力：训练数据里没有的结论也能推出来
#   microtest        微内核自测（C 级）：队列去重/预算/每条拒绝理由/检测器/补丁故障隔离/冻结
#   selfmod          微内核端到端（CLI）：::mod/::safe/::patch 三个入口 + 自修改结构随图存盘
#   delibtest        思维循环自测（C 级）：议程/反方向/矛盾/跨轮/铁律/元认知/UTF-8
#   thinking         思维循环端到端（CLI）：::delib/::pending/::thinkrun/::thinkout/::thinkbudget
#   consolidate      推理沉淀：想通的记下来（标成推出来的）/ 复用 / 观测压过 / 撤销 / 存读
#   cmdtable         元命令表：命令名只有一份（帮助由表生成），帮助里每条都真的认得
#   dupcheck         清单自检：疑问词/虚词/助词/规则/关系表里没有重复项，两张 frame 词表不分家
#   revision         观测改口语义：后说的替掉先说的（on/off A/B 对照 + 装载边界）
#   biastest         反糟粕自测（C 级）：一次巧合不是规律 / 反事实真削弱 / 不虚构 / 不自我印证 / 不空想造事实 / 不从众 / 不泛化
#   biases           反糟粕端到端（CLI）：因果要协变、不知道不说成知道、别人的看法不是我的知识
#   optest           Token 行为/临时 Token/修正的记忆自测（C 级）：接线/插词/中转/不存盘/预算真扣
#   tokenop          Token 行为端到端（CLI）：::op 接线插词、临时 Token 不存盘、修正的记忆持久
#
# 用法：sh tests/all.sh [语料文件]      默认 _corpus.txt
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
CORPUS=${1:-_corpus.txt}

echo "=== 0/31 编译（严格零警告 + 所有探针）==="
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_aip main.c -lm
echo "BUILD OK"
# 探针也要编译得过、跑得出结论 —— 它们是"把拍的变成量的"那件工具，自己不能没看护。
# （血泪：affectprobe 改 API 后编译失败，旧二进制照跑，量出来的数看着像真的。）
sh tests/probes_build.sh

fail=0
step() {   # $1=N/M  $2=名字  $3=命令
    echo "=== $1 $2 ==="
    out=$(sh -c "$3" 2>&1) && rc=0 || rc=$?
    printf '%s\n' "$out" | tail -2
    [ "$rc" -eq 0 ] || { echo "  ^^^ $2 失败（exit $rc）"; fail=1; }
}

step "1/31" "model_roundtrip" "sh tests/model_roundtrip.sh $CORPUS"
step "2/31" "features"        "sh tests/features.sh"
step "3/31" "frames"          "sh tests/frames.sh"
step "4/31" "framegate_ab"    "sh tests/framegate_ab.sh"
step "5/31" "coining"         "sh tests/coining.sh"
step "6/31" "segmentation"    "sh tests/segmentation.sh"
step "7/31" "cot"             "sh tests/cot.sh"
step "8/31" "embed"           "gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -o build_slotprobe tools/slotprobe.c -lm && sh tests/embed.sh"
step "9/31" "relations"       "sh tests/relations.sh"
step "10/31" "derivation"     "sh tests/derivation.sh"
step "11/31" "dertest"        "gcc -std=c11 -O2 -o build_dertest tools/dertest.c -lm && ./build_dertest model.bin | tail -12"
step "12/31" "hypothesis"    "sh tests/hypothesis.sh"
step "13/31" "logic4"        "sh tests/logic4.sh"
step "14/31" "exclusivity"   "sh tests/exclusivity.sh"
step "15/31" "belief"        "sh tests/belief.sh"
step "16/31" "hypo_frames"   "sh tests/hypo_frames.sh"
step "17/31" "hypo_scope"    "sh tests/hypo_scope.sh"
step "18/31" "prerequisite"  "sh tests/prerequisite.sh"
step "19/31" "microtest"     "gcc -std=c11 -O2 -Wall -Wextra -o build_microtest tools/microtest.c -lm && ./build_microtest | tail -3"
step "20/31" "selfmod"       "sh tests/selfmod.sh"
step "21/31" "delibtest"     "gcc -std=c11 -O2 -Wall -Wextra -o build_delibtest tools/delibtest.c -lm && ./build_delibtest | tail -3"
step "22/31" "thinking"      "sh tests/thinking.sh"
step "23/31" "consolidate"   "sh tests/consolidate.sh"
step "24/31" "cmdtable"      "sh tests/cmdtable.sh"
step "25/31" "dupcheck"      "gcc -std=c11 -O2 -Wall -Wextra -o build_dupcheck tools/dupcheck.c -lm && ./build_dupcheck | tail -3"
step "26/31" "revision"     "sh tests/revision.sh"
step "27/31" "biastest"      "gcc -std=c11 -O2 -Wall -Wextra -o build_biastest tools/biastest.c -lm && ./build_biastest | tail -3"
step "28/31" "biases"        "sh tests/biases.sh"
step "29/31" "optest"        "gcc -std=c11 -O2 -Wall -Wextra -o build_optest tools/optest.c -lm && ./build_optest | tail -3"
step "30/31" "tokenop"       "sh tests/tokenop.sh"

echo "=== 31/31 交互冒烟（读盘后各派生层是否都在 + 混合模式 + 开关）==="
# 硬断言：读盘之后**派生层**必须都在。这几层不进模型文件、靠加载器现读/现算，
# 曾经因为一次字符串替换把 ai_embed_build + frames_load 删掉，
# 症状只是「帧库空了」→ 帧层与思维链步骤静默消失，而 roundtrip 照样 PASS
# （它只比状态转储，不看这些层）。所以这里补一道硬门。
smoke=$(printf '::load model.bin\n::frames\n::rel 猫\n::quit\n' | ./build_aip 2>&1)
sfail=0
printf '%s\n' "$smoke" | grep -q '没有帧库' && { echo "  ^^^ 加载后帧库是空的（frames_load 没被调用？）"; sfail=1; }
printf '%s\n' "$smoke" | grep -q -- '--\[属于\]-->' || { echo "  ^^^ 加载后关系知识是空的（relations_load 没被调用？）"; sfail=1; }
[ "$sfail" -eq 0 ] || fail=1

printf '::load model.bin\n你好呀\n::free\n::framegate\n::coin\n::seg\n::relay\n::fall\n::near 猫\n::rel 猫\n::der\n::hypo\n::world\n::walk\n你想学画画吗\n::quit\n' \
    | ./build_aip 2>&1 | grep -E 'AI >|内部联想|帧读关联链|现场造词|换切分|中转词|降级答复|联想直接输出' \
    || { echo "FAIL 交互冒烟"; fail=1; }

echo
[ "$fail" -eq 0 ] && echo "全部回归通过" || { echo "有回归失败"; exit 1; }
