/* tools/dupcheck.c — **清单自检**：同一份清单不许抄两份、表里不许有重复项
 *
 * 这一套盯的是「滥用硬编码」里最隐蔽的那一类：**清单本身重复**。
 * 它有两种形态，都不崩溃，只是让行为悄悄分叉或让文档骗人：
 *   ① 同一份清单出现在两处（命令名、规则名、疑问词、虚词/词尾）；
 *   ② 一张表里同一个词写了两遍（改一处忘一处，或者复制粘贴留下的）。
 * 前者这个仓库已经用「表 + 生成」解决过好几轮（CMDS[]、REL_RULES[]、
 * AI_QUESTION_WORDS[]、FR_PARTICLES[]），这一套是**回归看护**：谁再把清单抄一份，
 * 或者往表里粘一行重复的，这里当场变红。
 *
 * 除了查重复，还钉住三张表的**结构关系**（那是设计意图，不是实现细节）：
 *   · FR_PARTICLES ⊆ fr_is_func 的判据（语气助词也算虚词）；
 *   · FR_PARTICLES 的每个词都被 fr_wellformed 当作「不能收尾」而拒绝；
 *   · 反过来，「经过」这种以虚词「过」收尾的真词**不能被**误伤 ——
 *     这就是「过」留在虚词表、不进词尾表的理由（合并两份表会踩这个坑）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_dupcheck tools/dupcheck.c -lm
 *       ./build_dupcheck
 */
#include "../agent.h"
#include "../token_lang.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

/* 一张以 NULL 结尾的字符串表：查重复项。返回重复条数，并打出来。
 * 表长上限给 512 够用（最大的表是疑问词，23 条）。 */
static void ok_len(const char *const *list, const char *name, size_t n);
static int check_dup(const char *const *list, const char *name, int binary_search_sorted) {
    size_t n = 0;
    for (; list[n] && n < 512; ++n) { }
    int dups = 0;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (strcmp(list[i], list[j]) == 0) {
                if (dups < 4) printf("        %s 里重复：%s\n", name, list[i]);
                dups++;
            }
    /* 第二参数只是提醒调用方：表**不要求**有序（查重不靠它），留个位子给未来 */
    (void)binary_search_sorted;
    if (dups == 0) ok_len(list, name, n);
    else           ok(0, name);
    return dups;
}
static void ok_len(const char *const *list, const char *name, size_t n) {
    (void)list;
    if (n == 0) { printf("  [FAIL] %s 是空表\n", name); fails++; return; }
    printf("  [OK]   %s：%zu 项，无重复\n", name, n);
    passes++;
}

/* 两张表的交集（应当为空） */
static int overlap(const char *const *a, const char *const *b, const char **hit) {
    for (size_t i = 0; a[i]; ++i)
        for (size_t j = 0; b[j]; ++j)
            if (strcmp(a[i], b[j]) == 0) { if (hit) *hit = a[i]; return 1; }
    return 0;
}

/* 关系表：名字/ id 都不许撞（id 撞了就「同义写法共用一个 id」的意思，
 * 那要 name 不同、id 相同 —— 所以只查 name 重复与 id 完全重复的那一行）。 */
static void check_rel_table(void) {
    int dup_name = 0, dup_pair = 0;
    for (size_t i = 0; AI_RELS[i].name; ++i)
        for (size_t j = i + 1; AI_RELS[i].name; ++j) {
            if (!AI_RELS[j].name) break;
            if (strcmp(AI_RELS[i].name, AI_RELS[j].name) == 0) dup_name++;
            if (AI_RELS[i].name == AI_RELS[j].name) dup_pair++;
        }
    ok(dup_name == 0, "AI_RELS 的关系词名不重复");
    ok(dup_pair == 0, "AI_RELS 里没有同一行被写两遍");
}

static void check_rule_table(void) {
    int dup = 0;
    for (size_t i = 0; REL_RULES[i].name; ++i)
        for (size_t j = i + 1; REL_RULES[i].name; ++j) {
            if (!REL_RULES[j].name) break;
            if (REL_RULES[i].id == REL_RULES[j].id) dup++;
        }
    ok(dup == 0, "REL_RULES 的规则 id 不重复");
}

int main(void) {
    printf("== ① 清单里没有重复项 ==\n");
    check_dup((const char *const *)FR_FUNC,      "FR_FUNC（虚词表）", 0);
    check_dup((const char *const *)FR_PARTICLES, "FR_PARTICLES（语气/结构助词）", 0);
    check_dup(AI_QUESTION_WORDS,                 "AI_QUESTION_WORDS（疑问词）", 0);
    check_dup(AI_REFUSALS,                       "AI_REFUSALS（拒答说法池）", 0);
    check_rel_table();
    check_rule_table();

    printf("== ② 两张 frame 词表的关系（父集 / 不相交 / 真的在两处生效） ==\n");
    const char *hit = NULL;
    ok(!overlap((const char *const *)FR_FUNC, (const char *const *)FR_PARTICLES, &hit),
       "FR_FUNC 与 FR_PARTICLES 不相交（不是抄出来的第二份）");
    if (hit) printf("        撞了：%s\n", hit);

    /* 每一个助词都要**同时**在两处生效：算虚词 + 不能收尾 */
    int all_func = 1, all_tail = 1;
    for (size_t i = 0; FR_PARTICLES[i]; ++i) {
        if (!fr_is_func(FR_PARTICLES[i])) all_func = 0;
        char buf[32];
        snprintf(buf, sizeof buf, "累%s", FR_PARTICLES[i]);   /* 「累了吗」那种碎片 */
        if (fr_wellformed(buf)) all_tail = 0;
    }
    ok(all_func, "FR_PARTICLES 的每个词都算虚词（fr_is_func）");
    ok(all_tail, "FR_PARTICLES 的每个词收尾都被 fr_wellformed 拒绝");

    printf("== ③ 别把真词误伤（合并两张表的边界） ==\n");
    ok(fr_wellformed("经过") == 1, "「经过」是真词（以虚词「过」收尾，但过不在词尾表里）");
    ok(fr_is_func("过") == 1,     "「过」仍然算虚词（填进槽位必成病句）");
    ok(fr_wellformed("火锅") == 1, "「火锅」是真词");
    ok(fr_wellformed("累了吗") == 0, "「累了吗」是切词碎片（以助词收尾）");
    ok(fr_is_func("") == 1,        "空串当虚词（保守：别填进槽位）");
    ok(fr_is_func("猫") == 0,      "「猫」不是虚词");
    ok(fr_wellformed("猫") == 0,   "单个汉字的「猫」不算成形词（白名单走 Gate A）");

    printf("== ④ 原语表：名字只有一份、每条都有 kind、intern 得进池子 ==\n");
    {
        int dup = 0, dead = 0;
        for (size_t i = 0; PRIMS[i].name; ++i) {
            if (PRIMS[i].kind == PK_NONE) dead++;
            for (size_t j = i + 1; PRIMS[j].name; ++j)
                if (strcmp(PRIMS[i].name, PRIMS[j].name) == 0) dup++;
        }
        ok(dup == 0, "PRIMS 里名字不重复（以前 prim_kind 与 pool_init_prims 各写一份）");
        ok(dead == 0, "PRIMS 里没有 kind 落空的条目");
        ok(prim_count() >= 37, "PRIMS 条数与实现相符（含符号节点行为入口）");
        AI *ai = ai_new();
        pool_init_prims(ai->pool);
        size_t miss = 0;
        for (size_t i = 0; PRIMS[i].name; ++i)
            if (!pool_find(ai->pool, PRIMS[i].name)) miss++;
        for (size_t i = 0; PRIM_SYMS[i]; ++i)
            if (!pool_find(ai->pool, PRIM_SYMS[i])) miss++;
        ok(miss == 0, "pool_init_prims 把表里的名字（含符号）都 intern 了");
        ok(prim_kind(pool_find(ai->pool, PRIM_INSERT)) == PK_INSERT,
           "intern 出来的符号节点认得出是原语（识别与 intern 同一张表）");
        ai_free(ai);
    }

    printf("== ⑤ 遍历函数与两张表一致（fr_func_list 不许漏项） ==\n");
    {
        const char *all[256];
        size_t n = fr_func_list(all, 256);
        size_t nf = 0, np = 0;
        for (size_t i = 0; FR_FUNC[i]; ++i) nf++;
        for (size_t i = 0; FR_PARTICLES[i]; ++i) np++;
        ok(n == nf + np, "fr_func_list 覆盖虚词表 + 助词表");
        int miss = 0;
        for (size_t i = 0; i < n; ++i) if (!fr_is_func(all[i])) miss++;
        ok(miss == 0, "fr_func_list 出来的每一项 fr_is_func 都认");
    }

    printf("\n");
    if (fails == 0) {
        printf("PASS dupcheck（清单自检：%d 项）\n", passes);
        return 0;
    }
    printf("FAIL dupcheck（%d 项失败，%d 项通过）\n", fails, passes);
    return 1;
}
