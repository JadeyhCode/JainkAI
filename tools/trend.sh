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

# §6.421 **先重建 patuse**：否则它会用旧二进制统计（踩过：加了标签，表里却没变，
# 因为 build_patuse 是上一版编译的）。放在任何测量之前。
subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "-o", "build_patuse", "tools/patuse.c", "-lm"], check=False)

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
print("| 样本 | 条数 | 给事实 | 诚实未知 | 兜底 | 数据行 | 关系层 | 兜底池 | 框架 | 检索/说明 | 自我/裸 | 未标注 |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for f in ["tests/corpus_q100.txt", "tests/corpus_q100b.txt",
          "tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
    line = run(["sh", "tests/corpusprobe.sh", MODEL, "0", f]).splitlines()
    got = pcts(line[0]) if line else None
    # §6.420 答复来源：用**代码路径标签**（patuse 的 SRC 行），不再猜文本
    real_share = bound_share = share = other_share = "?"
    self_share = bare_share = ret_share = "?"
    try:
        qs = [l.split("\t")[0].strip() for l in open(f, encoding="utf-8")
              if l.strip() and not l.startswith("#")]
        env2 = dict(os.environ); env2["AIP_PATUSE_SRC"] = "1"
        pu = subprocess.run(["./build_patuse", MODEL], input="\n".join(qs) + "\n",
                            capture_output=True, text=True, errors="replace", env=env2).stdout
        cnt = {"reply": 0, "self": 0, "bare": 0, "relation": 0, "fallback": 0,
               "frame": 0, "retrieval": 0, "note": 0, "other": 0}
        for l in pu.splitlines():
            if l.startswith("SRC|"):
                _, src, _inp = l.split("|", 2)
                cnt[src] = cnt.get(src, 0) + 1
        tot = sum(cnt.values())
        if tot:
            pct = lambda x: "%.0f%%" % (100.0 * x / tot)
            real_share = pct(cnt["reply"])          # 数据行答复
            bound_share = pct(cnt["relation"])      # 关系层
            share = pct(cnt["fallback"])            # 兜底池

            self_share = pct(cnt["self"] + cnt["bare"])
            bare_share = pct(cnt["frame"])
            other_share = pct(cnt["other"])
            ret_share = pct(cnt["retrieval"] + cnt["note"])
    except Exception:
        pass
    if got:
        print("| `%s` | %s | %s%% | %s%% | %s%% | %s | %s | %s | %s | %s | %s | %s |" % (
            os.path.basename(f), *got, real_share, bound_share, share,
            bare_share, ret_share, self_share, other_share))
    else:
        print("| `%s` | ? | ? | ? | ? | %s | %s | %s | %s | %s | %s | %s |" % (
            os.path.basename(f), real_share, bound_share, share,
            bare_share, ret_share, self_share, other_share))

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
        if not (f.startswith("holdout") and f.endswith(".txt")):
            continue
        if "pert" in f:
            continue
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/" + f])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            tp += int(mm.group(1)); tt += int(mm.group(2))
    print("| 十七组留出集合计（不含 pert） | **%d / %d** |" % (tp, tt))
    # §6.469 泛化探针折成一张小表（11 批太占版面）：基线 = 那一批"改动前写"时的通过数。
    probes = [
        ("genprobe",   None, None),   # 换词变体（§6.452）
        ("genprobe2",  5, 14),
        ("genprobe3",  8, 16),
        ("genprobe4",  3, 16),
        ("genprobe5",  9, 16),
        ("genprobe6",  2, 16),
        ("genprobe7",  2, 16),
        ("genprobe8",  5, 16),
        ("genprobe9",  2, 16),
        ("genprobe10", 4, 16),
        ("genprobe11", 5, 16),
        ("genprobe12", 3, 16),
        ("genprobe13", 11, 16),
        ("genprobe14", 6, 16),
        ("genprobe15", 0, 16),
        ("genprobe16", 8, 16),
        ("genprobe17", 7, 16),
        ("genprobe18", 11, 16),
        ("genprobe19", 15, 16),
        ("genprobe20", 2, 16),
        ("genprobe21", 6, 16),
    ]
    rows = []
    for name, base, tot in probes:
        fn = "tests/%s.txt" % name
        if not os.path.exists(fn):
            continue
        out = run(["sh", "tests/holdout.sh", MODEL, fn])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if not mm:
            continue
        cur, total = mm.group(1), mm.group(2)
        b = "%s/%s" % (base, tot) if base is not None else "—"
        rows.append((name, b, "%s/%s" % (cur, total)))
    if rows:
        print("| 探针 | 改动前基线 | 现在 |")
        print("|---|---|---|")
        for name, b, cur in rows:
            print("| %s | %s | **%s** |" % (name, b, cur))
    print()
    print("## 5. 复读率")
    print()
    for f in ["tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
        out = run(["sh", "tests/replayrate.sh", MODEL, f]).splitlines()
        print("| `%s` | %s |" % (os.path.basename(f),
                                 out[0].replace("复读率：", "") if out else "?"))
PY
