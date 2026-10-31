/* tools/paperprobe.c — **读论文（离线抽断言）**探针（阶段二 WP5；§6.638） */
#include "../agent.h"
#include "../model_io.h"
#include "../paper.h"

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
    printf("== 读论文（离线，只抽「恰好三段」） ==\n");

    const char *txt =
        "企鹅 属于 鸟类。企鹅 会 游泳。麒麟 属于 神兽。\n"
        "本文的样本很小，只用来演示这条链路。\n"
        "企鹅 属于 鸟类。\n";          /* 重复句 */

    PaperClaim cs[16];
    size_t n = paper_extract(a, txt, cs, 16);
    ok(n == 3, "抽到 3 条（重复句被去重、非三段句不抽）");
    ok(n >= 1 && !strcmp(cs[0].a, "企鹅") && !strcmp(cs[0].r, "属于") && !strcmp(cs[0].b, "鸟类"),
       "第一条 = 企鹅/属于/鸟类（主语没有残字节前缀）");
    ok(n >= 2 && !strcmp(cs[1].r, "会"), "第二条关系词 = 会（关系词在中间）");

    /* 不该抽的：四段、两段、没有关系词 */
    {
        PaperClaim t[8];
        ok(paper_extract(a, "企鹅 会 游泳 非常 厉害。", t, 8) == 0, "**四段** ⇒ 不抽（不猜）");
        ok(paper_extract(a, "企鹅 游泳。", t, 8) == 0, "两段 ⇒ 不抽");
        ok(paper_extract(a, "企鹅 像 鸟类。", t, 8) == 0, "中间不是已知关系词 ⇒ 不抽");
        ok(paper_extract(a, "这个 属于 那个。", t, 8) == 0, "单字主/宾 ⇒ 不抽（太容易误抽）");
    }

    /* 全部进待证，且**不写事实边** */
    size_t made = paper_to_claims(a, cs, n);
    ok(made == 3, "3 条都写成待证");
    {
        Token *A = pool_find(a->pool, "企鹅"), *B = pool_find(a->pool, "鸟类");
        ok(!(A && B && rel_direct(A, REL_IS_A, B)), "抽到≠学到：事实层**没有**企鹅属于鸟类");
        Claim cl[16];
        size_t pn = claim_list(a, cl, 16);
        ok(pn == 3, "待证队列里 3 条");
        int all_paper = 1;
        for (size_t i = 0; i < pn; ++i) if (strcmp(cl[i].src, "论文")) all_paper = 0;
        ok(all_paper, "来源都是「论文」（可信度 50/100）");
    }

    /* 重复读同一篇：不重复造标记 */
    size_t before = a->pool->count;
    paper_to_claims(a, cs, n);
    {
        Claim cl[16];
        ok(claim_list(a, cl, 16) == 3, "同一篇再读一次 ⇒ 待证还是 3 条（不重复造）");
        ok(a->pool->count == before, "词池没膨胀");
    }

    /* 确认一条 ⇒ 才进事实层 */
    {
        Claim one;
        char line[160] = {0};
        ok(claim_at(a, 0, &one) == 1 && claim_accept(a, one.node, line, sizeof line) == 1,
           "::claim accept ⇒ 升格一条");
        Token *A = pool_find(a->pool, one.a), *B = pool_find(a->pool, one.b);
        uint8_t rid = rel_id_of(a, one.r, 0);
        ok(rid && A && B && rel_direct(A, rid, B), "升格之后**事实层**才有这条（其余仍待证）");
        {
            Claim rest[16];
            ok(claim_list(a, rest, 16) == 2, "其余两条仍挂在待证（没有连带升格）");
        }
    }

    /* 存读：待证跟着图走 */
    {
        size_t sz = ai_save_file(a, "_paperprobe.bin");
        AI *b = sz ? ai_load_file("_paperprobe.bin") : NULL;
        ok(b != NULL, "带论文待证的图能存能读");
        if (b) {
            Claim cl[16];
            ok(claim_list(b, cl, 16) == 2, "读回来还有 2 条待证（已确认那条不在里面）");
            ai_free(b);
        }
        remove("_paperprobe.bin");
    }

    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
