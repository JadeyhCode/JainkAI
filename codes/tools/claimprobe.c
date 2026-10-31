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

    /* ⑤ 第二个来源 ⇒ **交叉验证**：按事实记下（§6.637 的新行为） */
    agent_claim_from_text(a, "论文 猫 属于 鸟类", out, sizeof out);
    ok(strstr(out, "两 个") != NULL || strstr(out, "两个") != NULL, "第二个独立来源 ⇒ 答复「两个来源」");
    {
        Token *A = pool_find(a->pool, "猫"), *B = pool_find(a->pool, "鸟类");
        ok(A && B && rel_direct(A, REL_IS_A, B), "两个来源之后**按事实记下**");
        ok(claim_list(a, cl, 8) == 0, "两条待证都清掉（变事实了）");
    }

    /* ⑥ 单一来源那条：确认才升格（换一件事，别和上面混） */
    ok(agent_claim_from_text(a, "听说 狗 属于 鸟类", out, sizeof out) == 1, "再记一条单一来源的待证");
    Claim one;
    ok(claim_at(a, 0, &one) == 1, "claim_at(0) 取得到（狗 那条）");
    char line[160] = {0};
    int acc = one.node ? claim_accept(a, one.node, line, sizeof line) : 0;
    ok(acc == 1, "claim accept ⇒ 升格成功");
    {
        Token *A = pool_find(a->pool, "狗"), *B = pool_find(a->pool, "鸟类");
        ok(A && B && rel_direct(A, REL_IS_A, B), "确认之后**事实边**才出现");
        char mark[96];
        snprintf(mark, sizeof mark, "%s%s", MICRO_TEACH_TOKEN_PREFIX, "狗_属于_鸟类]");
        ok(pool_find(a->pool, mark) != NULL, "同时落了**教学留痕**（是用户确认的）");
    }
    ok(claim_list(a, cl, 8) == 0, "确认的那条从待证里消失");

    /* ⑦ 重复确认 ⇒ 拒绝（标记已经没了） */
    ok(claim_accept(a, one.node, line, sizeof line) == 0, "同一条再确认一次 ⇒ 0（不重复写）");

    /* ⑧ 丢掉一条 */
    agent_claim_from_text(a, "据说 羊 属于 鸟类", out, sizeof out);
    Claim two;
    ok(claim_at(a, 0, &two) == 1 && claim_reject(a, two.node) == 1, "reject ⇒ 丢掉一条");
    ok(claim_list(a, cl, 8) == 0, "丢掉之后待证空了");

    /* ⑨ 存读：待证跟着图走 */
    agent_claim_from_text(a, "据说 马 属于 鸟类", out, sizeof out);
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


    /* ⑩ §6.637 来源分档 + 交叉验证 */
    ok(claim_source_tier(a, "听说") == 30 && claim_source_tier(a, "论文") == 50 &&
       claim_source_tier(a, "书上") == 60, "来源可信度读表：听说 30 / 论文 50 / 书上 60");
    ok(claim_source_tier(a, "某未知来源") == 40, "表里没有的来源 ⇒ 40（来历不明，不猜高）");
    {
        AI *d = ai_new();
        aip_patterns_load(d);
        relations_load(d, "relations.txt");
        char o1[256] = {0}, o2[256] = {0};
        ok(agent_claim_from_text(d, "听说 鱼 属于 鸟类", o1, sizeof o1) == 1, "第一个来源 ⇒ 记成待证");
        {
            Token *A = pool_find(d->pool, "鱼"), *B = pool_find(d->pool, "鸟类");
            ok(!(A && B && rel_direct(A, REL_IS_A, B)), "一个来源时**还不是事实**");
        }
        ok(agent_claim_from_text(d, "论文 鱼 属于 鸟类", o2, sizeof o2) == 1, "第二个来源 ⇒ 处理");
        ok(strstr(o2, "两 个") != NULL || strstr(o2, "两个") != NULL, "答复说「两个独立来源」");
        {
            Token *A = pool_find(d->pool, "鱼"), *B = pool_find(d->pool, "鸟类");
            ok(A && B && rel_direct(A, REL_IS_A, B), "两个来源之后**按事实记下**（边写进去了）");
        }
        {
            Claim dc[8];
            ok(claim_list(d, dc, 8) == 0, "两条待证都清掉了（已经变事实，不再挂待证）");
        }
        ai_free(d);
    }
    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
