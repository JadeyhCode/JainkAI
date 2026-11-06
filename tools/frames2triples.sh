#!/bin/sh
# tools/frames2triples.sh — 把 `frames.txt` 生成**完整**图谱三元组（§6.768）
#
# 一个帧在文件里长这样：
#   [帧:害怕X吗]
#   [问] 你 害怕 <X> 吗
#   [答] 我 不 怕 <X>
#   [填:X=<hex>,<hex>]        ← 槽位填充词（hex 编码的 UTF-8）
#   ---
#
# 生成（stdout，`::ingest` 能吃）：
#   帧库\t帧\t帧:<名字>
#   帧:<名字>\t问法\t<模板，空格→下划线，<X>→X>
#   帧:<名字>\t答法\t<同上>
#   帧:<名字>\t填充\t<解码后的词，逗号分隔>
#
# 用法：sh tools/frames2triples.sh [frames.txt] > /tmp/frames.triples
set -u
F=${1:-frames.txt}
[ -s "$F" ] || { echo "FAIL: 没有 $F" >&2; exit 1; }
python3 - "$F" <<'PYEOF'
import sys, re
path = sys.argv[1]
cur = None
out = []
def enc(s):
    s = s.replace('<X>', 'X').replace('<', '').replace('>', '')
    return re.sub(r'\s+', '_', s.strip())
for raw in open(path, encoding='utf-8', errors='ignore'):
    line = raw.rstrip('\n')
    m = re.match(r'^\[帧:(.+)\]$', line)
    if m:
        cur = m.group(1)
        out.append(('帧库', '帧', '帧:' + cur))
        continue
    if line.startswith('---') or not line.strip():
        continue
    if cur is None:
        continue
    if line.startswith('[问]'):
        out.append(('帧:' + cur, '问法', enc(line[3:])))
    elif line.startswith('[答]'):
        out.append(('帧:' + cur, '答法', enc(line[3:])))
    elif line.startswith('[填:'):
        body = line[3:].rstrip(']')
        if '=' in body:
            body = body.split('=', 1)[1]
        words = []
        for h in body.split(','):
            h = h.strip()
            if not h: continue
            try:
                words.append(bytes.fromhex(h).decode('utf-8'))
            except Exception:
                pass
        if words:
            out.append(('帧:' + cur, '填充', ','.join(words)))
# 去重但保序
seen = set(); 
for a, b, c in out:
    k = (a, b, c)
    if k in seen: continue
    seen.add(k)
    print(f'{a}\t{b}\t{c}')
PYEOF
