#!/bin/sh
# tools/induce_paraphrase.sh — **从例句归纳说法族**（§6.760 顺序说法 + §6.762 框式说法）
#
# 输入：每行 `原句<TAB>规范事实`（规范事实 = `主体 关系 客体`，空格分隔）
#   企鹅擅长游泳	企鹅 会 游泳          ⇒ 顺序说法：擅长 → 会
#   蜜蜂以花蜜为生	蜜蜂 吃 花蜜          ⇒ 框式说法：以…为生 → 吃
# 归纳规则（只看形状，不看语义）：
#   · 原句 = 主语 + MID + 宾语  ⇒ MID 是"顺序说法" ⇒ paraphrase.map:  MID<TAB>关系
#   · 原句 = 主语 + 前缀 + 宾语 + 后缀 且宾语夹在中间 ⇒ 框式 ⇒ circumfix.map: 前缀<TAB>后缀<TAB>关系
#
# 用法：sh tools/induce_paraphrase.sh <例句文件> [paraphrase.map] [circumfix.map]
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EX=${1:?用法: sh tools/induce_paraphrase.sh <例句文件> [paraphrase.map] [circumfix.map]}
MAP=${2:-$ROOT/paraphrase.map}
CF=${3:-$ROOT/circumfix.map}
: >> "$MAP"; : >> "$CF"
python3 - "$EX" "$MAP" "$CF" <<'PYEOF'
import sys
ex, mapf, cff = sys.argv[1], sys.argv[2], sys.argv[3]
seq = set()
cfg = set()
for line in open(mapf, encoding='utf-8', errors='ignore'):
    if '\t' in line: seq.add(tuple(line.rstrip('\n').split('\t', 1)))
for line in open(cff, encoding='utf-8', errors='ignore'):
    if line.count('\t') == 2: cfg.add(tuple(line.rstrip('\n').split('\t')))
n_seq = n_cf = tried = 0
for raw in open(ex, encoding='utf-8', errors='ignore'):
    if '\t' not in raw: continue
    s_raw, s_can = raw.rstrip('\n').split('\t', 1)
    s_raw = s_raw.strip().rstrip('。！？')
    parts = s_can.split()
    if len(parts) < 3: continue
    sub, rel, obj = parts[0], parts[1], ' '.join(parts[2:])
    tried += 1
    if not s_raw.startswith(sub): continue
    rest = s_raw[len(sub):]
    if not rest: continue
    pos = rest.find(obj)
    if pos < 0: continue
    pre, suf = rest[:pos], rest[pos+len(obj):]
    if not pre and not suf:
        continue                      # 原句用的就是规范关系词本身，不用归纳
    if not suf:                       # 顺序说法：主语 + 说法 + 宾语
        mid = pre.strip()
        if not mid or mid == rel: continue
        if (mid, rel) in seq: continue
        seq.add((mid, rel)); n_seq += 1
        print(f'  归纳（顺序）：{mid} → {rel}（来自「{s_raw}」）')
    elif not pre:                     # 只有后缀（没有前缀）⇒ 不是框式，跳过
        continue
    else:                             # 框式：主语 + 前缀 + 宾语 + 后缀
        pre, suf = pre.strip(), suf.strip()
        if not pre or not suf: continue
        if (pre, suf, rel) in cfg: continue
        cfg.add((pre, suf, rel)); n_cf += 1
        print(f'  归纳（框式）：{pre}…{suf} → {rel}（来自「{s_raw}」）')

with open(mapf, 'w', encoding='utf-8') as f:
    for a, b in sorted(seq): f.write(f'{a}\t{b}\n')
with open(cff, 'w', encoding='utf-8') as f:
    for a, b, c in sorted(cfg): f.write(f'{a}\t{b}\t{c}\n')
print(f'== 读完 {tried} 条例句：顺序 +{n_seq} 条 ⇒ {mapf}（{len(seq)} 行）；框式 +{n_cf} 条 ⇒ {cff}（{len(cfg)} 行）==')
PYEOF
