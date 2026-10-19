#!/bin/sh
# tools/trend.sh — **趋势快照生成**（§6.400）：表格由命令跑出来，不手抄
#
# 为什么要有：§6.399 手写快照把 reply 行数写成 74（实际 73），是"两个来源对一遍"才发现的。
# 表格必须由命令生成，否则快照本身就会腐坏。
#
# 用法：
#   sh tools/trend.sh            # 全量（四样本 + 数据层 + 留出集 + 复读率，几分钟）
#   sh tools/trend.sh --fast     # 只跑四样本 + 数据层（约 1 分钟）
# 输出 markdown 片段到 stdout；`docs/trend.md` 的说明段是手写的，别直接覆盖整份文件。
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
exec python3 - "$@" <<'PY'
import os, re, subprocess, sys

FAST = "--fast" in sys.argv
BIN = os.environ.get("BIN", "./build_aip")
MODEL = os.environ.get("MODEL", "session.bin")

def run(cmd, timeout=400):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    except subprocess.TimeoutExpired:
        return ""

def pcts(line):
    m = re.search(r"问句 (\d+) 条：给事实 (\d+)（([\d.]+)%）／诚实未知 (\d+)（([\d.]+)%）／兜底 (\d+)（([\d.]+)%）", line)
    if not m:
        return None
    return m.group(1), m.group(3), m.group(5), m.group(7)

print("## 1. 语料问句四样本")
print()
print("| 样本 | 条数 | 给事实 | 诚实未知 | 兜底 |")
print("|---|---|---|---|---|")
for f in ["tests/corpus_q100.txt", "tests/corpus_q100b.txt",
          "tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
    line = run(["sh", "tests/corpusprobe.sh", MODEL, "0", f]).splitlines()
    got = pcts(line[0]) if line else None
    if got:
        print("| `%s` | %s | %s%% | %s%% | %s%% |" % (os.path.basename(f), *got))
    else:
        print("| `%s` | ? | ? | ? | ? |" % os.path.basename(f))

print()
print("## 2. 数据层规模")
print()
print("| 桶 | 文件行数 | 加载数 |")
print("|---|---|---|")
env = dict(os.environ); env["AIP_PATDBG"] = "1"
dbg = subprocess.run([BIN, "--model", MODEL], input="::quit\n",
                     capture_output=True, text=True, timeout=60, env=env).stderr
m = re.search(r"\[patterns\] (lead=.*)", dbg)
counts = dict(re.findall(r"(\w+)=(\d+)", m.group(1))) if m else {}
src = open("patterns.txt", encoding="utf-8").read()
for k in ["lead", "slot", "confuse", "reply", "shape", "syn", "affix", "tail", "fill", "class"]:
    rows = len(re.findall(r"^\[%s\]" % k, src, re.M))
    print("| `%s` | %d | %s |" % (k, rows, counts.get(k, "?")))

if not FAST:
    print()
    print("## 3. 留出集")
    print()
    tp = tt = 0
    for f in sorted(os.listdir("tests")):
        if not (f.startswith("holdout") and f.endswith(".txt")) and f != "blind5.txt":
            continue
        if "pert" in f:
            continue
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/" + f])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            tp += int(mm.group(1)); tt += int(mm.group(2))
    print("| 十七组留出集合计 | **%d / %d** |" % (tp, tt))
    print()
    print("## 4. 复读率")
    print()
    for f in ["tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
        out = run(["sh", "tests/replayrate.sh", MODEL, f]).splitlines()
        print("| `%s` | %s |" % (os.path.basename(f),
                                 out[0].replace("复读率：", "") if out else "?"))
PY
