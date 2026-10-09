/* tools/biastest.c — **反糟粕审计**（C 级）：人的思考原理要学，人的偏误不许学
 *
 * 这个文件是 `docs/人思考的原理-采用与不采用.md` 里那张「不采用（糟粕）表」的
 * 可执行版本：每一条糟粕都必须有一条**能跑红**的护栏，而不是文档里的一句好话。
 *
 *   ① 反迷信：一次时序相邻 ≠ 因果规律（协变才升格），而且重复有上限
 *   ② 反事实真的能削弱因果猜想（这一条原来**是空的** —— 见下）
 *   ③ 反虚构：推不出来就说推不出来，不下断言、不硬凑
 *   ④ 反自我印证：统计只看观测，缓存（自己推的）不算数据
 *   ⑤ 反空想造事实：想象/内心排练只动注意力，不改图的**事实**边
 *   ⑥ 反从众：别人说的进他的信念表，不进我的知识
 *   ⑦ 反过度泛化：样本不够就保留声明值，不拿一两个例子改结论
 *   ⑧ 反「挨着出现就算一类」：D1 固化必须有图内证据（邻居重叠）
 *
 * 踩过的坑（写在这儿，免得下一个改这段代码的人再踩）：
 *   `ai_learn_counterfactual` 原来调 `ai_weaken_edge`，而后者要求 `n_subs == 0`；
 *   因果边当时靠语境字段标出来，而 ai_weaken_edge 只认无语境的边 → **永远匹配不上**。
 *   症状极隐蔽：计数器 `反事实=` 每次都 +1（看起来机制在跑），
 *   边上的 hits/状态却一直没变 —— **声明了机制，行为不在**。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_biastest tools/biastest.c -lm
 *       ./build_biastest
 */
#include "../ai.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static Token *T(AI *ai, const char *name) { return pool_token_intern(ai->pool, name); }
static void prep(AI *ai, char *const *w, size_t n) {
    for (size_t i = 0; i < n; ++i) (void)pool_token_intern(ai->pool, w[i]);
}

/* 图里有多少条**被断言的关系边**（rel 或 nrel 位非 0）—— 「事实」的规模 */
static size_t count_rel_edges(AI *ai) {
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        for (size_t j = 0; j < t->count; ++j)
            if (t->assocs[j].rel || t->assocs[j].nrel) n++;
    }
    return n;
}
/* 图里所有边（含共现/语境边）的条数 —— 「空想会不会偷偷加一条边」看这个 */
static size_t count_all_edges(AI *ai) {
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        n += t->count;
    }
    return n;
}

/* ============================ ① 反迷信：一次巧合不是规律 ============================ */
static void test_superstition(void) {
    printf("== ① 反迷信：一次时序相邻不生成「规律」，协变才升格 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *fish = T(ai, "鱼");
    (void)fish;
    char *w[1] = {(char *)"鱼"};
    Token *lo[1] = {cat};
    ai_set_last_output(ai, lo, 1);

    ai_learn_causal(ai, w, 1);
    AssocEntry *e = ai_causal_edge(cat, fish, ai->causal);
    ok(e != NULL, "第一次「我说猫 → 对方说鱼」建了一条**猜想**边");
    ok(e && e->hits == 1, "一次巧合：hits=1（而不是被当成已证实的连结）");
    ok(e && e->mem_state == MEM_FADING, "一次巧合：状态=淡化（会淡、可被反例雪藏）");
    ok(e && edge_is_causal_guess(e), "动作槽带 [因果] 标记（所以知识查询认不出它）");
    ok(e && ai_edge_walkable(ai, e, 1.0f) == 0, "不参与走边（猜想的身份不会被生成器用掉）");
    ok(rel_direct(cat, REL_IS_A, fish) == 0 && rel_direct(cat, REL_HAS, fish) == 0,
       "它**不是**知识：任何核心关系上都没有它");

    /* 协变：再见两次（共 3 次）→ 升格 */
    ai_learn_causal(ai, w, 1);
    ai_learn_causal(ai, w, 1);
    ok(e && e->hits >= AI_CAUSAL_MIN_HITS, "反复协变：hits 涨到门槛");
    ok(e && e->mem_state == MEM_ACTIVE, "见够次数→升格成「可信的猜想」");
    ok(ai->n_causal_promoted >= 1, "升格计数在走（::stats 里的 因果=(升格N)）");

    /* 上限：见得多不代表能刷成事实 */
    for (int k = 0; k < 50; ++k) ai_learn_causal(ai, w, 1);
    ok(e && e->hits == AI_CAUSAL_HITS_CAP, "重复有上限（刷不到「事实」那一档）");
    ok(e && edge_is_causal_guess(e) && ai_edge_walkable(ai, e, 1.0f) == 0,
       "升格之后仍然是猜想：带标记、不走边、不进关系表");
    ai_free(ai);
}

/* ============================ ② 反事实真的削弱 ============================ */
static void test_counterfactual(void) {
    printf("== ② 反事实：「我没做 X，Y 照样发生」→ 真的削弱那条猜想 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *fish = T(ai, "鱼");
    char *w[1] = {(char *)"鱼"};
    Token *lo[1] = {cat};
    ai_set_last_output(ai, lo, 1);
    ai_learn_causal(ai, w, 1);
    ai_learn_causal(ai, w, 1);          /* hits = 2 */

    AssocEntry *e = ai_causal_edge(cat, fish, ai->causal);
    unsigned before = e ? e->hits : 0;
    ai_push_skipped(ai, cat);           /* 我本来要说「猫」，没说 */
    ai_learn_counterfactual(ai, w, 1);
    ok(e && e->hits == before - 1, "反例让 hits 真的减了（原来这里**一点没变**）");
    ok(ai->n_counterfactual == 1, "计数只在**真的削弱**时 +1（不再谎报）");

    /* 掉到 0 → 退回「暂时」；在「暂时」上再来一个反例 → 雪藏 */
    ai_push_skipped(ai, cat);
    ai_learn_counterfactual(ai, w, 1);
    ok(e && e->hits == 0 && e->mem_state == MEM_FADING, "hits 掉到 0 → 退回「暂时」");
    ai_push_skipped(ai, cat);
    ai_learn_counterfactual(ai, w, 1);
    ok(e && e->mem_state == MEM_DORMANT, "在「暂时」上再来一个反例 → 雪藏（不信了）");
    size_t cf = ai->n_counterfactual;
    ai_push_skipped(ai, cat);
    ai_learn_counterfactual(ai, w, 1);
    ok(ai->n_counterfactual == cf, "已经雪藏的再遇到反例不再计数（重复削弱没有意义）");

    /* 不误伤：反事实只动带 [因果] 标记的边，普通共现边不许被牵连 */
    Token *dog = T(ai, "狗"), *bone = T(ai, "骨头");
    (void)token_observe_pair(dog, bone);
    unsigned dh = 0;
    for (size_t i = 0; i < dog->count; ++i)
        if (dog->assocs[i].main == bone) dh = dog->assocs[i].hits;
    ai_push_skipped(ai, dog);
    char *w2[1] = {(char *)"骨头"};
    size_t cf2 = ai->n_counterfactual;
    ai_learn_counterfactual(ai, w2, 1);
    unsigned dh2 = 0;
    for (size_t i = 0; i < dog->count; ++i)
        if (dog->assocs[i].main == bone) dh2 = dog->assocs[i].hits;
    ok(dh2 == dh, "普通共现边不受反事实影响（它没有 [因果] 标记）");
    ok(ai->n_counterfactual == cf2, "削弱不到的时候不计数");
    ai_free(ai);
}

/* ============================ ③ 反虚构 ============================ */
static void test_confabulation(void) {
    printf("== ③ 反虚构：推不出来就说推不出来，不硬凑一个答案 ==\n");
    AI *ai = ai_new();
    (void)T(ai, "猫"); (void)T(ai, "会"); (void)T(ai, "跳舞");
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w, 4);
    Think *t = think_of(ai);
    think_run(ai, w, 4);
    ok(t->concl == 0, "没有依据 → 不给结论（把握归零）");
    printf("        （停因 = %s）\n", think_stop_name(t->stop));
    ok(t->stop != TH_STOP_CONCLUDED && t->stop != TH_STOP_GUESSED,
       "停因是诚实的那几种（推不出来/原地打转/超预算），不是「有结论」");
    ok(count_rel_edges(ai) == 0, "没有因此凭空长出一条关系边");
    ai_free(ai);
}

/* ============================ ④ 反自我印证 ============================ */
static void test_self_confirmation(void) {
    printf("== ④ 反自我印证：统计只看观测，缓存（自己推的）不算数据 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *animal = T(ai, "动物"), *bird = T(ai, "鸟");
    /* 两条都是缓存（推出来沉淀的）：猫 属于 动物 / 猫 属于 鸟 → 看起来「属于」多值 */
    (void)token_tag_rel(cat, animal, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(cat, bird,   REL_IS_A, REL_BOOST);
    for (size_t i = 0; i < cat->count; ++i) cat->assocs[i].role = ASSOC_ROLE_DERIVED;
    rel_excl_build(ai);
    ok(ai->rel_excl_single[REL_IS_A] == 0 && ai->rel_excl_multi[REL_IS_A] == 0,
       "缓存**不**计入独占统计（否则自己推的结论会强化推出它的规则）");
    ok(ai->rel_excl[REL_IS_A] == 1, "声明值保持不变（没被自己的缓存推翻）");

    /* 观测进来之后才计入（要**两个**观测宾语才叫「多值」） */
    (void)token_tag_rel(cat, T(ai, "植物"), REL_IS_A, REL_BOOST);
    (void)token_tag_rel(cat, T(ai, "食物"), REL_IS_A, REL_BOOST);
    rel_excl_build(ai);
    ok(ai->rel_excl_multi[REL_IS_A] >= 1,
       "换成**观测**边，统计就看得见（缓存不算、观测才算）");
    ai_free(ai);
}

/* ============================ ⑤ 反空想造事实 ============================ */
static void test_no_fabrication_from_rehearsal(void) {
    printf("== ⑤ 反空想造事实：想象/内心排练只动注意力，不加事实边 ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "猫"), *b = T(ai, "狗");
    (void)token_observe_pair(a, b);
    ai->focus_ring[0] = a; ai->focus_ring[1] = b; ai->focus_n = 2;

    size_t rel0 = count_rel_edges(ai), all0 = count_all_edges(ai);
    ai_inner_dialogue(ai);
    ok(count_rel_edges(ai) == rel0 && count_all_edges(ai) == all0,
       "内心排练（反复想）不加任何边 —— 空想不产生知识");

    /* 想象：可能被 rng 挡住，所以「跑很多次」再断言：只许加带 [想象] 标记的合成节点，
     * **不许**产生任何关系事实边。 */
    ai->imagine_rate = 1.0f;
    size_t rel1 = count_rel_edges(ai);
    for (int k = 0; k < 200; ++k) ai_imagine(ai);
    ok(count_rel_edges(ai) == rel1, "想象跑了 200 次也没有长出一条关系事实边");
    ok(ai->n_imaginations == 0 || pool_find(ai->pool, "[想象]") != NULL,
       "想象出来的东西挂的是 [想象] 标记（可辨识，不当观测）");
    ai_free(ai);
}

/* ============================ ⑥ 反从众 ============================ */
static void test_no_conformity(void) {
    printf("== ⑥ 反从众：别人说的进他的信念表，不进我的知识 ==\n");
    AI *ai = ai_new();
    Token *who = T(ai, "小明"), *cat = T(ai, "猫"), *fly = T(ai, "飞");
    OtherModel *om = other_intern(ai, who);
    ok(om != NULL, "建一个他者模型");
    ok(other_belief_learn(ai, om, cat, fly, REL_CAN, 0) == 1, "记下「他认为 猫 会 飞」");
    ok(rel_direct(cat, REL_CAN, fly) == 0, "**我**的关系图里没有这条（别人的看法不是我的知识）");
    const HypoFact *f = other_belief_find(om, cat, REL_CAN, fly);
    ok(f && f->neg == 0, "他的信念表里查得到（问他就查他的）");
    ai_free(ai);
}

/* ============================ ⑦ 反过度泛化 ============================ */
static void test_no_overgeneralization(void) {
    printf("== ⑦ 反过度泛化：样本不够就不改结论 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫");
    (void)token_tag_rel(cat, T(ai, "动物"), REL_IS_A, REL_BOOST);
    (void)token_tag_rel(cat, T(ai, "鸟"),   REL_IS_A, REL_BOOST);
    rel_excl_build(ai);
    ok(ai->rel_excl[REL_IS_A] == 1,
       "只有 1 个主语（< REL_EXCL_MIN_SUBJ）→ 保留声明值，不据一例改判定");
    ok(ai->n_rel_excl_evidenced == 0, "「有证据支持的项」是 0（没表态就是没表态）");
    ai_free(ai);
}

/* ============================ ⑧ 反「挨着出现就算一类」 ============================ */
static void test_d1_needs_graph_evidence(void) {
    printf("== ⑧ D1 固化必须有图内证据（邻居重叠），不是「挨着出现过」 ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "甲"), *b = T(ai, "乙");
    /* 两边各 6 个**不相交**的邻居：轮子里挨着出现，但图上没有任何「一类」的证据 */
    for (int k = 0; k < 6; ++k) {
        char na[16], nb[16];
        snprintf(na, sizeof na, "甲邻%d", k);
        snprintf(nb, sizeof nb, "乙邻%d", k);
        (void)token_observe_pair(a, T(ai, na));
        (void)token_observe_pair(b, T(ai, nb));
    }
    Micro *mk = micro_of(ai);
    mod_queue_clear(&mk->queue);
    char *w[2] = {(char *)"甲", (char *)"乙"};
    micro_detect_coactivation(ai, w, 2, mk);
    ok(mk->queue.n == 0, "只挨着出现过（邻居不重叠）→ **不提**固化意图（这就是反迷信）");

    /* 补上图内证据：两边共享 6 个邻居 → 才有资格提 */
    for (int k = 0; k < 6; ++k) {
        char ns[16];
        snprintf(ns, sizeof ns, "共%d", k);
        Token *s = T(ai, ns);
        (void)token_observe_pair(a, s);
        (void)token_observe_pair(b, s);
    }
    mod_queue_clear(&mk->queue);
    micro_detect_coactivation(ai, w, 2, mk);
    ok(mk->queue.n >= 1, "邻居高度重叠（同类证据）→ 才提「加一条共现边」");
    ai_free(ai);
}

int main(void) {
    test_superstition();
    test_counterfactual();
    test_confabulation();
    test_self_confirmation();
    test_no_fabrication_from_rehearsal();
    test_no_conformity();
    test_no_overgeneralization();
    test_d1_needs_graph_evidence();
    printf("\n反糟粕审计：%d 项通过，%d 项失败\n", passes, fails);
    return fails ? 1 : 0;
}
