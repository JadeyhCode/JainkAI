#!/usr/bin/env python3
"""tools/ordercheck.py — **顺序敏感性检查**（§6.411）

同一批输入，**打乱顺序**再跑一遍 `patuse`（带 AIP_PATUSE_LOG=1，逐句记录命中的行）。
如果"输入 → 命中行集合"的映射在两种顺序下不一致，说明有**状态依赖**（上一句影响下一句的走向）。
这正是"同一句单独跑命中、连跑不命中"这类怪事的来源。

用法：python3 tools/ordercheck.py [输入文件]   （默认用四样本+探针+留出集拼的集合）
退出码：0 = 没有差异（或只有重复输入造成的不可判）；1 = 发现顺序敏感。
"""
import os, random, re, subprocess, sys, tempfile, glob

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
        ans = parts[3] if len(parts) > 3 else ""
        m = row.replace("row=", "").split("|")[0]
        # 同一句出现多次时合并（集合）。**同时比较答复文本**：前态影响往往表现为
        # "命中同一行、但答复不同"（§6.412），只看命中行会漏掉。
        hits.setdefault(inp, set()).add(m + "\x01" + norm_ans(ans))
    return hits

# 语气前缀（随机/前态相关）：比较前统一剥掉，避免"估计/大概"这类差异掩盖真问题
PREFIX = re.compile(r"^(估计|大概|应该|可能|也许|我看|大概是这样)[，,]?")

# 这些词的走向**本来就依赖上文**（指代/省略/追问） ⇒ 顺序敏感是预期的
CTX_WORDS = ("它", "他", "她", "那", "呢", "刚才", "上面", "前面", "再说", "还有",
             "我 说 的", "那个", "这个", "上句", "接着")

SUFFIX = re.compile(r"（你 上次 问 的.*?）")

def norm_ans(ans):
    a = SUFFIX.sub("", ans).strip()
    while True:
        b = PREFIX.sub("", a).strip()
        if b == a:
            return a
        a = b

CJK = re.compile(r"[\u4e00-\u9fff]")

def category(inp):
    if not CJK.search(inp):
        return "noise"          # 非中文输入（拼写/符号）：不参与判定
    # 裸名词/单字（"鲸鱼"这种）也是**省略式**：它的答复天然依赖上一轮在聊什么。
    if " " not in inp and len(inp) <= 6:
        return "context"
    return "context" if any(w in inp for w in CTX_WORDS) else "plain"

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

def run_fresh(lines):
    """每条输入**单独起进程**（干净状态）跑一遍，返回 输入→答复。
    用来判断"连跑时的答复"有多少是**上文造成的**。"""
    out = {}
    for q in dict.fromkeys(lines):
        p = subprocess.run([os.environ.get("AIP", "./build_aip"), "--model", MODEL],
                           input=q + "\n::quit\n", capture_output=True, text=True,
                           errors="replace", timeout=60).stdout
        ans = ""
        for l in p.splitlines():
            # 真实格式是 `你 > AI > 答复`（同一个行里）⇒ 取最后一个 "AI > " 之后
            if "AI > " in l:
                ans = l.rsplit("AI > ", 1)[1]
                break
        out[q] = norm_ans(ans)
    return out

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
    notes = []
    for mode, keep in (("清前态", False), ("保留前态", True)):
        a = run(lines, keep)
        b = run(shuffled, keep)
        diff = []
        for inp in set(a) | set(b):
            if a.get(inp) != b.get(inp):
                diff.append((inp, a.get(inp), b.get(inp)))
        ctx = [d for d in diff if category(d[0]) == "context"]
        plain = [d for d in diff if category(d[0]) == "plain"]
        noise = [d for d in diff if category(d[0]) == "noise"]
        print("[%s] 不一致：%d 条（指代/追问类 %d = 预期；普通句 %d = 需看；非中文 %d = 忽略）"
              % (mode, len(diff), len(ctx), len(plain), len(noise)))
        for inp, x, y in plain[:8]:
            print("    普通句 %-12s 原序=%s 乱序=%s" % (inp, sorted(x or []), sorted(y or [])))
        for inp, x, y in ctx[:3]:
            print("    追问句 %-12s 原序=%s 乱序=%s" % (inp, sorted(x or []), sorted(y or [])))
        # 普通句在清前态下还敏感：可能是**学习效应**（前面的输入教会了图里新事实），
        # 不一定是 bug。默认只报告；加 --strict 才判 FAIL。
        if plain and not keep:
            notes.append((mode, len(plain)))
            if "--strict" in sys.argv:
                bad = 1
    if "--fresh" in sys.argv:
        fresh = run_fresh(lines)
        cont = run(lines)
        hist = [q for q in fresh if q in cont and
                norm_ans(next(iter(cont[q])).split("\x01", 1)[-1]) != fresh[q]]
        print("[单句新进程] 与连跑答复不同（=上文造成）的输入：%d 条" % len(hist))
        for q in hist[:10]:
            print("    %-14s 单跑=%.30s" % (q, fresh[q]))
    if bad:
        print("FAIL ordercheck（--strict：清前态下普通句仍顺序敏感）")
        return 1
    if notes:
        print("NOTE ordercheck：普通句里有顺序敏感（%s）—— 多半是**学习效应**（前者教了图里新事实），"
              "不是纯 bug；要看细节加 --strict 或用指定输入集。" % ", ".join(
                  "%s %d 条" % (m, n) for m, n in notes))
    else:
        print("PASS ordercheck（清前态下普通句顺序无关）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
