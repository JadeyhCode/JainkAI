#!/bin/sh
# tests/consistency.sh — 「教进去的，问得出来；存盘再开，还在」（§6.213）
#
# 为什么单有一份：前面的套件都是**按功能**切的（关系层/追问/造词/存盘各测一段）。
# 这条盯的是**端到端自洽**：一次教 14 条事实（含否定、含新词），然后逐条问回去，
# 答错一条就是"知识进得去、出不来"；再 `::save` → 新进程 `::load` 问 6 条，
# 答错就是"存盘丢了/装错了"。
#
# 判据不看整句文本，只看**判定词 + 极性**（`对 ， S -[R]-> B` / `不 对 ， S -[不 R]-> B`），
# 这样换措辞、加尾巴都不会误报。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

BIN=${BIN:-./build_aip}
MODEL=${MODEL:-model.bin}
[ -x "$BIN" ] || { echo "FAIL: 缺可执行文件 $BIN"; exit 1; }
[ -f "$MODEL" ] || { echo "FAIL: 缺模型 $MODEL"; exit 1; }

TMP=./_consistency_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
fail=0
chk() { if [ "$2" -eq 1 ]; then echo "  OK   $1"; else echo "  FAIL $1${3:+（实得「$3」）}"; fail=1; fi }

# 教 14 条（`::natcoin on` 让新词当场造），问 14 条
{
    printf '::load %s\n::natcoin on\n' "$MODEL"
    for f in "考拉 属于 动物" "考拉 会 爬树" "考拉 不 会 飞" \
             "企鹅 会 游泳" "企鹅 不 会 飞" "鲨鱼 属于 鱼" "鲨鱼 会 游泳" \
             "鲨鱼 不 会 爬树" "骆驼 属于 动物" "骆驼 会 走路" "骆驼 不 会 游泳" \
             "猫头鹰 会 飞" "猫头鹰 不 会 游泳" "熊猫 属于 动物"; do
        printf '%s\n' "$f"
    done
    for q in "考拉 属于 动物 吗" "考拉 会 爬树 吗" "考拉 会 飞 吗" \
             "企鹅 会 游泳 吗" "企鹅 会 飞 吗" "鲨鱼 属于 鱼 吗" "鲨鱼 会 游泳 吗" \
             "鲨鱼 会 爬树 吗" "骆驼 属于 动物 吗" "骆驼 会 走路 吗" "骆驼 会 游泳 吗" \
             "猫头鹰 会 飞 吗" "猫头鹰 会 游泳 吗" "熊猫 属于 动物 吗"; do
        printf '%s\n' "$q"
    done
    printf '::quit\n'
} > "$TMP/in.txt"
timeout 300 "$BIN" < "$TMP/in.txt" > "$TMP/out.txt" 2>&1 || true

# 回答行里的判定词 + 极性：`对 ， S -[R]-> B` / `不 对 ， S -[不 R]-> B`
ans() { grep -a 'AI > ' "$TMP/out.txt" | sed -n "$1p" | sed 's/.*AI > //'; }

echo "① 教进去的 14 条，问回去必须全对（对的答「对」、否定的答「不 对」）"
i=15; n_ok=0; n_bad=0
for spec in "考拉 属于 动物:对" "考拉 会 爬树:对" "考拉 会 飞:不对" \
            "企鹅 会 游泳:对" "企鹅 会 飞:不对" "鲨鱼 属于 鱼:对" "鲨鱼 会 游泳:对" \
            "鲨鱼 会 爬树:不对" "骆驼 属于 动物:对" "骆驼 会 走路:对" "骆驼 会 游泳:不对" \
            "猫头鹰 会 飞:对" "猫头鹰 会 游泳:不对" "熊猫 属于 动物:对"; do
    fact=${spec%:*}; want=${spec##*:}
    line=$(ans "$i"); i=$((i + 1))
    if [ "$want" = "对" ]; then
        printf '%s\n' "$line" | grep -q '^ *对 ， ' && n_ok=$((n_ok + 1)) || { n_bad=$((n_bad + 1)); echo "     ↳ 应为「对」：$fact ⇒ $line"; }
    else
        printf '%s\n' "$line" | grep -q '不 对 ， ' && n_ok=$((n_ok + 1)) || { n_bad=$((n_bad + 1)); echo "     ↳ 应为「不 对」：$fact ⇒ $line"; }
    fi
done
[ "$n_bad" -eq 0 ] && chk "14 条全部答对（$n_ok/14）" 1 || chk "14 条全部答对（$n_ok/14）" 0

echo "② 存盘 → 新进程载入 → 再问 6 条，还得对（存盘不许丢/不许装错）"
{ printf '::load %s\n::natcoin on\n' "$MODEL"
  for f in "考拉 属于 动物" "考拉 会 爬树" "考拉 不 会 飞" "企鹅 会 游泳" "鲨鱼 会 游泳" "骆驼 会 走路"; do
      printf '%s\n' "$f"
  done
  printf '::save %s/round.bin\n::quit\n' "$TMP"
} > "$TMP/save.txt"
timeout 300 "$BIN" < "$TMP/save.txt" > "$TMP/save.out" 2>&1 || true
grep -aq '已保存' "$TMP/save.out" && chk "存盘成功" 1 || chk "存盘成功" 0 "$(tail -1 "$TMP/save.out")"

{ printf '::load %s/round.bin\n' "$TMP"
  for q in "考拉 属于 动物 吗" "考拉 会 爬树 吗" "考拉 会 飞 吗" "企鹅 会 游泳 吗" "鲨鱼 会 游泳 吗" "骆驼 会 走路 吗"; do
      printf '%s\n' "$q"
  done
  printf '::quit\n'
} > "$TMP/reload.txt"
timeout 300 "$BIN" < "$TMP/reload.txt" > "$TMP/reload.out" 2>&1 || true
i=1; n_ok=0; n_bad=0
for spec in "考拉 属于 动物:对" "考拉 会 爬树:对" "考拉 会 飞:不对" "企鹅 会 游泳:对" \
            "鲨鱼 会 游泳:对" "骆驼 会 走路:对"; do
    line=$(grep -a 'AI > ' "$TMP/reload.out" | sed -n "${i}p" | sed 's/.*AI > //'); i=$((i + 1))
    want=${spec##*:}
    if [ "$want" = "对" ]; then
        printf '%s\n' "$line" | grep -q '^ *对 ， ' && n_ok=$((n_ok + 1)) || n_bad=$((n_bad + 1))
    else
        printf '%s\n' "$line" | grep -q '不 对 ， ' && n_ok=$((n_ok + 1)) || n_bad=$((n_bad + 1))
    fi
done
[ "$n_bad" -eq 0 ] && chk "重载后 6 条全对（$n_ok/6）" 1 || chk "重载后 6 条全对（$n_ok/6）" 0

echo "③ 跨事实继承：类别上学的结论要能推到成员上（肯定 + 否定）"
{
    printf '::load %s\n::natcoin on\n' "$MODEL"
    printf '动物 会 呼吸\n动物 不 会 唱歌\n考拉 属于 动物\n'
    printf '考拉 会 呼吸 吗\n考拉 不 会 唱歌 吗\n考拉 会 唱歌 吗\n'
    printf '::quit\n'
} > "$TMP/inh.txt"
timeout 300 "$BIN" < "$TMP/inh.txt" > "$TMP/inh.out" 2>&1 || true
i=4; n_ok=0; n_bad=0
for spec in "考拉 会 呼吸:对" "考拉 不 会 唱歌:对" "考拉 会 唱歌:不对"; do
    line=$(grep -a 'AI > ' "$TMP/inh.out" | sed -n "${i}p" | sed 's/.*AI > //'); i=$((i + 1))
    want=${spec##*:}
    if [ "$want" = "对" ]; then
        printf '%s\n' "$line" | grep -q '^ *对 ， ' && n_ok=$((n_ok + 1)) || { n_bad=$((n_bad + 1)); echo "     ↳ 应为「对」：${spec%:*} ⇒ $line"; }
    else
        printf '%s\n' "$line" | grep -q '不 对 ， ' && n_ok=$((n_ok + 1)) || { n_bad=$((n_bad + 1)); echo "     ↳ 应为「不 对」：${spec%:*} ⇒ $line"; }
    fi
done
[ "$n_bad" -eq 0 ] && chk "继承 3/3（肯定·否定·否定问法）" 1 || chk "继承 3/3" 0
grep -a '属于·继承' "$TMP/inh.out" > /dev/null \
    && chk "答案里写明走的是「属于·继承」这一跳" 1 \
    || chk "答案里写明走的是「属于·继承」" 0 "$(grep -a 'AI > ' "$TMP/inh.out" | tail -2 | head -1)"

echo "④ §6.221 类比兜底的答复要点到被问的那个词（不能只提"最像的那个词"）"
{
    printf '::load %s\n::natcoin on\n' "$MODEL"
    printf '动物 吃 草\n考拉 属于 动物\n考拉 吃 草 吗\n'
    printf '::quit\n'
} > "$TMP/ana.txt"
timeout 300 "$BIN" < "$TMP/ana.txt" > "$TMP/ana.out" 2>&1 || true
line=$(grep -a 'AI > ' "$TMP/ana.out" | sed -n 3p | sed 's/.*AI > //')
printf '%s\n' "$line" | grep -q "^（问的是 考拉）" \
    && chk "类比答复以「（问的是 考拉）」开头" 1 \
    || chk "类比答复以「（问的是 考拉）」开头" 0 "$line"

echo "⑤ §6.229b 虚词守卫矩阵：五个学习入口 × 三个位置，垃圾说法一律不学"
{
    printf '::load %s\n::natcoin on\n' "$MODEL"
    # 主语位 / 谓语位 / 宾语位 各一条（挑最容易被学进去的形状）
    printf '很 叫 小明\n'          # 关系·主语位
    printf '猫 了 狗\n'            # 关系·谓语位
    printf '猫 属于 很\n'          # 关系·宾语位（核心关系）
    printf '了 在 家里\n'          # 地点·主语位
    printf '我 在 很\n'            # 地点·宾语位
    printf '他 以为 猫 属于 很\n'  # 看法·宾语位
    printf '如果 猫 属于 很\n'     # 假设·宾语位
    printf '::quit\n'
} > "$TMP/mx.txt"
timeout 300 "$BIN" < "$TMP/mx.txt" > "$TMP/mx.out" 2>&1 || true
n_learn=$(grep -ac "记下 了 ：" "$TMP/mx.out" || true)
[ "$n_learn" -eq 0 ] && chk "七条垃圾说法一条都没学（记下 了 出现 $n_learn 次）" 1 \
    || chk "七条垃圾说法一条都没学" 0 "$(grep -a "记下 了 ：" "$TMP/mx.out" | head -1)"
grep -aq "就 当 猫 属于 很" "$TMP/mx.out" \
    && chk "垃圾假设也没进假设层" 0 || chk "垃圾假设也没进假设层" 1
# 正例：正常地点照旧学（防止守卫把正常路径也堵了）
{
    printf '::load %s\n猫 在 这里\n' "$MODEL"
    printf '::quit\n'
} > "$TMP/mx2.txt"
timeout 300 "$BIN" < "$TMP/mx2.txt" > "$TMP/mx2.out" 2>&1 || true
grep -aq "记下 了 ： 猫 在 这里" "$TMP/mx2.out" \
    && chk "正例照旧（猫 在 这里 学得进）" 1 \
    || chk "正例照旧（猫 在 这里）" 0 "$(grep -a '记下 了' "$TMP/mx2.out" | head -1)"

rm -rf "$TMP"
if [ "$fail" -ne 0 ]; then
    echo "FAIL consistency（教→问→存盘→重载 的端到端自洽）"
    exit 1
fi
echo "PASS consistency（教→问 14/14；存盘重载 6/6；继承 3/3；类比点到被问的词；守卫矩阵全拒）"
