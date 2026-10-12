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
    { printf '::load %s\n::notepad off\n::rel analogy off\n::rel analogy solver off\n' "$MODEL"; for q in "$@"; do printf '%s\n' "$q"; done
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
{ printf '::load %s\n::notepad off\n::rel analogy off\n::rel analogy solver off\n他 以为 猫 会 飞\n::save %s/m.bin\n::load %s/m.bin\n他 觉得 猫 会 飞 吗\n::others\n::quit\n' \
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

echo "⑩ 他者视角**多层合并 + 四态判定**（同类别并进滑动窗；冲突交四态，待验≠否定）"
# 两个人说法相反 → 有分歧（**待验**），不是否定；两条都留着
out=$(run "甲 以为 猫 属于 动物" "乙 说 猫 不属于 动物" "::others 猫 属于 动物")
printf '%s\n' "$out" | grep -aq '有分歧（待验，不是否定）' \
    && chk "两人相反 → 有分歧（待验，不是否定）" 1 \
    || chk "有分歧（实得：$(printf '%s\n' "$out" | grep '合并判定' | head -1)）" 0
printf '%s\n' "$out" | grep -aq '1 认 / 1 不认' \
    && chk "两条都留着，谁也不盖谁（1 认 / 1 不认）" 1 || chk "认/不认计数" 0
# **铁律**：他的信念不进我的图 —— 乙 说不属于，我的知识照旧
printf '%s\n' "$out" | grep -aq '我的知识没被改' \
    && chk "判定之后我的知识没被改（他的信念只在他名下）" 1 || chk "不该改我的知识" 0
out=$(run "甲 以为 猫 属于 动物" "乙 说 猫 不属于 动物" "猫 属于 动物 吗")
printf '%s\n' "$out" | reps | grep -aq '对' \
    && chk "我照旧知道「猫 属于 动物」（没被乙的说法改掉）" 1 \
    || chk "我的知识被别人的说法改了（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
# 两个人都认 → 都说认
out=$(run "甲 以为 猫 会 飞" "乙 觉得 猫 会 飞" "::others 猫 会 飞")
printf '%s\n' "$out" | grep -aq '都说认' && chk "两人都认 → 都说认" 1 \
    || chk "都说认（实得：$(printf '%s\n' "$out" | grep '合并判定' | head -1)）" 0
# 同类别：他说「学生 属于 人」，问「学生 属于 动物」→ 人 ⊂ 动物 **蕴含**所问，算依据
out=$(run "甲 以为 学生 属于 人" "::others 学生 属于 动物")
printf '%s\n' "$out" | grep -aq '同类' && chk "上位链蕴含所问 → 标成「同类」，算依据" 1 \
    || chk "同类蕴含（实得：$(printf '%s\n' "$out" | grep '甲：' | head -1)）" 0
printf '%s\n' "$out" | grep -aq '都说认' && chk "并进滑动窗之后判成「都说认」" 1 || chk "同类后的判定" 0
# 反过来（他说的更泛）不能当依据：他说「猫 属于 生物」，问「猫 属于 动物」
# `::others refine off`：这两条钉的是**默认语义**（上位只当相关）—— 反向测量下也要关
out=$(run "::others refine off" "甲 以为 猫 属于 生物" "::others 猫 属于 动物")
printf '%s\n' "$out" | grep -aq '不蕴含' \
    && chk "上位说法标成「相关但不蕴含」，**不算依据**" 1 \
    || chk "上位说法（实得：$(printf '%s\n' "$out" | grep '甲：' | head -1)）" 0
printf '%s\n' "$out" | grep -aq '没人说过' \
    && chk "只剩相关说法时判成「没人说过」（不拿它冒充答案）" 1 || chk "不蕴含时的判定" 0
# 没人提过 → 没人说过
out=$(run "甲 以为 猫 会 飞" "::others 猫 有 毛")
printf '%s\n' "$out" | grep -aq '没人说过' && chk "没人提过 → 没人说过" 1 || chk "没人说过" 0
# 判定是**现算**的：存盘重载后同一个问题给同一个答案（信念表是持久的）
run "甲 以为 猫 属于 动物" "乙 说 猫 不属于 动物" "::save $TMP/ov.bin" > "$TMP/ov1.txt" 2>&1 || true
out=$( { printf '::load %s/ov.bin\n::notepad off\n::rel analogy off\n::rel analogy solver off\n::others 猫 属于 动物\n::quit\n' "$TMP"; } | "$BIN" 2>&1 )
printf '%s\n' "$out" | grep -aq '有分歧（待验，不是否定）' \
    && chk "重载后同一个问题给同一个判定（现算，不落库也不丢）" 1 \
    || chk "重载后的判定（实得：$(printf '%s\n' "$out" | grep '合并判定' | head -1)）" 0

echo "⑨ ③ 他者视角**进回答路径**（复数主语，默认关）"
# 默认关那一侧：复数主语照走流水线（今天那条"在 大家 看来"的路），没有四态答复。
# ⚠ 开关反向测量（AIP_OTHERS=1，见 tests/switches_ab.sh）时这几条**自动跳过**。
if [ -z "${AIP_OTHERS:-}" ]; then
    out=$(run "甲 以为 猫 会 飞" "大家 觉得 猫 会 飞 吗")
    if printf '%s\n' "$out" | grep -aq '大家 都 说'; then
        chk "默认关不该有四态答复" 0
    else
        chk "默认关：复数主语照走流水线（与今天逐字节相同）" 1
    fi
    out=$(run "::others ask")
    printf '%s\n' "$out" | grep -aq '他者视角进回答路径 = 关（默认）' \
        && chk "::others ask 自报默认关" 1 || chk "ask 默认" 0
else
    chk "AIP_OTHERS=1（反向测量）：默认值断言按规矩跳过" 1
fi
# 开着那一侧：**都说认** / **都说不认** / **有分歧（待验）** / **没人说过** 四种都要有入口
out=$(run "::others ask on" "甲 以为 猫 会 飞" "乙 觉得 猫 会 飞" "大家 觉得 猫 会 飞 吗")
printf '%s\n' "$out" | grep -aq '大家 都 说 ： 猫 会 飞' \
    && chk "都说认：两个人说会飞 → 「大家都说：猫 会 飞」" 1 \
    || chk "都说认（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
printf '%s\n' "$out" | grep -aq '2 个人 说 过' && chk "说清是几个人说的" 1 || chk "人数" 0
printf '%s\n' "$out" | grep -aq '我 不 这么 看' \
    && chk "**分开说**：他们的看法与「我自己的知识」分开（猫不会飞）" 1 || chk "分开说" 0
# 注意用**声明里的否定形式**（关系表把「会」的否定声明成两词的 `不 会`；
# 合写的「不会」今天不被关系/信念解析认出来 —— 那是另一个真缺口，已登记，不在这一节夹带）
out=$(run "::others ask on" "甲 认为 猫 不 会 游泳" "乙 觉得 猫 不 会 游泳" "大家 觉得 猫 会 游泳 吗")
printf '%s\n' "$out" | grep -aq '大家 都 说 ： 猫 不 会 游泳' \
    && chk "都说不认：都说不 会 → 「大家都说：猫 不 会 游泳」" 1 \
    || chk "都说不认（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
out=$(run "::others ask on" "甲 以为 猫 属于 动物" "乙 说 猫 不属于 动物" "大家 怎么 看 猫 属于 动物 吗")
printf '%s\n' "$out" | grep -aq '大家 说 法 不 一致' \
    && chk "有分歧：两边都留着 → 「大家说法不一致」" 1 \
    || chk "有分歧（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
printf '%s\n' "$out" | grep -aq '待验 ， 不是 否定' \
    && chk "分歧照旧标「待验，不是否定」（不是把一边判死）" 1 || chk "待验" 0
out=$(run "::others ask on" "甲 以为 猫 会 飞" "别人 说 猫 属于 动物 吗")
printf '%s\n' "$out" | grep -aq '他们 没 说 过' \
    && chk "没人说过：如实说没人说过（不拿我的知识冒充）" 1 || chk "没人说过" 0
# 只读：他者信念只在**他的**名下 —— 问了也不会把我的知识改掉
out=$(run "::others ask on" "甲 以为 猫 会 飞" "大家 觉得 猫 会 飞 吗" "猫 会 飞 吗")
printf '%s\n' "$out" | reps | tail -1 | grep -aq '不 对' \
    && chk "只读：他说的不影响我的知识（我还是答「猫不会飞」）" 1 \
    || chk "只读（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0


echo
echo "⑩ ② 他者上位降权（默认关）：更宽的说法算半票参考"
# 「他说的比你问的更宽」（他说 喜欢 动物、你问 喜欢 猫）在默认档只算"相关"；打开后进弱票。
# ⚠ 开关反向测量（AIP_OTHERWEAK=1）时，"默认关"那一侧按规矩跳过
if [ -z "${AIP_OTHERWEAK:-}" ]; then
    out=$(run "甲 以为 狗 喜欢 动物" "::others 狗 喜欢 猫")
    printf '%s\n' "$out" | grep -aq '上位' && printf '%s\n' "$out" | grep -aq '不算依据（要纳入就 ::others refine on）' \
        && chk "默认关：上位说法**只当相关**（并告诉你开关在哪）" 1 \
        || chk "默认关（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
else
    chk "AIP_OTHERWEAK=1（反向测量）：默认值断言按规矩跳过" 1
fi
out=$(run "::others refine on" "甲 以为 狗 喜欢 动物" "::others 狗 喜欢 猫")
printf '%s\n' "$out" | grep -aq '半票参考' && printf '%s\n' "$out" | grep -aq '合并判定：\*\*都说认\*\*' \
    && chk "开着：上位说法进**弱票** ⇒ 只有弱票时给倾向（都说认）" 1 \
    || chk "开着（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0

echo "⑪ ② 指代消解最小版（默认关）：他/她/它 指回最近说话的人"
if [ -z "${AIP_PRONLAST:-}" ]; then
    out=$(run "::others pron off" "甲 以为 猫 会 飞" "他 觉得 猫 会 飞 吗")
    printf '%s\n' "$out" | grep -aq '在 他 看来' \
        && chk "默认关：把「他」当人名（今天的行为，如实记着）" 1 || chk "默认关（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
fi
out=$(run "::others pron on" "甲 以为 猫 会 飞" "他 觉得 猫 会 飞 吗")
printf '%s\n' "$out" | grep -aq '在 甲 看来' \
    && chk "开着：代词**指回甲**（不再是他那个人名）" 1 \
    || chk "指代（实得：$(printf '%s\n' "$out" | reps | tail -1)）" 0
printf '%s\n' "$out" | grep -aq '假设' && printf '%s\n' "$out" | grep -aq '这 是 他 的 看法' \
    && chk "答的是**他的看法**（假设下 + 明说不是我的知识）" 1 || chk "归属清楚" 0

[ "$fail" -eq 0 ] && echo "PASS belief（归属 + 来源监控 + 可信度跨重启 + 他者视角多层合并/四态 + 复数主语进回答路径）" \
                  || { echo "FAIL belief"; exit 1; }
