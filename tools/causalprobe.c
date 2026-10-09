/* tools/causalprobe.c — 量「因果猜想」的生命周期：一次巧合 / 协变升格 / 反例削弱
 *
 * 这一条对应人的一个原理：**因果要协变、反例要算数**（不是一次时序相邻就成规律）。
 * 它原来有两个洞（见 docs/人思考的原理-采用与不采用.md 第四节）：
 *   ① `ai_learn_causal` 只看时序相邻：一次巧合建一条 `hits=0`、状态=活跃的边，
 *      **出现一百次和出现一次没区别**（post hoc 迷信）；
 *   ② `ai_learn_counterfactual` 调 `ai_weaken_edge`，而后者要求 `n_subs==0`，
 *      因果边当时靠语境字段标出来，而 ai_weaken_edge 只认无语境的边 → **永远匹配不上**：
 *      计数器 `反事实=` 每次 +1，边却一点没变（机制在、行为不在）。
 *
 * 这个探针把三个时刻的状态直接打出来，改这条语义之前先跑它 ——
 * 「一次=暂时 / 协变=升格 / 反例=削弱」三条都要在**边的 hits 与状态**上看得见，
 * 不能只看计数器（计数器会骗人）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_causalprobe tools/causalprobe.c -lm
 *       ./build_causalprobe
 */
#include "../ai.h"

#include <stdio.h>
#include <string.h>

static void show(AI *ai, const char *from, const char *to) {
    Token *a = pool_find(ai->pool, from);
    Token *b = pool_find(ai->pool, to);
    AssocEntry *e = a && b ? ai_causal_edge(a, b, ai->causal) : NULL;
    if (!e) { printf("  %s -> %s : （还没有这条猜想）\n", from, to); return; }
    printf("  %s -> %s : hits=%u mem=%u（%s） 标记=%s walkable=%d\n",
           from, to, (unsigned)e->hits, (unsigned)e->mem_state,
           e->mem_state == MEM_ACTIVE ? "升格/可信" :
           e->mem_state == MEM_FADING ? "暂时" :
           e->mem_state == MEM_DORMANT ? "雪藏" : "遗忘",
           edge_is_causal_guess(e) ? AI_CAUSAL_MARK : "-",
           ai_edge_walkable(ai, e, 1.0f));
}

int main(void) {
    AI *ai = ai_new();
    Token *cat = pool_token_intern(ai->pool, "猫");
    (void)pool_token_intern(ai->pool, "鱼");
    char *w[1] = {(char *)"鱼"};
    Token *lo[1] = {cat};
    ai_set_last_output(ai, lo, 1);

    printf("① 「我说 猫 → 对方说 鱼」一次：\n");
    ai_learn_causal(ai, w, 1);
    show(ai, "猫", "鱼");

    printf("② 再协变 3 次（共 4 次）：\n");
    for (int k = 0; k < 3; ++k) ai_learn_causal(ai, w, 1);
    show(ai, "猫", "鱼");

    printf("③ 反复出现 60 次（有上限，刷不成事实）：\n");
    for (int k = 0; k < 60; ++k) ai_learn_causal(ai, w, 1);
    show(ai, "猫", "鱼");

    printf("④ 反事实：「我本来要说 猫，没说，鱼照样出现」：\n");
    for (int k = 0; k < 9; ++k) {
        ai_push_skipped(ai, cat);
        ai_learn_counterfactual(ai, w, 1);
    }
    show(ai, "猫", "鱼");
    printf("   反事实计数=%zu（只在**真的削弱**时才涨）\n", ai->n_counterfactual);

    printf("⑤ 猜想身份：见得多也**不是知识** ——\n");
    {
        Token *fish = pool_find(ai->pool, "鱼");
        AssocEntry *e = ai_causal_edge(cat, fish, ai->causal);
        printf("   rel_direct(猫, 属于, 鱼)=%d  rel_direct(猫, 有, 鱼)=%d  走边可行=%d\n",
               rel_direct(cat, REL_IS_A, fish), rel_direct(cat, REL_HAS, fish),
               e ? ai_edge_walkable(ai, e, 1.0f) : 0);
    }
    printf("   因果=%zu(升格%zu)\n", ai->n_causal_seen, ai->n_causal_promoted);
    ai_free(ai);
    return 0;
}
