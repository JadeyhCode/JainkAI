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
    ok(strstr(out, "规则") == NULL, "未知时**不编**证明步骤");

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

    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
