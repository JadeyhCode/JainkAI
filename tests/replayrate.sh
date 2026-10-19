#!/bin/sh
# tests/replayrate.sh — **语料复读率**（§6.364）
#
# "不是兜底"不等于"答得好"：很多答复是**语料原文**被当成答案复读。
# 判据：把答复去空格，若它是 `_corpus.txt` 里某句的**逐字子串**（≥6 字）⇒ 记为复读。
# 复读再分两类（§6.364）：
#   对题复读：答复里出现了输入中的**内容字**（去掉 你我他她它们的地得了吗呢吧啊是不有和就都很也太什么怎么）——
#             说明"答的是这个话题，但用的是原文"（自我描述类问题常见，属记忆不是错答）；
#   跑题复读：输入的内容字一个都没出现在答复里 —— 这是真错答（`狗 是谁 => 我 觉得 它 们 比 人 单 纯`）。
# 用法：sh tests/replayrate.sh [模型] [样本文件]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MODEL=${1:-session.bin}
SAMPLE=${2:-tests/corpus_q100d.txt}
exec python3 - "$MODEL" "$SAMPLE" <<'PY'
import subprocess, re, sys
model, sample = sys.argv[1], sys.argv[2]
qs=[l.strip() for l in open(sample, encoding="utf-8") if l.strip() and not l.startswith("#")]
corpus=re.sub(r"\s+", "", open("_corpus.txt", encoding="utf-8", errors="ignore").read())
STOP=set("你我他她它们的地得了吗呢吧啊是不有和就都很也太些这那个一么怎什哪")
def content(s):
    return {c for c in re.sub(r"\s+","",s) if c not in STOP}
total=replay=on_topic=off_topic=0
ex_on=[]; ex_off=[]
for q in qs:
    p=subprocess.run(["./build_aip","--model",model], input=q+"\n::quit\n",
                     capture_output=True, text=True, timeout=60)
    lines=[l for l in p.stdout.splitlines() if "AI > " in l]
    if not lines: continue
    r=lines[-1].split("AI > ",1)[1].strip()
    if not r: continue
    total+=1
    ns=re.sub(r"\s+","",r)
    if len(ns)>=6 and ns in corpus:
        replay+=1
        # 对题判定：输入内容字里**至少一个**出现在答复里
        if content(q) & set(ns):
            on_topic+=1
            if len(ex_on)<4: ex_on.append((q, r[:30]))
        else:
            off_topic+=1
            if len(ex_off)<4: ex_off.append((q, r[:30]))
tot=max(total,1)
print(f"复读率：{total} 条答复里 {replay} 条是语料原文（{100*replay/tot:.0f}%）"
      f" —— 对题 {on_topic}（{100*on_topic/tot:.0f}%）、跑题 {off_topic}（{100*off_topic/tot:.0f}%）")
for q,r in ex_on: print(f"  对题复读: {q} => {r}")
for q,r in ex_off: print(f"  跑题复读: {q} => {r}")
PY
