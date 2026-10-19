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
import hashlib, datetime

def sha8(path):
    try:
        return hashlib.sha256(open(path, "rb").read()).hexdigest()[:8]
    except OSError:
        return "?"

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

print("<!-- trend-meta: binary=%s data=%s model=%s fast=%d generated=%s -->" % (
    sha8(BIN), sha8("patterns.txt"), sha8(MODEL), 1 if FAST else 0,
    datetime.date.today().isoformat()))
print("## 1. 语料问句四样本（含**答复来源**：固定答复行 vs 其它路径）")
print()
print("| 样本 | 条数 | 给事实 | 诚实未知 | 兜底 | 固定答复占比 | 其中真答复 | 其中边界声明 |")
print("|---|---|---|---|---|---|---|---|")
for f in ["tests/corpus_q100.txt", "tests/corpus_q100b.txt",
          "tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
    line = run(["sh", "tests/corpusprobe.sh", MODEL, "0", f]).splitlines()
    got = pcts(line[0]) if line else None
    # §6.418 答复来源再拆一层：**真·现成答复** vs **能力/边界声明**（都在 reply 桶里）
    share = "?"
    real_share = "?"
    bound_share = "?"
    try:
        qs = [l.split("\t")[0].strip() for l in open(f, encoding="utf-8")
              if l.strip() and not l.startswith("#")]
        env2 = dict(os.environ); env2["AIP_PATUSE_LOG"] = "1"
        pu = subprocess.run(["./build_patuse", MODEL], input="\n".join(qs) + "\n",
                            capture_output=True, text=True, errors="replace", env=env2).stdout
        # 读 patterns.txt 里 [reply] 行的文本（行号 = 加载顺序 = LOG 里的 row 下标）
        texts = [l.split("=", 1)[1].strip() for l in open("patterns.txt", encoding="utf-8")
                 if l.startswith("[reply]") and "=" in l]
        BOUND = re.compile(r"不 会|不 认 识|不 知 道|没 有|不 记|不 联 网|不 能|做 不 到|不 敢|"
                           r"不 编|不 太 懂|只 能|没 办 法")
        hit = miss = real = bound = 0
        for l in pu.splitlines():
            if not l.startswith("LOG|"):
                continue
            if "|row=-1|" in l:
                miss += 1
                continue
            hit += 1
            m = re.search(r"\|row=(\d+)\|", l)
            idx = int(m.group(1)) if m else -1
            txt = texts[idx] if 0 <= idx < len(texts) else ""
            if BOUND.search(txt):
                bound += 1
            else:
                real += 1
        if hit + miss:
            share = "%.0f%%" % (100.0 * hit / (hit + miss))
            real_share = "%.0f%%" % (100.0 * real / (hit + miss))
            bound_share = "%.0f%%" % (100.0 * bound / (hit + miss))
    except Exception:
        pass
    if got:
        print("| `%s` | %s | %s%% | %s%% | %s%% | %s | %s | %s |" % (
            os.path.basename(f), *got, share, real_share, bound_share))
    else:
        print("| `%s` | ? | ? | ? | ? | %s | %s | %s |" % (
            os.path.basename(f), share, real_share, bound_share))

# §6.417 **A vs B 差距**（调参集 vs 分布外）：每轮看这一行，就是"有没有过拟合"的体温计
try:
    def fb(ff):
        ll = run(["sh", "tests/corpusprobe.sh", MODEL, "0", ff]).splitlines()
        gg = pcts(ll[0]) if ll else None
        return float(gg[3].rstrip("%")) if gg else None
    fa, fb_ = fb("tests/corpus_q100.txt"), fb("tests/corpus_q100b.txt")
    if fa is not None and fb_ is not None:
        print()
        print("**A vs B 兜底差**：A %.1f%% / B %.1f%% ⇒ **差 %.1f 个百分点**（差越小越不像过拟合）"
              % (fa, fb_, fb_ - fa))
except Exception:
    pass

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

print()
print("## 3. 数据卫生（reply 行使用率）")
print()
try:
    pu = subprocess.run(["sh", "tools/probeunhit.sh"], capture_output=True, text=True,
                        errors="replace", timeout=900).stdout
except subprocess.TimeoutExpired:
    pu = ""
m1 = re.search(r"未命中行 (\d+) 条；造出探针 (\d+) 条", pu)
m2 = re.search(r"小结：(\d+) 条同答复.*?(\d+) 条被抢答.*?(\d+) 条仍兜底", pu)
if m1:
    print("| 未命中的 reply 行 | %s |" % m1.group(1))
    print("| 能造探针的行 | %s |" % m1.group(2))
if m2:
    print("| 同答复 / 被抢答 / 兜底 | %s / %s / %s |" % (m2.group(1), m2.group(2), m2.group(3)))
if not (m1 or m2):
    print("| （probeunhit 没跑出结果） | ? |")

if not FAST:
    print()
    print("## 4. 留出集")
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
    print("## 5. 复读率")
    print()
    for f in ["tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
        out = run(["sh", "tests/replayrate.sh", MODEL, f]).splitlines()
        print("| `%s` | %s |" % (os.path.basename(f),
                                 out[0].replace("复读率：", "") if out else "?"))
PY
