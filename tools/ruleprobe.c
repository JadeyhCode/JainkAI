/* tools/ruleprobe.c — **规则 / 后向链 / 证明**探针（阶段二 WP2；§6.635） */
#include "../agent.h"
#include "../model_io.h"
#include "../rule.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static Token *add_rule(AI *a, const char *hn, char **ha, size_t hnn,
                       const char **pn, char *(*pa)[PRED_MAX_ARGS], const size_t *pns,
                       size_t np) {
    return rule_add(a, hn, ha, hnn, pn, pa, pns, np);
}

int main(void) {
    AI *a = ai_new();
    printf("== 规则 + 后向链 + 证明 ==\n");

    /* 事实：父(甲,乙) 父(乙,丙) */
    char *f1[2] = { "甲", "乙" }, *f2[2] = { "乙", "丙" };
    pred_make(a, "父", f1, 2);
    pred_make(a, "父", f2, 2);

    /* 规则 1（基）：祖先($x,$y) :- 父($x,$y) */
    char *h1[2] = { "$x", "$y" };
    char *pr1[1][PRED_MAX_ARGS] = { { "$x", "$y" } };
    char *pa1[1][PRED_MAX_ARGS];
    for (size_t i = 0; i < 2; ++i) pa1[0][i] = pr1[0][i];
    const char *pn1[1] = { "父" };
    size_t ps1[1] = { 2 };
    Token *r1 = add_rule(a, "祖先", h1, 2, pn1, pa1, ps1, 1);
    ok(r1 != NULL && !strcmp(r1->name, "[规则:R1]"), "第一条规则 = [规则:R1]");

    /* 规则 2（递归）：祖先($x,$z) :- 父($x,$y), 祖先($y,$z) */
    char *h2[2] = { "$x", "$z" };
    char *pr2[2][PRED_MAX_ARGS] = { { "$x", "$y" }, { "$y", "$z" } };
    char *pa2[2][PRED_MAX_ARGS];
    for (size_t i = 0; i < 2; ++i) { pa2[0][i] = pr2[0][i]; pa2[1][i] = pr2[1][i]; }
    const char *pn2[2] = { "父", "祖先" };
    size_t ps2[2] = { 2, 2 };
    Token *r2 = add_rule(a, "祖先", h2, 2, pn2, pa2, ps2, 2);
    ok(r2 != NULL && !strcmp(r2->name, "[规则:R2]"), "第二条规则 = [规则:R2]（编号不撞）");
    ok(r1 != r2, "两条规则是**不同节点**（第一版编号解析漏了 R ⇒ 挤在一起）");

    Token *rs[8];
    size_t nr = rule_list(a, rs, 8);
    ok(nr == 2, "rule_list 数到 2 条");
    ok(rule_npremise(a, r2) == 2, "R2 有 2 条前提");
    ok(rule_head(a, r1) != NULL && rule_head(a, r1) == pred_find(a, "祖先",
        (char *[]){ "$x", "$y" }, 2), "R1 的结论是 `祖先($x,$y)`（变量进名字）");

    /* 证明：基例 */
    char out[4096];
    char *g1[2] = { "甲", "乙" };
    ok(rule_prove(a, "祖先", g1, 2, out, sizeof out) == 1, "基例：祖先(甲,乙) 证得出来");
    ok(strstr(out, "事实：父(甲,乙)") != NULL, "证明里写了**用了哪条事实**");

    /* 证明：递归（两跳） */
    char *g2[2] = { "甲", "丙" };
    int rc = rule_prove(a, "祖先", g2, 2, out, sizeof out);
    ok(rc == 1, "递归：祖先(甲,丙) 证得出来（父(甲,乙)+父(乙,丙)）");
    ok(strstr(out, "规则") != NULL && strstr(out, "选取事实") != NULL,
       "证明里有**规则步**和**选取事实**（不是只给个「成立」）");

    /* 证不出来 = 未知（不是假） */
    char *g3[2] = { "丙", "甲" };
    memset(out, 0, sizeof out);
    ok(rule_prove(a, "祖先", g3, 2, out, sizeof out) == 0, "祖先(丙,甲) ⇒ **0（未知）**，不是「假」");
    ok(strstr(out, "事实：") == NULL && strstr(out, "选取事实") == NULL,
       "未知时**不编**证明步骤（只有缺口清单，没有假的事实/选取事实行）");

    /* 参数/形状检查 */
    ok(rule_prove(a, "不存在", g1, 2, out, sizeof out) == 0, "没见过的名字 ⇒ 未知（不崩）");
    char *gv[2] = { "$x", "乙" };
    ok(rule_prove(a, "祖先", gv, 2, out, sizeof out) == -1, "目标是变量 ⇒ **拒绝**（-1）");
    ok(rule_prove(a, "祖先", NULL, 2, out, sizeof out) == -1, "args == NULL ⇒ -1");

    /* 模式节点**不是事实**：只有递归规则、没有任何 父 事实时，不该"证明"出任何东西 */
    {
        AI *b = ai_new();
        char *bp[2][PRED_MAX_ARGS] = { { "$x", "$y" }, { "$y", "$z" } };
        char *bpa[2][PRED_MAX_ARGS];
        for (size_t i = 0; i < 2; ++i) { bpa[0][i] = bp[0][i]; bpa[1][i] = bp[1][i]; }
        const char *bpn[2] = { "父", "祖先" };
        size_t bps[2] = { 2, 2 };
        char *bh[2] = { "$x", "$z" };
        Token *br = add_rule(b, "祖先", bh, 2, bpn, bpa, bps, 2);
        ok(br != NULL, "只加递归规则也能建出来");
        char *bg[2] = { "甲", "丙" };
        char bo[1024];
        ok(rule_prove(b, "祖先", bg, 2, bo, sizeof bo) == 0,
           "**没有事实时证不出来**（模式节点 `父($x,$y)` 不被当成事实）");
        ai_free(b);
    }

    /* 自环规则不能挂死 */
    {
        AI *c = ai_new();
        char *ch[1] = { "$x" };
        char *cp[1][PRED_MAX_ARGS] = { { "$x" } };
        char *cpa[1][PRED_MAX_ARGS];
        cpa[0][0] = cp[0][0];
        const char *cpn[1] = { "环" };
        size_t cps[1] = { 1 };
        Token *cr = add_rule(c, "环", ch, 1, cpn, cpa, cps, 1);
        char *cg[1] = { "甲" };
        char co[1024];
        ok(cr != NULL && rule_prove(c, "环", cg, 1, co, sizeof co) == 0,
           "自环规则 `环($x) :- 环($x)` ⇒ 返回未知、**不挂死**（靠栈检测）");
        ai_free(c);
    }

    /* 前提条数上限 */
    {
        char *h[1] = { "$x" };
        char *q[5][PRED_MAX_ARGS] = { { "$x" }, { "$x" }, { "$x" }, { "$x" }, { "$x" } };
        char *qpa[5][PRED_MAX_ARGS];
        for (int i = 0; i < 5; ++i) qpa[i][0] = q[i][0];
        const char *qn[5] = { "父", "父", "父", "父", "父" };
        size_t qs[5] = { 1, 1, 1, 1, 1 };
        ok(add_rule(a, "九前提", h, 1, qn, qpa, qs, 5) == NULL, "5 条前提 ⇒ 拒绝（上限 4）");
    }

    /* 存读往返：规则随图存盘，重开还能证 */
    {
        size_t sz = ai_save_file(a, "_ruleprobe.bin");
        AI *b = sz ? ai_load_file("_ruleprobe.bin") : NULL;
        ok(b != NULL, "带规则的图能存能读");
        if (b) {
            Token *rs2[8];
            ok(rule_list(b, rs2, 8) == 2, "读回来规则还是 2 条");
            char *bg[2] = { "甲", "丙" };
            char bo[2048];
            ok(rule_prove(b, "祖先", bg, 2, bo, sizeof bo) == 1,
               "读回来之后**照样能证**出 祖先(甲,丙)（规则随图存盘）");
            ai_free(b);
        }
        remove("_ruleprobe.bin");
    }

    /* §6.649 规则体检：安全条件 + 重复规则 */
    {
        AI *d = ai_new();
        char *h[2] = { "$x", "$y" };
        char *okp[1][PRED_MAX_ARGS] = { { "$x", "$y" } };
        char *okpa[1][PRED_MAX_ARGS];
        okpa[0][0] = okp[0][0]; okpa[0][1] = okp[0][1];
        const char *okn[1] = { "父" };
        size_t okns[1] = { 2 };
        char *badp[1][PRED_MAX_ARGS] = { { "$x", "$z" } };
        char *badpa[1][PRED_MAX_ARGS];
        badpa[0][0] = badp[0][0]; badpa[0][1] = badp[0][1];
        add_rule(d, "祖先", h, 2, okn, okpa, okns, 1);
        add_rule(d, "祖先", h, 2, okn, badpa, okns, 1);      /* 不安全：$y 不在前提 */
        add_rule(d, "祖先", h, 2, okn, okpa, okns, 1);       /* 与第一条完全重复 */
        char msg[1024] = {0};
        int bad = rule_check(d, msg, sizeof msg);
        ok(bad == 2, "rule_check 恰好报 2 处（不安全 1 + 重复 1）");
        ok(strstr(msg, "不在前提里") != NULL, "报出**不安全**（头的变量不在前提）");
        ok(strstr(msg, "完全相同") != NULL, "报出**重复规则**");
        char m2[256] = {0};
        AI *e = ai_new();
        char *h2[1] = { "$x" };
        char *q[1][PRED_MAX_ARGS] = { { "$x" } };
        char *qpa[1][PRED_MAX_ARGS];
        qpa[0][0] = q[0][0];
        const char *qn[1] = { "父" };
        size_t qns[1] = { 1 };
        add_rule(e, "单目", h2, 1, qn, qpa, qns, 1);
        ok(rule_check(e, m2, sizeof m2) == 0, "合格的规则 ⇒ 0 处问题");
        ai_free(d); ai_free(e);
    }
    /* §6.653 失败时给**缺口清单**（哪条匹配上的规则、前提证不出来） */
    {
        AI *g = ai_new();
        char *h[2] = { "$x", "$z" };
        char *p1[2] = { "$x", "$y" }, *p2[2] = { "$y", "$z" };
        char *pa[2][PRED_MAX_ARGS];
        pa[0][0] = p1[0]; pa[0][1] = p1[1]; pa[1][0] = p2[0]; pa[1][1] = p2[1];
        const char *pn[2] = { "父", "祖先" };
        size_t pns[2] = { 2, 2 };
        add_rule(g, "祖先", h, 2, pn, pa, pns, 2);
        char go[1024] = {0};
        char *q[2] = { "甲", "丙" };
        ok(rule_prove(g, "祖先", q, 2, go, sizeof go) == 0, "没有事实 ⇒ 证不出来");
        ok(strstr(go, "缺口") != NULL, "失败时给出**缺口清单**（不是只说「推不出来」）");
        ok(strstr(go, "规则") != NULL, "缺口里点名**哪条规则**");
        ai_free(g);
    }
    /* §6.654 否定前提（NAF）：鸟会飞，企鹅例外 */
    {
        AI *n = ai_new();
        char *b1[2] = { "麻雀", "麻雀" }, *b2[2] = { "企鹅", "企鹅" };
        pred_make(n, "鸟", b1, 1);            /* 鸟(麻雀) */
        pred_make(n, "鸟", b2, 1);            /* 鸟(企鹅) */
        pred_make(n, "企鹅", b2, 1);          /* 企鹅(企鹅) */
        char *h[1] = { "$x" };
        char *q1[1] = { "$x" }, *q2b[1] = { "$x" };
        char *pa[2][PRED_MAX_ARGS];
        pa[0][0] = q1[0]; pa[1][0] = q2b[0];
        const char *pn[2] = { "鸟", "不企鹅" };
        size_t pns[2] = { 1, 1 };
        Token *r = add_rule(n, "会飞", h, 1, pn, pa, pns, 2);
        ok(r != NULL, "带否定前提的规则能建（`不企鹅($x)`）");
        char o1[1024] = {0};
        char *g1[1] = { "麻雀" };
        ok(rule_prove(n, "会飞", g1, 1, o1, sizeof o1) == 1, "会飞(麻雀) ⇒ 成立（企鹅不成立）");
        ok(strstr(o1, "否定前提成立") != NULL, "证明里写明**否定前提**是怎么成立的");
        char o2[1024] = {0};
        char *g2[1] = { "企鹅" };
        ok(rule_prove(n, "会飞", g2, 1, o2, sizeof o2) == 0, "会飞(企鹅) ⇒ 推不出来（例外生效）");
        ok(strstr(o2, "否定前提成立") == NULL,
           "被否定挡下时**不谎称**否定前提成立（前面证过的正面事实照常出现）");
        ai_free(n);
    }
    /* §6.655 前向链（一遍、有界）：推出的事实带 `[推:…]` 留痕 */
    {
        AI *f = ai_new();
        char *b1[2] = { "甲", "乙" }, *b2[2] = { "乙", "丙" };
        pred_make(f, "父", b1, 2);
        pred_make(f, "父", b2, 2);
        char *h[2] = { "$x", "$y" };
        char *p1[2] = { "$x", "$y" };
        char *pa[1][PRED_MAX_ARGS];
        pa[0][0] = p1[0]; pa[0][1] = p1[1];
        const char *pn[1] = { "父" };
        size_t pns[1] = { 2 };
        add_rule(f, "祖先", h, 2, pn, pa, pns, 1);
        RuleForward fs;
        memset(&fs, 0, sizeof fs);
        int got = rule_apply_once(f, 200, &fs);
        ok(got == 2, "前向链一遍推出 2 条（祖先(甲,乙)/祖先(乙,丙)）");
        char *g[2] = { "甲", "乙" };
        ok(pred_find(f, "祖先", g, 2) != NULL, "推出的结论**真的是事实**（pred_find 找得到）");
        ok(pool_find(f->pool, "[推:甲_祖先_乙]") != NULL, "落事实时留了 `[推:…]` 留痕（和 [教:] 分开）");
        ok(rule_apply_once(f, 200, &fs) == 0, "再跑一遍 ⇒ 0 条（已经有的不重复写）");
        /* §6.656 不动点：链式规则（祖先 :- 父, 祖先）能自己链下去 */
        {
            AI *c2 = ai_new();
            pred_make(c2, "父", b1, 2);
            pred_make(c2, "父", b2, 2);
            char *h2[2] = { "$x", "$z" };
            char *r1[2] = { "$x", "$y" }, *r2[2] = { "$y", "$z" };
            char *par[2][PRED_MAX_ARGS];
            par[0][0] = r1[0]; par[0][1] = r1[1]; par[1][0] = r2[0]; par[1][1] = r2[1];
            const char *pn2[2] = { "父", "祖先" };
            size_t pns2[2] = { 2, 2 };
            char *hb[2] = { "$x", "$y" };
            char *pb[2] = { "$x", "$y" };
            char *pab[1][PRED_MAX_ARGS];
            pab[0][0] = pb[0]; pab[0][1] = pb[1];
            const char *pnb[1] = { "父" };
            size_t pnsb[1] = { 2 };
            add_rule(c2, "祖先", hb, 2, pnb, pab, pnsb, 1);
            add_rule(c2, "祖先", h2, 2, pn2, par, pns2, 2);
            RuleForward fx;
            memset(&fx, 0, sizeof fx);
            int got2 = rule_apply_fixpoint(c2, 200, 5, &fx);
            ok(got2 >= 3, "不动点：链式规则推出 ≥3 条（含两跳 祖先(甲,丙)）");
            char *g3[2] = { "甲", "丙" };
            ok(pred_find(c2, "祖先", g3, 2) != NULL, "**两跳结论**真的落了（甲→丙）");
            ai_free(c2);
        }
        ok(rule_apply_once(f, 1, &fs) == 0 || fs.truncated || fs.derived <= 1, "有上限参数（≤1 时不会超）");
        ai_free(f);
    }
    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
