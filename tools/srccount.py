#!/usr/bin/env python3
"""tools/srccount.py — **答复来源分布**（§6.455）

同一进程里喂一个问句文件，用 `AIP_SRC_ECHO=1` 读 stderr 的 `[src] X`，统计每种来源各多少。

用法：python3 tools/srccount.py tests/corpus_q100.txt [模型]
      给两个文件就并在一起统计。
"""
import re, subprocess, sys, collections

def labels(fn, model):
    rows = [l.split("\t")[0] for l in open(fn, encoding="utf-8").read().splitlines()
            if l.strip() and not l.startswith("#")]
    rows = [r.split("\t")[0] for r in rows]
    inp = ("::load %s\n::char off\n::notepad off\n::char off\n::hypo cache off\n"
           "::rel analogy off\n::rel analogy solver off\n" % model) + "\n".join(rows) + "\n::quit\n"
    p = subprocess.run(["./build_aip"], input=inp, capture_output=True, text=True,
                       errors="replace", env={"AIP_SRC_ECHO": "1", "PATH": "/usr/bin:/bin"},
                       timeout=900)
    return re.findall(r"\[src\] (\w+)", p.stderr)

def main():
    files = sys.argv[1:2] or ["tests/corpus_q100.txt"]
    model = sys.argv[2] if len(sys.argv) > 2 else "session.bin"
    c = collections.Counter()
    for fn in files:
        c.update(labels(fn, model))
    n = sum(c.values())
    if not n:
        print("没读到 [src]（模型或文件不对？）")
        return 1
    print("来源分布（%d 条）:" % n)
    for k, v in c.most_common():
        print("  %-10s %4d  (%2.0f%%)" % (k, v, 100.0 * v / n))
    return 0

if __name__ == "__main__":
    sys.exit(main())
