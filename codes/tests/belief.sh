#!/bin/sh
# tests/belief.sh — 「归属」不是标注：每个「他」有一份**自己的信念表**
#
# 原来「他 以为 X」只给假设层贴个标签，轮末就没了；问「他 觉得 X 吗」其实
# 是拿**我的知识**答的 —— 那不叫视角推理。现在：
#   · 「他 以为 X」= 他告诉我的看法 → 存在**他名下**（持久，模型 v8 存盘）；
#   · 「如果 他 以为 X」= 反事实假设 → 进假设层（临时，不存盘）；两者绝不混淆；
#   · 他的信念**不进我的图**：他说猫会飞，我的知识不因此改变；
#   · 问他的想法只查他的表：他没说过的，就答「我没听他说过这个」，
#     不许拿我的知识冒充他的想法；
#   · 我不知道、但他提过 → **引用**他的话，并说明我不知道实情；
#   · 他的看法和我的知识相反时，答复里如实点出来。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$ROOT/build_aip}
MODEL=${MODEL:-$ROOT/model.bin}
cd "$ROOT"
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }

TMP=./_t/belief
rm -rf "$TMP"; mkdir -p "$TMP"

run() {  # run <输入行...>：全部输出
    { printf '::load %s\n::notepad off\n' "$MODEL"; for q in "$@"; do printf '%s\n' "$q"; done
      printf '::quit\n'; } | "$BIN" 2>&1
}
reps() { sed -n 's/.*AI > //p'; }

echo "① 「他 以为 X」记到他名下，并点出与我的知识相反"
out=$(run "他 以为 猫 会 飞" "::others")
r=$(printf '%s\n' "$out" | reps | head -1)
case "$r" in *"记下"*"他 以为 猫 会 飞"*) chk "记下了他的看法（$r）" 1 ;;
                                  *)     chk "记下了他的看法（实得「$r」）" 0 ;;
esac
case "$r" in *"我 知道 实际"*) chk "指出这和他的说法与我的知识相反" 1 ;;
                          *)     chk "指出与我的知识相反（实得「$r」）" 0 ;;
esac
printf '%s\n' "$out" | grep -aq '他者 0\] 他' \
    && chk "::others 里挂在他名下" 1 || chk "::others 里挂在他名下" 0
printf '%s\n' "$out" | grep -aq '猫  会 飞' \
    && chk "他的信念表里确实有「猫 会 飞」" 1 || chk "他的信念表里有「猫 会 飞」" 0

echo "② 他的信念**不进我的知识**"
out=$(run "他 以为 猫 会 飞" "猫 会 飞 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"不 对"*) chk "我的视角答「不对」（$r）" 1 ;;
                 *)     chk "我的视角答「不对」（实得「$r」）" 0 ;;
esac

echo "③ 问他的想法：按他的信念答，并标明是他的看法"
out=$(run "他 以为 猫 会 飞" "他 觉得 猫 会 飞 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"在 他 看来"*"对"*) chk "在他看来说「对」（$r）" 1 ;;
                             *)    chk "在他看来说「对」（实得「$r」）" 0 ;;
esac
case "$r" in *"他 的 看法"*) chk "标明这是他的看法、不是我确认真实" 1 ;;
                       *)    chk "标明这是他的看法（实得「$r」）" 0 ;;
esac

echo "④ 他没说过的：不许拿我的知识冒充他的想法"
out=$(run "他 以为 猫 会 飞" "他 觉得 猫 属于 鸟 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"没 听 他 说 过"*) chk "说清「我没听他说过这个」（$r）" 1 ;;
                             *)   chk "说清「我没听他说过这个」（实得「$r」）" 0 ;;
esac
case "$r" in *"不 对"*) chk "没有拿我的知识（「不对」）冒充他" 0 ;;
                 *)     chk "没有拿我的知识（「不对」）冒充他" 1 ;;
esac

echo "⑤ 我不知道但他提过 → 引用他的话，不当成事实"
# 这里必须挑一件**我真的不知道**的事：`猫 会 唱歌` 现在靠「缺前提」能推出来
# （见 tests/prerequisite.sh），我既然知道就不用引他的话了。用「跳舞」。
out=$(run "他 以为 猫 会 跳舞" "猫 会 跳舞 吗")
r=$(printf '%s\n' "$out" | reps | tail -1)
case "$r" in *"说 过"*"猫 会 跳舞"*"不 知道 实情"*) chk "引用了他的话并说明我不知道（$r）" 1 ;;
                                               *)    chk "引用了他的话并说明我不知道（实得「$r」）" 0 ;;
esac

echo "⑥ 他的信念是持久状态：存盘 → 重载 → 还在"
{ printf '::load %s\n::notepad off\n他 以为 猫 会 飞\n::save %s/m.bin\n::load %s/m.bin\n他 觉得 猫 会 飞 吗\n::others\n::quit\n' \
      "$MODEL" "$TMP" "$TMP"; } | "$BIN" 2>&1 > "$TMP/out.txt" || true
r=$(reps < "$TMP/out.txt" | tail -1)
case "$r" in *"在 他 看来"*"对"*) chk "重载后他仍然相信（$r）" 1 ;;
                             *)    chk "重载后他仍然相信（实得「$r」）" 0 ;;
esac
grep -aq '猫  会 飞' "$TMP/out.txt" && chk "重载后 ::others 里还在" 1 || chk "重载后 ::others 里还在" 0

echo "⑦ 反事实的「如果 他 以为」**不**进他的信念表（临时 ≠ 他的看法）"
out=$(run "如果 他 以为 猫 会 飞" "::others")
printf '%s\n' "$out" | grep -aq '还没有「别人」的模型' \
    && chk "反事实没有写进他者模型" 1 || chk "反事实没有写进他者模型" 0

echo "⑧ 来源监控 ::src：这条知识是哪来的（基准/观测/推论/他说的/假设）"
out=$(run "::src 猫 属于 动物")
printf '%s
' "$out" | grep -aq '来源=基准' && chk "我的图那条是**基准**（relations.txt 教过的）" 1 \
                                        || chk "基准档（实得：$(printf '%s\n' "$out" | grep '我的图'))" 0
printf '%s
' "$out" | grep -aq '推得出来吗：成立' && chk "求解器也推得出来（链 + 验证器通过）" 1 \
                                                 || chk "求解器" 0
printf '%s
' "$out" | grep -aq '他说的：没听谁说过这条' && chk "他没说过这条 —— 不拿他的名义说话" 1 \
                                                      || chk "他没说过" 0
out=$(run "他 说 猫 会 飞" "::src 猫 会 飞")
printf '%s
' "$out" | grep -aq '和我的图相反' && chk "他说 猫 会 飞 → 来源账点明**与我的图相反**" 1 \
                                              || chk "相反要写明（实得：$(printf '%s\n' "$out" | grep '他说的'))" 0
printf '%s
' "$out" | grep -aq '可 我 知道 实际 上 不 是 这样' && chk "当场说清：那是他说的，我知道实际不是" 1 \
                                                             || chk "当场说清" 0
out=$(run "他 说 我 拥有 猫" "::src 我 有 猫")
printf '%s\n' "$out" | grep -aq '他 说' && chk "他说「拥有 猫」，我查「有 猫」——**同义改写对得上**（谓词别名归一）" 1 \
                                          || chk "同义改写（实得：$(printf '%s\n' "$out" | grep '他说的' | head -1)）" 0
out=$(run "::src 猫 不存在的关系 鱼")
printf '%s
' "$out" | grep -aq '不认识的谓语' && chk "没登记过的谓语：说清楚，不瞎编" 1 || chk "未知谓语" 0
out=$(run "::edges 猫")
printf '%s
' "$out" | grep -aq '来源=' && chk "::edges 每条边带来源档" 1 || chk "::edges 来源" 0

echo "⑨ 「学出来的可信度」的证据要从**图里重新数**（计数器不进存档）"
# 现场学两条关系 → 存档 → 重载：证据（现场学到 N 条）与后验都要还在
run "甲 属于 家具" "乙 属于 家具" "::save $TMP/ev.bin" > "$TMP/ev1.txt" 2>&1 || true
out=$( { printf '::load %s/ev.bin\n::stats\n::quit\n' "$TMP"; } | "$BIN" 2>&1 )
printf '%s\n' "$out" | grep -aq '现场学到 [2-9]' \
    && chk "重载后「现场学到 N 条」还在（证据是从图里数出来的）" 1 \
    || chk "重载后证据还在（实得：$(printf '%s\n' "$out" | grep '现场学到' | head -1)）" 0
printf '%s\n' "$out" | grep -aqE '观测 0\.[5-9]' \
    && chk "观测档后验 > 0.5（学到了东西，不是先验 0.5）" 1 \
    || chk "观测档后验（实得：$(printf '%s\n' "$out" | grep '来源档可信度' | head -1)）" 0

echo
[ "$fail" -eq 0 ] && echo "PASS belief（归属 + 来源监控 + 学出来的可信度跨重启）" \
                  || { echo "FAIL belief"; exit 1; }
