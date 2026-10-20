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
    # §6.452 泛化探针：已收形状的**换词变体**（非盲测，作者知道实现）
    if os.path.exists("tests/genprobe9.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe9.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe9（第九批，非盲测；基线 2/16 = 外壳不转移的证据） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe8.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe8.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe8（第八批，非盲测；基线 5/16，§6.465 起已受污染） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe7.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe7.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe7（第七批，非盲测；基线 2/16，§6.464 起已受污染） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe6.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe6.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe6（第六批，非盲测；§6.462 起已受污染） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe5.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe5.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe5（第五批，非盲测；改动前基线 9/16，§6.462 起已受污染） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe4.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe4.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe4（第四批，非盲测；§6.460 起已受污染） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe3.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe3.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe3（第三批，非盲测；§6.458 起已受污染） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe2.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe2.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe2（第二批，非盲测） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    if os.path.exists("tests/genprobe.txt"):
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/genprobe.txt"])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            print("| 泛化探针 genprobe（换词变体，非盲测） | **%s / %s** |" % (mm.group(1), mm.group(2)))
    out = run(["sh", "tests/holdout.sh", MODEL, "tests/holdout_pert.txt"])
    mm = re.search(r"（(\d+)/(\d+)）", out)
    if mm:
        print("| 扰动留出集 holdout_pert | **%s / %s** |" % (mm.group(1), mm.group(2)))
    # §6.426 盲测集（blind*）单独一行：它们是"新说法"，不是原来那 17 组
    bp = bt = 0
    for f in sorted(os.listdir("tests")):
        if not (f.startswith("blind") and f.endswith(".txt")):
            continue
        out = run(["sh", "tests/holdout.sh", MODEL, "tests/" + f])
        mm = re.search(r"（(\d+)/(\d+)）", out)
        if mm:
            bp += int(mm.group(1)); bt += int(mm.group(2))
    if bt:
        # 历史口径：17 组 + blind5；其余盲测（blind2-4,6-9）单独一行
        b5 = run(["sh", "tests/holdout.sh", MODEL, "tests/blind5.txt"])
        m5 = re.search(r"（(\d+)/(\d+)）", b5)
        p5, t5 = (int(m5.group(1)), int(m5.group(2))) if m5 else (0, 0)
        print("| blind5（历史口径里的那个） | **%d / %d** |" % (p5, t5))
        print("| **17 组 + blind5 合计（历史口径）** | **%d / %d** |" % (tp + p5, tt + t5))
        print("| 其余盲测（blind2-4,6-9） | **%d / %d** |" % (bp - p5, bt - t5))
    print()
    print("## 5. 复读率")
    print()
    for f in ["tests/corpus_q100c.txt", "tests/corpus_q100d.txt"]:
        out = run(["sh", "tests/replayrate.sh", MODEL, f]).splitlines()
        print("| `%s` | %s |" % (os.path.basename(f),
                                 out[0].replace("复读率：", "") if out else "?"))
PY
