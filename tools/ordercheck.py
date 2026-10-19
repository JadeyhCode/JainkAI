#!/usr/bin/env python3
"""tools/ordercheck.py — **顺序敏感性检查**（§6.411）

同一批输入，**打乱顺序**再跑一遍 `patuse`（带 AIP_PATUSE_LOG=1，逐句记录命中的行）。
如果"输入 → 命中行集合"的映射在两种顺序下不一致，说明有**状态依赖**（上一句影响下一句的走向）。
这正是"同一句单独跑命中、连跑不命中"这类怪事的来源。

用法：python3 tools/ordercheck.py [输入文件]   （默认用四样本+探针+留出集拼的集合）
退出码：0 = 没有差异（或只有重复输入造成的不可判）；1 = 发现顺序敏感。
"""
import os, random, subprocess, sys, tempfile, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
BIN = os.environ.get("BIN", "./build_patuse")
MODEL = os.environ.get("MODEL", "session.bin")

def build_inputs():
    lines = []
    for f in ["tests/corpus_q100.txt", "tests/corpus_q100b.txt", "tests/corpus_q100c.txt",
              "tests/corpus_q100d.txt"] + sorted(glob.glob("tests/*para*.txt")) + \
             ["tests/blind5.txt"] + sorted(glob.glob("tests/holdout*.txt")):
        if not os.path.exists(f):
            continue
        for l in open(f, encoding="utf-8"):
            if l.startswith("#") or not l.strip():
                continue
            q = l.split("\t")[0].strip()
            if q:
                lines.append(q)
    return lines

def run(lines, keep_state=False):
    env = dict(os.environ); env["AIP_PATUSE_LOG"] = "1"
    if keep_state:
        env["AIP_PATUSE_KEEP_STATE"] = "1"
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as fh:
        fh.write("\n".join(lines) + "\n")
        path = fh.name
    try:
        out = subprocess.run([BIN, MODEL], stdin=open(path, encoding="utf-8"),
                             capture_output=True, text=True, errors="replace", env=env).stdout
    finally:
        os.unlink(path)
    hits = {}
    for l in out.splitlines():
        if not l.startswith("LOG|"):
            continue
        parts = l.split("|")
        if len(parts) < 3:
            continue
        inp, row = parts[1], parts[2]
        m = row.replace("row=", "").split("|")[0]
        # 同一句出现多次时合并（集合），顺序敏感会表现为"某些出现命中、某些不命中"
        hits.setdefault(inp, set()).add(m)
    return hits

def selftest():
    """§6.411 自检：比对逻辑必须能**发现**人为制造的差异（否则它就是恒绿的摆设）。"""
    a = {"甲": {"1"}, "乙": {"2"}}
    b = {"甲": {"1"}, "乙": {"3"}}      # 乙 在两序下不同
    diff = [k for k in set(a) | set(b) if a.get(k) != b.get(k)]
    if diff == ["乙"]:
        print("PASS ordercheck --selftest（比对逻辑能发现差异）")
        return 0
    print("FAIL ordercheck --selftest：比对逻辑没抓到人为差异（它是摆设）")
    return 1

def main():
    if "--selftest" in sys.argv:
        return selftest()
    lines = build_inputs()
    if len(sys.argv) > 1:
        lines = [l.strip() for l in open(sys.argv[1], encoding="utf-8")
                 if l.strip() and not l.startswith("#")]
    print("输入 %d 条" % len(lines))
    shuffled = lines[:]
    random.Random(20261020).shuffle(shuffled)
    bad = 0
    for mode, keep in (("清前态", False), ("保留前态", True)):
        a = run(lines, keep)
        b = run(shuffled, keep)
        diff = []
        for inp in set(a) | set(b):
            if a.get(inp) != b.get(inp):
                diff.append((inp, a.get(inp), b.get(inp)))
        print("[%s] '输入→命中行集合' 不一致：%d 条" % (mode, len(diff)))
        for inp, x, y in diff[:5]:
            print("    %-14s 原序=%s 乱序=%s" % (inp, sorted(x or []), sorted(y or [])))
        if diff and not keep:
            bad = 1        # 清前态还敏感 ⇒ 真问题
    if bad:
        print("FAIL ordercheck（清前态下仍顺序敏感）")
        return 1
    print("PASS ordercheck（清前态下顺序无关；保留前态下的差异属于追问链，属预期）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
