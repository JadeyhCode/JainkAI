#!/usr/bin/env python3
"""tools/twicecheck.py — **"第一次 vs 第二次"检查**（§6.442）

同一句在**同一个进程里问两遍**，答复可能不同。不同的原因有好几种，必须分开看：

  · 历史注解（`（你 上次 问 的…）`）—— 复述提示，**不是行为变化**；
  · 语气前缀（`估计，`/`大概，`）—— 时序/模式造成，观感变化；
  · 真正的类型变化（兜底 ↔ 学习确认 ↔ 已知 ↔ 关系答复）—— 这才是要注意的。

用法：python3 tools/twicecheck.py [文件] [模型] [条数]
      默认读 tests/blind4.txt 的前 120 条、模型 model.bin。
"""
import re, subprocess, sys, collections

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tests/blind4.txt"
    model = sys.argv[2] if len(sys.argv) > 2 else "model.bin"
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    rows = [l.split("\t")[0] for l in open(path, encoding="utf-8")
            if not l.startswith("#") and "\t" in l][:limit]

    SUF = re.compile(r"（你 上次 问 的.*?）")
    PREF = re.compile(r"^(估计|大概|应该|可能|也许)，")
    REFUSE = ("这个我 不 太 懂", "我 不 知道 你 能 告诉 我 吗", "我 还 没 学 过 这个",
              "这 个 问题 我 答 不 上", "我 不 太 确定 你 说 给 我 听")

    def norm(s):
        return PREF.sub("", SUF.sub("", s).strip()).strip()

    REFUSE_F = tuple(x.replace(" ", "") for x in REFUSE)

    def kind(a):
        f = a.replace(" ", "")
        if any(r in f for r in REFUSE_F): return "拒绝/兜底"
        if "记下" in f: return "学习确认"
        if "我知道" in f: return "已知"
        return "其它"

    cats = collections.Counter()
    for q in rows:
        p = subprocess.run(["./build_aip", "--model", model], input=q + "\n" + q + "\n::quit\n",
                           capture_output=True, text=True, errors="replace", timeout=60).stdout
        ans = [norm(l.rsplit("AI > ", 1)[1]) for l in p.splitlines() if "AI > " in l]
        if len(ans) < 2 or ans[0] == ans[1]:
            continue
        cats[(kind(ans[0]), kind(ans[1]))] += 1

    print("抽样 %d 条；去掉历史注解与语气前缀后，类型变化：" % len(rows))
    bad = 0
    for (a, b), v in cats.most_common():
        print("  %s → %s：%d 条" % (a, b, v))
        if a != b and not (a == "学习确认" and b == "已知"):
            bad += v
    if bad:
        print("注意：%d 条属于**非预期**的类型变化（学习确认→已知 之外）" % bad)
    else:
        print("PASS twicecheck（差异只剩'拒绝措辞不同'和'学到后变已知'两类）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
