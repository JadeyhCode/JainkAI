/* tools/claimprobe.c — **待证**探针（阶段二 WP4 第一刀；§6.636） */
#include "../agent.h"
#include "../model_io.h"
#include "../claim.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

int main(void) {
    AI *a = ai_new();
    aip_patterns_load(a);
    relations_load(a, "relations.txt");
    printf("== 待证（不轻信） ==\n");

    /* ① 来源前缀识别 */
    ok(agent_hedge_prefix("听说 猫 属于 鸟类", (char[24]){0}, 24, NULL) == 1, "「听说」认出来源前缀");
    ok(agent_hedge_prefix("论文说 猫 属于 鸟类", (char[24]){0}, 24, NULL) == 1, "「论文」也认");
    ok(agent_hedge_prefix("猫 属于 鸟类", (char[24]){0}, 24, NULL) == 0, "不带前缀的陈述**不算**待证");

    /* ② 记一条待证：不进事实层 */
    char out[256] = {0};
    ok(agent_claim_from_text(a, "听说 猫 属于 鸟类", out, sizeof out) == 1,
       "`听说 猫 属于 鸟类` ⇒ 记成待证并答复");
    ok(strstr(out, "待 证") != NULL || strstr(out, "待证") != NULL, "答复里明说是**待证**");
    {
        Token *A = pool_find(a->pool, "猫"), *B = pool_find(a->pool, "鸟类");
        /* 注意：待证阶段**连词都还没 intern**（只有标记节点），所以这里判"没有那条边"而不是"词在" */
        ok(!(A && B && rel_direct(A, REL_IS_A, B)), "待证**没有**写成事实边（猫 不要 属于 鸟类）");
    }

    /* ③ 能列出来，字段解析对 */
    Claim cl[8];
    size_t n = claim_list(a, cl, 8);
    ok(n == 1, "claim_list 数到 1 条");
    ok(n == 1 && !strcmp(cl[0].src, "听说") && !strcmp(cl[0].a, "猫") &&
                !strcmp(cl[0].r, "属于") && !strcmp(cl[0].b, "鸟类"),
       "标记名能解析回 来源/主语/关系/宾语");

    /* ④ 重复的同一来源 ⇒ 不重复造 */
    size_t before = a->pool->count;
    agent_claim_from_text(a, "听说 猫 属于 鸟类", out, sizeof out);
    ok(claim_list(a, cl, 8) == 1, "同一条再来一次 ⇒ 还是 1 条（不重复造标记）");
    ok(a->pool->count == before, "词池没有多出新词（内部标记复用）");

    /* ⑤ 第二个来源 ⇒ **另一条**（仍然都不算数） */
    agent_claim_from_text(a, "论文 猫 属于 鸟类", out, sizeof out);
    ok(claim_list(a, cl, 8) == 2, "另一个来源 ⇒ 单独一条（不自动合并/不自动升格）");

    /* ⑥ 确认第一条 ⇒ 这时才是事实，并且留教学痕 */
    Claim one;
    ok(claim_at(a, 0, &one) == 1, "claim_at(0) 取得到");
    char line[160] = {0};
    int acc = one.node ? claim_accept(a, one.node, line, sizeof line) : 0;
    ok(acc == 1, "claim accept ⇒ 升格成功");
    {
        Token *A = pool_find(a->pool, "猫"), *B = pool_find(a->pool, "鸟类");
        ok(A && B && rel_direct(A, REL_IS_A, B), "确认之后**事实边**才出现（对 ， 猫 属于 鸟类）");
        char mark[96];
        snprintf(mark, sizeof mark, "%s%s", MICRO_TEACH_TOKEN_PREFIX, "猫_属于_鸟类]");
        ok(pool_find(a->pool, mark) != NULL, "同时落了**教学留痕**（是用户确认的）");
    }
    ok(claim_list(a, cl, 8) == 1, "确认的那条从待证里消失（只剩论文那条）");

    /* ⑦ 重复确认 ⇒ 拒绝（标记已经没了） */
    ok(claim_accept(a, one.node, line, sizeof line) == 0, "同一条再确认一次 ⇒ 0（不重复写）");

    /* ⑧ 丢掉剩下的那条 */
    Claim two;
    ok(claim_at(a, 0, &two) == 1 && claim_reject(a, two.node) == 1, "reject ⇒ 丢掉一条");
    ok(claim_list(a, cl, 8) == 0, "丢掉之后待证空了");

    /* ⑨ 存读：待证跟着图走 */
    agent_claim_from_text(a, "据说 狗 属于 鸟类", out, sizeof out);
    size_t sz = ai_save_file(a, "_claimprobe.bin");
    AI *b = sz ? ai_load_file("_claimprobe.bin") : NULL;
    ok(b != NULL, "有待证的图能存能读");
    if (b) {
        Claim bc[8];
        size_t bn = claim_list(b, bc, 8);
        ok(bn == 1 && !strcmp(bc[0].src, "据说"), "读回来待证还在（来源也对）");
        /* 读回来的图上确认一次 */
        char l2[160] = {0};
        ok(bn == 1 && claim_accept(b, bc[0].node, l2, sizeof l2) == 1, "读回来之后还能确认升格");
        ai_free(b);
    }
    remove("_claimprobe.bin");

    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
