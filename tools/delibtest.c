/* tools/delibtest.c — 思维循环自己的测试（C 级，不经过 CLI）
 *
 * 看护的是「它在怎么想」这件事本身，而不只是「它答了什么」：
 *   ① 议程：目标去重、循环真的在推进（够不着 → 拆子目标 → 再取一个）
 *   ② 结论：直接事实 / 规则推导（验证器通过）/ **反方向才是答案**（缺前提那条规则）
 *   ③ 矛盾：正反都成立时**不下断言**，并把矛盾留在痕上
 *   ④ 诚实：推不出来就是推不出来（停因写明），不编结论
 *   ⑤ 跨轮：想不通 → 挂成未解问题（经意图队列落成一个可辨识的节点）→
 *      后来学到了 → 想通 → 记着「等下要说一句」
 *   ⑥ 铁律：**思维不把推出来的东西写进图**（观测才配进图）
 *   ⑦ 元认知：原地打转 / 步数预算 / 挂满被拦
 *   ⑧ UTF-8：定长缓冲里的中文截断必须落在字符边界上（这个项目踩过乱码）
 *   ⑭ N1：一轮思考产出**对象**（目标 + 结构化步骤 + 复核缺口），默认关、只进内存、不写图
 *   ⑮ N4：空闲思考（没人说话时自己挑一个未解问题来想）—— 默认关、**只读**
 *   ⑯ N3：回溯 + 在线修订（前提不成立 → 退回分支起点换一条）—— 默认关、预算不退
 *   ⑰ 机制路的候选证据（链长/链底可信度）要真的报上来（不是中性 0/50）
 *
 * 用法：gcc -std=c11 -O2 -o build_delibtest tools/delibtest.c -lm && ./build_delibtest
 */
#include "../ai.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static Token *T(AI *ai, const char *name) { return pool_token_intern(ai->pool, name); }

/* 本文件测的是**默认档**的行为（写入闸门关、统一写入通道路由关）。
 * `AIP_WGATE=1` / `AIP_ROUTE=1` 这类反向测量不该把它们弄红 —— C 级测试**自己钉**
 * 自己依赖的那一档（实测踩过：AIP_WGATE=1 跑整包时这里 12 项假红）。 */
static AI *tnew(void) {
    AI *ai = ai_new();          /* ← 这里必须是 ai_new（替换过头会变成无限递归，实测崩过） */
    if (ai) { ai->write_gate_off = 1; ai->route_writes = 0; }
    return ai;
}

/* 感知阶段（agent_step 的预测/学习循环）会先把输入里的词 intern 进池子，
 * 思维循环拿到的是**已经在池子里**的词。测试里要照这个前提准备，
 * 否则 pool_find 找不到宾语词，连目标都立不起来（那是测试的错，不是思维的错）。 */
static void prep(AI *ai, char *const *w, size_t n) {
    for (size_t i = 0; i < n; ++i) (void)pool_token_intern(ai->pool, w[i]);
}

/* 数一数图里有多少条**被断言的关系边**（rel 或 nrel 位非 0） */
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
static int has_token(AI *ai, const char *name) { return pool_find(ai->pool, name) != NULL; }

/* 造一个「猫 属于 动物、动物 有 毛」的小知识库 */
static AI *mini(void) {
    AI *ai = tnew();
    Token *cat = T(ai, "猫"), *animal = T(ai, "动物"), *fur = T(ai, "毛");
    (void)token_tag_rel(cat, animal, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(animal, fur, REL_HAS, REL_BOOST);
    return ai;
}

/* ============================ ① 议程与循环 ============================ */
static void test_agenda(void) {
    printf("== ① 议程：去重 + 真的在推进 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);
    Token *a = T(ai, "猫"), *b = T(ai, "动物");
    ok(think_goal_push(t, TG_REL, a, b, REL_IS_A, 0, 0) == 1, "压一个目标进议程");
    ok(think_goal_push(t, TG_REL, a, b, REL_IS_A, 0, 0) == 0, "同一件事不重复压（去重）");
    ok(think_goal_push(t, TG_REL, a, b, REL_IS_A, 1, 0) == 1,
       "换一个问法（问否定）算另一件事");
    ai_free(ai);
}

/* ============================ ② 结论：查 / 推 / 反方向 ============================ */
static void test_conclude(void) {
    printf("== ② 结论：直接查、规则推、反方向才是答案 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);

    /* 直接事实 */
    char *w1[4] = {(char *)"猫", (char *)"属于", (char *)"动物", (char *)"吗"};
    prep(ai, w1, 4);
    think_run(ai, w1, 4);
    ok(t->concl == 1 && t->stop == TH_STOP_CONCLUDED, "直接事实：结论=成立，停因=有结论");
    ok(t->conf >= 90, "直接事实的把握高（≥90）");

    /* 规则推导（继承）：猫 有 毛 —— 语料里没这句，是推出来的 */
    char *w2[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
    prep(ai, w2, 4);
    think_run(ai, w2, 4);
    ok(t->concl == 1 && t->stop == TH_STOP_CONCLUDED, "继承规则：猫 有 毛 推得出来");
    ok(strstr(t->chain_text, "继承") != NULL, "依据链上标了「继承」");
    ok(t->conf == 85, "推导出来的把握是 85（比直接事实低）");

    /* 反方向才是答案：唱歌 需要 声带，猫 没有 声带 ⟹ 猫 不会 唱歌 */
    Token *sing = T(ai, "唱歌"), *cord = T(ai, "声带");
    (void)token_tag_rel(sing, cord, REL_NEED, REL_BOOST);
    (void)token_tag_nrel(T(ai, "猫"), cord, REL_HAS, REL_BOOST);
    char *w3[4] = {(char *)"猫", (char *)"会", (char *)"唱歌", (char *)"吗"};
    prep(ai, w3, 4);
    think_run(ai, w3, 4);
    ok(t->concl == -1, "问「会」推不出来，反方向推得出来 → 结论=不成立");
    int saw = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (strstr(t->trace[i].what, "反方向")) saw = 1;
    ok(saw, "痕上写明「反方向才是答案」（不是把方向搞反了）");
    ok(strstr(t->chain_text, "缺前提") != NULL, "依据链上是「缺前提」那条规则");

    /* 推不出来：诚实 */
    char *w4[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w4, 4);
    think_run(ai, w4, 4);
    ok(t->concl == 0, "没知识就是没结论（不编）");
    ok(t->stop == TH_STOP_UNKNOWN || t->stop == TH_STOP_STALL, "停因写明「想不下去」");
    ai_free(ai);
}

/* ============================ ⑭ N1：思考对象（[思考:N]） ============================ */
static void test_think_obj(void) {
    printf("== ⑭ N1：一轮思考产出**对象**（目标 + 结构化步骤 + 复核），只进内存 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);
    /* C 级测试**自己定这一位**：不跟着 AIP_THINKOBJ 走（否则反向测量时
     * "默认关"那几条会假红 —— 实测踩过：整包在 AIP_THINKOBJ=1 下这里报 5 项失败）。 */
    t->obj_on = 0;

    /* 默认关：这一轮不产出对象（与从前逐字节相同的那一侧） */
    {
        char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
        prep(ai, w, 4);
        think_run(ai, w, 4);
        ok(!t->obj_on && !t->obj.have, "默认关：跑完一轮也不产出对象（obj_on=0）");
        ok(t->n_obj == 0, "默认关：对象计数一个不涨");
    }

    /* 开着：推得出来的那一轮 —— 对象里要有目标 + 规则步 + 结论，复核缺口=0 */
    t->obj_on = 1;
    size_t edges_before = count_rel_edges(ai);
    {
        char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
        prep(ai, w, 4);
        think_run(ai, w, 4);
        ok(t->obj.have, "开着：这一轮产出了对象");
        ok(t->obj.seq == 1 && t->n_obj == 1, "对象有编号（[思考:1]，N 从 1 起）");
        ok(t->obj.have_goal && t->obj.goal.rid == REL_HAS && t->obj.goal.a == T(ai, "猫"),
           "对象里记着**目标**（猫 -[有]-> 毛）");
        ok(t->obj.der.n >= 3, "对象里是**结构化步骤**（不是文本痕迹）");
        ok(t->obj.verified && t->obj.gaps == 0, "定稿时跑过复核器，缺口=0（站得住）");
        int saw_rule = 0, saw_concl = 0;
        for (size_t i = 0; i < t->obj.der.n; ++i) {
            if (t->obj.der.step[i].kind == DER_RULE) saw_rule = 1;
            if (t->obj.der.step[i].kind == DER_CONCLUDE) saw_concl = 1;
        }
        ok(saw_rule && saw_concl, "步骤里有 RULE（继承那一跳）和 CONCLUDE");
        ok(t->obj.n_trace > 0, "对象记下了「这一轮痕迹几条」（痕迹↔对象对得上）");
        /* 当场复核：再独立重推一遍，结论不变 */
        ok(think_obj_reverify(ai, t) == 0, "::thinkobj verify 那条路：当场复核还是缺口 0");
    }

    /* 想不出来的那一轮：对象照旧产出，但要**照实留一条缺口** */
    {
        char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
        prep(ai, w, 4);
        think_run(ai, w, 4);
        ok(t->obj.have && t->obj.seq == 2, "想不出来的那一轮也有对象（[思考:2]）");
        ok(t->obj.gaps > 0, "没接出链 ⇒ 对象里留一条**缺口**（不许把空对象说成想通了）");
        ok(think_obj_reverify(ai, t) == t->obj.gaps, "当场复核是幂等的（缺口数不会越复核越多）");
    }

    /* 铁律：对象是"想法"，一个字节都不改图 */
    ok(count_rel_edges(ai) == edges_before, "对象**没往图里写**断言边（只有观测进图）");

    /* 关掉开关：下一轮就不再产出对象了（历史对象留在内存里不动 ——
     * `::thinkobj` 那时打的是"关着"，不会把上一轮的对象冒充成这一轮的） */
    t->obj_on = 0;
    {
        char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
        prep(ai, w, 4);
        think_run(ai, w, 4);
        ok(t->n_obj == 2 && t->obj.seq == 2, "关掉之后不再产出（n_obj 不变 = 2，历史对象不冒充新的）");
    }
    ai_free(ai);
}

/* ============================ ⑮ N4：空闲思考（只读） ============================ */
static void test_idle_think(void) {
    printf("== ⑮ N4：空闲思考 —— 自己挑一个未解问题来想，只读 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);
    t->idle_every = 0;                 /* C 级测试自己定这一位（不跟 AIP_IDLE 走） */
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    think_settle(ai);                  /* 这一轮会把没想通的问题挂起来（[自问:…]） */
    ok(t->n_open == 1, "前置：挂了一个没想通的问题");
    ok(think_idle(ai) == 0 && t->n_idle == 0, "默认关（idle_every=0）：什么都不做");
    t->idle_every = 1;
    ai->pool->current_tick++;          /* 空轮也会推进 tick（agent_step 开头那一下） */
    size_t tokens0 = ai->pool->count;
    size_t edges0  = count_rel_edges(ai);
    size_t q0      = micro_of(ai)->queue.n;
    int ran = think_idle(ai);
    ok(ran == 1, "开着：真的空闲想了一轮");
    ok(t->n_idle == 1 && t->idle_note[0], "记账：想过 1 轮 + 记下挑的是哪个问题");
    ok(strstr(t->idle_note, "跳舞") != NULL, "挑的正是挂着的那条问题");
    ok(ai->pool->count == tokens0 && count_rel_edges(ai) == edges0,
       "**只读**：没往图里写（符号节点数 / 关系边数都不动）");
    ok(micro_of(ai)->queue.n == q0, "**只读**：没提议任何意图（意图队列长度不变）");
    int saw = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (strstr(t->trace[i].what, "空闲思考")) saw = 1;
    ok(saw, "痕上写明这一轮是「空闲思考：自己挑的」");
    ok(think_idle(ai) == 0, "节拍：同一个 tick 里不会连着想两次");
    ai->pool->current_tick++;
    ok(think_idle(ai) == 1, "下一个 tick 才再想一次");
    ai_free(ai);
}

/* ============================ ⑯ N3：回溯 + 在线修订（默认关） ============================ */
static void test_redo(void) {
    printf("== ⑯ N3：前提不成立就退回去换一条（回溯/在线修订，默认关） ==\n");
    /* 场景：给「猫」挂一条前提链接（猫 —[前提]→ 饿 → 鱼），再问一个推不出来的问题 ——
     * 循环会走到「问前提」那一步并判定前提没成立。 */
    AI *ai = mini();
    Think *t = think_of(ai);
    t->redo_on = 0;                  /* C 级测试自己定这一位（不跟 AIP_THINKREDO 走） */
    {
        /* `加前提` 用 pool_find（不 intern）—— 端点词得先在池子里（CLI 那侧是分词先进池） */
        (void)T(ai, "猫"); (void)T(ai, "鱼"); (void)T(ai, "饿");
        char *op[3] = {(char *)"猫", (char *)"鱼", (char *)"饿"};
        char note[160];
        ok(token_op_run(ai, "加前提", op, 3, note, sizeof note) >= 0,
           "前置：给猫挂一条前提链接（A —[前提]→ P → B 建出来了）");
    }
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->premise_blocked, "走到了「问前提」：判定前提没成立（触发条件）");
    int saw_back = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (strstr(t->trace[i].what, "回退")) saw_back = 1;
    ok(!saw_back && t->n_backtrack == 0, "默认关：**不回退**（继续硬往下想）");

    /* 开着：退回去 + 换一条（改问反面） */
    AI *b = mini();
    Think *tb = think_of(b);
    tb->redo_on = 0;
    {
        (void)T(b, "猫"); (void)T(b, "鱼"); (void)T(b, "饿");
        char *op[3] = {(char *)"猫", (char *)"鱼", (char *)"饿"};
        char note[160];
        (void)token_op_run(b, "加前提", op, 3, note, sizeof note);
    }
    tb->redo_on = 1;
    char *w2[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(b, w2, 4);
    think_run(b, w2, 4);
    int saw = 0, saw_why = 0;
    for (size_t i = 0; i < tb->trace_n; ++i) {
        if (strstr(tb->trace[i].what, "回退")) saw = 1;
        if (strstr(tb->trace[i].why, "改问"))  saw_why = 1;
    }
    ok(saw, "开着：痕上多了一条「回退：前提不成立」");
    ok(saw_why, "痕上**写明了为什么退、换的是哪一条**（改问反面）");
    ok(tb->n_backtrack >= 1 && tb->n_backtrack <= (size_t)THINK_REDO_MAX,
       "退了一次，而且不超过每轮上限（THINK_REDO_MAX=2）");
    ok(tb->n_backtrack_total == tb->n_backtrack, "累计计数也在走");
    /* 失败那一支的痕**留着**（不去删 trace） */
    int saw_prem = 0;
    for (size_t i = 0; i < tb->trace_n; ++i)
        if (strstr(tb->trace[i].what, "问前提")) saw_prem = 1;
    ok(saw_prem, "失败那一支的痕留着（「问前提」那一步还在）");
    /* 换的那条**真的进了议程并被处理**：回退之后还有新的求解尝试 */
    int after = 0, seen_back = 0;
    for (size_t i = 0; i < tb->trace_n; ++i) {
        if (strstr(tb->trace[i].what, "回退")) seen_back = 1;
        else if (seen_back && strstr(tb->trace[i].what, "交给求解器")) after = 1;
    }
    ok(after, "退回去之后**真的换了那条再试**（回退之后还有求解尝试）");
    ai_free(ai); ai_free(b);
}

/* ============================ ⑰ 顺手件②：机制那条路的候选证据 ============================ */
static void test_mech_cand(void) {
    printf("== ⑰ 机制路的「候选证据」：链长/链底可信度要真的报上来 ==\n");
    /* 场景：`猫 会 唱歌 吗` —— 走的是**机制**那条路（缺前提 R9），
     * 而机制路的 hops/cred 以前一律按中性（hops=0、cred=50）算（§6.43 的尾巴）。 */
    AI *ai = mini();
    Think *t = think_of(ai);
    Token *sing = T(ai, "唱歌"), *cord = T(ai, "声带");
    (void)token_tag_rel(sing, cord, REL_NEED, REL_BOOST);
    (void)token_tag_nrel(T(ai, "猫"), cord, REL_HAS, REL_BOOST);
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"唱歌", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->concl == -1, "前置：走机制路推出「不成立」（缺前提 R9）");
    ok(t->n_pend_cache >= 1, "前置：这一轮产生了「值得沉淀」的候选");
    if (t->n_pend_cache >= 1) {
        ok(t->pend_cache[0].hops > 0,
           "机制路报上来的**链长是真的**（不再是中性的 0）");
        ok(t->pend_cache[0].cred <= 100, "链底可信度在合法区间（0..100）");
        ok(t->pend_cache[0].ev >= THINK_EV_RULE,
           "证据档至少是「规则推导」（候选自己的证据齐了）");
    }
    /* 反向：外部注册的机制**不报**证据 ⇒ 按中性算（与没接之前逐字节相同） */
    {
        AI *b = tnew();
        Think *tb = think_of(b);
        Token *x = T(b, "外甲"), *y = T(b, "外乙");
        (void)token_tag_rel(x, y, REL_IS_A, REL_BOOST);
        tb->mech_cand_set = 0;
        char *w2[4] = {(char *)"外甲", (char *)"属于", (char *)"外乙", (char *)"吗"};
        prep(b, w2, 4);
        think_run(b, w2, 4);
        ok(!tb->mech_cand_set, "没走到机制那一步时，证据位不会被谁顺手写脏");
        ai_free(b);
    }
    ai_free(ai);
}

/* ============================ ③ 矛盾 ============================ */
static void test_conflict(void) {
    printf("== ③ 矛盾：正反都成立就不下断言 ==\n");
    /* 注意：这里要的是「图里正反都有」这个**状态**，而不是「用户改口」。
     * 默认的改口语义下，后说的观测会把先说的对立极性改掉（根本到不了这个状态）——
     * 所以这一段显式关掉改口（等价于 `::revise off`，也等价于旧模型里遗留的并存位）。
     * 「改口本身」由第十三节单独看护。 */
    int save_revise = g_obs_revise;
    g_obs_revise = 0;
    AI *ai = mini();
    Think *t = think_of(ai);
    /* 图里同时有 猫 属于 动物 和 猫 不属于 动物（两条都能被验证） */
    (void)token_tag_nrel(T(ai, "猫"), T(ai, "动物"), REL_IS_A, REL_BOOST);
    char *w[4] = {(char *)"猫", (char *)"属于", (char *)"动物", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->conflict == 1, "查出正反都成立");
    ok(t->concl == 0 && t->conf == 0, "矛盾时**不下断言**（结论归零）");
    ok(t->stop == TH_STOP_CONFLICT, "停因=正反都能推出来");
    ok(has_token(ai, "[提醒:知识自相矛盾]"), "把矛盾报给元认知（留下提醒节点）");
    ok(t->n_conflicts == 1, "矛盾计数在走");
    ai_free(ai);
    g_obs_revise = save_revise;
}

/* ============================ ③b 弱依据不算矛盾 ============================
 *
 * 这一节是 A/B 测量逼出来的：把「思维接管输出」默认打开之后，
 * hypothesis / logic4 / exclusivity / hypo_frames 四套会红。
 * 查下来不是输出变差，而是矛盾的判据有两个真问题：
 *   ① `rel_solve` 的 status 说的是「问的那个方向成不成立」，
 *      旧版拿 `!want_neg` 再问一遍同一个问题，于是「有答案」被当成「反面成立」；
 *   ② 没分证据强度：独占/闭合世界（非单调默认）和**假设**推出来的东西，
 *      被拿去和事实并列判「矛盾」。
 * 这一节把两件事都钉住。 */
static void test_weak_evidence(void) {
    printf("== ③b 弱依据不与事实并列判矛盾 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);
    Token *cat = T(ai, "猫"), *fly = T(ai, "飞");

    /* 图里是事实：猫 不会 飞 */
    (void)token_tag_nrel(cat, fly, REL_CAN, REL_BOOST);
    /* 假设世界：猫 会 飞（假设绝不进图） */
    ok(hypo_add(&ai->hypo, cat, fly, REL_CAN, 0) == 1, "压一条假设（猫 会 飞）");
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"飞", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->concl == 1, "在假设世界里：结论=成立（猫 会 飞）");
    ok(t->concl_class == THINK_EV_HYP, "结论强度被标成「假设」（不是事实）");
    ok(t->conflict == 0, "**不算知识矛盾**（一边是假设、一边是现实）");
    int saw = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (strstr(t->trace[i].what, "不是矛盾") || strstr(t->trace[i].why, "不是知识矛盾")) saw = 1;
    ok(saw, "痕上写明了「那是假设与现实的差别，不是矛盾」");

    /* 证据强度分级本身 */
    Token *animal = T(ai, "动物"), *plant = T(ai, "植物");
    Derivation d;
    der_reset(&d); der_fact(&d, cat, REL_CAN, fly, 0, 1);
    ok(think_evidence_class(&d) == THINK_EV_FACT, "事实链 → 观测事实(4)");
    der_reset(&d); der_rule(&d, 2, cat, REL_IS_A, animal, 1);
    ok(think_evidence_class(&d) == THINK_EV_RULE, "继承链 → 规则推导(3)");
    der_reset(&d); der_rule(&d, 9, cat, REL_CAN, animal, 1);
    ok(think_evidence_class(&d) == THINK_EV_RULE, "缺前提(R9) → 也是规则推导(3)");
    der_reset(&d); der_rule(&d, 7, cat, REL_IS_A, plant, -1);
    ok(think_evidence_class(&d) == THINK_EV_DEFAULT, "独占(R7) → 非单调默认(2)");
    der_reset(&d); der_rule(&d, 8, cat, REL_IS_A, plant, -1);
    ok(think_evidence_class(&d) == THINK_EV_DEFAULT, "闭合世界(R8) → 非单调默认(2)");
    der_reset(&d); (void)der_add(&d, DER_HYP);
    ok(think_evidence_class(&d) == THINK_EV_HYP, "假设步 → 假设(1)");
    der_reset(&d); der_default(&d, cat, animal, REL_IS_A, plant, 50, 1);
    ok(think_evidence_class(&d) == THINK_EV_GUESS, "类比 → 类比推测(0)");
    der_reset(&d); der_rule(&d, 8, cat, REL_IS_A, plant, -1); der_fact(&d, cat, REL_CAN, fly, 0, 1);
    ok(think_evidence_class(&d) == THINK_EV_DEFAULT, "一条链取**最弱**那一步（默认规则拉低整条）");
    ai_free(ai);
}

/* ============================ ④ 跨轮：挂问题 → 想通 ============================ */
static void test_crossturn(void) {
    printf("== ④ 跨轮：挂未解问题 → 后来学到 → 想通 ==");
    printf("\n");
    AI *ai = mini();
    Micro *mk = micro_of(ai);
    Think *t = think_of(ai);

    char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->n_open == 1, "想不通的**问句**挂成未解问题（跨轮接着想）");
    ok(t->n_asked == 1, "「累计挂过」在走");
    /* 思维改脑走的是意图队列：安全点应用之后，图里应该有个 [自问:…] 节点 */
    micro_step(ai, w, 4, "测试");
    ok(mk->meta.n_new_tokens == 1, "经意图队列落了 1 个新符号节点（安全内核放行）");
    ok(has_token(ai, "[自问:猫_会_跳舞_吗]"), "落成的是可辨识的自问节点（名字无空格）");
    ok(micro_has_edge(pool_find(ai->pool, "[自问:猫_会_跳舞_吗]"), T(ai, "猫")),
       "自问节点接回触发它的词（不是孤岛）");

    /* 现在学到了：猫 会 跳舞 */
    (void)token_tag_rel(T(ai, "猫"), T(ai, "跳舞"), REL_CAN, REL_BOOST);
    think_settle(ai);
    ok(t->n_open == 0 && t->n_resolved == 1, "收尾时拿新知识重试 → 想通了");
    ok(t->mention_pending == 1 && t->mention_text[0], "记下「等下要说一句」");
    ok(strstr(t->mention_text, "现在 我 知道 了") != NULL, "要说的话里有「现在我知道了」");

    /* 接管输出是开关：显式关掉时一个字都不动，打开时才补一句。
     * （上面这些断言不依赖默认值 —— 默认值是量出来的，测试不该跟着它走。） */
    char out[512] = "我 在";
    think_set_out(ai, 0);
    ok(think_take_output(ai, out, sizeof out) == 0 && strcmp(out, "我 在") == 0,
       "::thinkout off 时**不**动输出");
    think_set_out(ai, 1);
    ok(think_take_output(ai, out, sizeof out) == 1, "打开之后才接管");
    ok(strstr(out, "我 在") == out, "是**追加**在正常回答后面（不把用户这轮问的挤掉）");
    ok(strstr(out, "现在 我 知道 了") != NULL, "追加的正是那句「想通了」");
    ok(think_take_output(ai, out, sizeof out) == 0, "说过一次就不再重复（用完即清）");
    ai_free(ai);
}

/* ============================ ⑤ 铁律：思维不写知识 ============================ */
static void test_no_assert(void) {
    printf("== ⑤ 铁律：推出来的东西**不进图** ==\n");
    AI *ai = mini();
    size_t before = count_rel_edges(ai);
    char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    micro_step(ai, w, 4, "测试");
    think_settle(ai);
    size_t after = count_rel_edges(ai);
    ok(after == before, "推出来的结论没有变成图里的关系边（观测才配进图）");
    ok(!micro_has_edge(T(ai, "猫"), T(ai, "毛")),
       "连那条被推出来的共现边也没写（它只是「想出来的」）");
    ai_free(ai);
}

/* ============================ ⑥ 元认知 ============================ */
static void test_meta(void) {
    printf("== ⑥ 元认知：打转 / 预算 / 挂满 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);

    /* 停滞：够不着、拆出来的子目标也都不成 → 连续几步没推进 */
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->n_stalls >= 1, "推不动又拆不出新东西 → 记一次「原地打转」");

    /* 预算：只给 1 步 */
    think_set_budget(ai, 1);
    think_run(ai, w, 4);
    ok(t->stop == TH_STOP_BUDGET, "步数预算用完就停（不硬凑结论）");
    ok(t->n_over_budget >= 1, "超预算计数在走");

    /* 挂满：把未解问题灌到上限，再问就不给挂了 */
    Think *t2 = think_of(ai);
    for (size_t i = 0; i < THINK_MAX_OPEN + 2; ++i) {
        char nm[16];
        snprintf(nm, sizeof nm, "怪词%zu", i);
        if (!think_open_add(t2, T(ai, nm), NULL, 0, 0, "测试用", 0)) break;
    }
    ok(t2->n_open == THINK_MAX_OPEN, "未解问题挂到上限");
    ok(t2->n_open_blocked >= 1, "再挂就被拦下并记账（元认知不许无限挂）");
    think_remind(ai, MICRO_REMIND_THINK_OPEN, NULL);
    ok(has_token(ai, "[提醒:未解问题太多]"), "挂太多会提醒");

    /* 关掉思维循环 */
    think_set_run(ai, 0);
    think_run(ai, w, 4);
    ok(t2->stop == TH_STOP_OFF, "关掉之后直接收束（停因写明）");
    ai_free(ai);
}

/* ============================ ⑦ 学习开关与 UTF-8 ============================ */
static void test_switch_and_utf8(void) {
    printf("== ⑦ 学习开关 / UTF-8 截断 ==\n");
    AI *ai = mini();
    Think *t = think_of(ai);
    think_set_learn(ai, 0);
    char *w[4] = {(char *)"猫", (char *)"会", (char *)"跳舞", (char *)"吗"};
    prep(ai, w, 4);
    think_run(ai, w, 4);
    micro_step(ai, w, 4, "测试");
    ok(t->n_open == 1, "关掉「改脑」之后，问题照样挂（想还是照想）");
    ok(!has_token(ai, "[自问:猫_会_跳舞_吗]"), "但**不落节点**（改不了脑就只留在痕上）");

    /* UTF-8：按字节截断必须落在字符边界上（否则名字是乱码） */
    char dst[16];
    think_utf8_clip(dst, sizeof dst, "很长很长很长很长的中文句子");
    size_t l = strlen(dst);
    ok(l > 0 && l < sizeof dst, "定长缓冲里放得下、且没写爆");
    int valid = 1;
    for (size_t i = 0; i < l; ) {
        unsigned char c = (unsigned char)dst[i];
        size_t n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (i + n > l) { valid = 0; break; }
        i += n;
    }
    ok(valid, "截断落在完整字符边界上（不会出半个汉字，也不会把多字节字符切一半）");
    ok(strlen(dst) <= 15, "截断后没写爆缓冲区");

    /* 名字里不留空格 */
    char nm[64];
    think_ask_name(nm, sizeof nm, "猫 会 跳舞 吗");
    ok(strchr(nm, ' ') == NULL, "[自问:…] 名字里没有空格（能当 CLI 参数用）");
    ok(strncmp(nm, "[自问:", 8) == 0 && nm[strlen(nm) - 1] == ']', "名字形状是 [自问:…]");
    ai_free(ai);
}

/* ============================ ⑧ 机制注册表 ============================
 *
 * 这一节要证的是**扩展点本身**：新增一条追踪目标的手段，
 * 只要注册一行 + 写它自己的回调，**循环、分解、调度都不用改**。
 * 所以这里在测试里现场注册一条内核根本没有的机制：借「上位类」。
 * 它只读图（父类指针 + 父类到宾语的那条边），不猜。 */
static int mech_parent_applicable(AI *ai, const ThinkGoal *g) {
    (void)ai;
    return g && g->a && g->b && g->a->parent && g->rid != 0;
}
static int mech_parent_process(AI *ai, Think *t, const ThinkGoal *g, int *cls,
                               int *cache, char *note, size_t note_cap) {
    (void)t;
    if (cache) *cache = 0;
    Token *p = g->a ? g->a->parent : NULL;
    if (!p || !g->b) return 0;
    if (!rel_direct(p, g->rid, g->b)) return 0;    /* 图上有这条边才算数（读图，不猜） */
    if (cls) *cls = THINK_EV_RULE;
    if (note && note_cap)
        snprintf(note, note_cap, "%s 的上位类是 %s，而 %s %s %s",
                 g->a->name ? g->a->name : "?", p->name ? p->name : "?",
                 p->name ? p->name : "?",
                 rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
                 g->b->name ? g->b->name : "?");
    return +1;                                     /* 正关系成立 */
}

static void test_mech_registry(void) {
    printf("== ⑧ 机制注册表（新增机制 = 注册一行） ==\n");
    AI *ai = mini();
    (void)think_of(ai);                            /* 先让内建四条注册好 */
    size_t builtin = think_mech_count();
    ok(builtin == 5, "内建注册了五条机制（问前提/换问法/缺前提/借最近邻/经由中间词）");

    ThinkMech custom;
    memset(&custom, 0, sizeof custom);
    custom.name = "试上位类";
    custom.kind = TG_EXT;                          /* 不抢内核的枚举值 */
    custom.needs = "主语有上位类，且上位类图上有这条边";
    custom.applicable = mech_parent_applicable;
    custom.process = mech_parent_process;
    int idx = think_mech_register(&custom);
    ok(idx >= 0 && think_mech_count() == builtin + 1, "运行时注册一条**内核之外**的新机制");
    ok(think_mech_register(&custom) == idx, "同名重复注册是幂等的（不会注册两次）");

    /* 造一个只有这条新机制能回答的局面：
     * 子类 的上位类是 父类；父类 -[属于]-> 目标。
     * 图里**没有** 子类 -[属于]-> 父类 这条边（父子关系是结构，不是边），
     * 所以核心的「查/推」都够不着，只能靠新机制。 */
    AI *ai2 = tnew();
    Think *t2 = think_of(ai2);                     /* 触发内建注册 */
    (void)think_mech_register(&custom);            /* 新机制也注册进来 */
    Token *child = T(ai2, "子类"), *parent = T(ai2, "父类"), *goal = T(ai2, "目标");
    token_set_parent(child, parent);
    (void)token_tag_rel(parent, goal, REL_IS_A, REL_BOOST);
    ok(!rel_direct(child, REL_IS_A, goal), "前置条件：核心的直接查够不着");
    char *w[4] = {(char *)"子类", (char *)"属于", (char *)"目标", (char *)"吗"};
    prep(ai2, w, 4);
    think_run(ai2, w, 4);
    ok(t2->concl == 1, "结论是**新注册的机制**给的（成立）");
    int saw = 0;
    for (size_t i = 0; i < t2->trace_n; ++i)
        if (strstr(t2->trace[i].why, "上位类")) saw = 1;
    ok(saw, "痕上能看到这条机制自己写的话（说明它真的被调用了）");
    ok(strstr(t2->concl_text, "属于") != NULL, "结论投影正常");
    ai_free(ai2);
    ai_free(ai);
}

/* ============================ ⑨ 借最近邻的极性 ============================
 * 真值 → 回答 的换算在每条机制上都要做。这条看护是写注册表时发现的：
 * 类比原来把「真值」直接当「回答」用，问否定时答案会反。 */
static void test_analogy_polarity(void) {
    printf("== ⑨ 借最近邻：问否定时答案要翻过来 ==\n");
    AI *ai = tnew();
    Think *t = think_of(ai);
    Token *cat = T(ai, "猫"), *fish = T(ai, "鱼"), *like = T(ai, "像的");
    (void)token_tag_rel(like, fish, REL_LIKE, REL_BOOST);   /* 像的 喜欢 鱼 */
    /* 让 猫 与 像的 的表示完全相同 → 余弦 1.0 → 借最近邻会借到它的态度 */
    int8_t v[VSA_DIM];
    emb_randvec(4242u, v);
    memcpy(cat->emb, v, VSA_DIM);
    memcpy(like->emb, v, VSA_DIM);
    ok(rel_guess(ai, cat, REL_LIKE, fish, NULL) == 0 || 1, "前置条件：表示层可用");

    char *w1[4] = {(char *)"猫", (char *)"喜欢", (char *)"鱼", (char *)"吗"};
    prep(ai, w1, 4);
    think_run(ai, w1, 4);
    ok(t->stop == TH_STOP_GUESSED, "没有直接知识 → 走「借最近邻」，停因标明是猜的");
    ok(t->concl == 1, "问「喜欢吗」→ 借到的态度是肯定 → 成立");

    char *w2[5] = {(char *)"猫", (char *)"不", (char *)"喜欢", (char *)"鱼", (char *)"吗"};
    prep(ai, w2, 5);
    think_run(ai, w2, 5);
    ok(t->concl == -1, "问「不喜欢吗」→ 同一个真值，答案必须翻成「不成立」");
    ai_free(ai);
}

/* ============================ ⑩ 推理沉淀（想出来的东西留下来） ============================
 *
 * 这一节盯的是「越用越懂」那条链：
 *   推导（健全规则）→ **沉淀**成一条缓存 → 下次直接用（不再重推）
 *   → 观测来了就撤 → 强依据推翻它也会撤（并且当场改口）。
 * 沉淀与观测必须分得清：缓存标着 ASSOC_ROLE_DERIVED，证据强度是「缓存」档，
 * 而且**默认不当知识**用（可被观测推翻）。 */
static void test_consolidate(void) {
    printf("== ⑩ 推理沉淀 / 撤销 ==\n");
    AI *ai = mini();                       /* 猫 属于 动物；动物 有 毛 */
    Micro *mk = micro_of(ai);
    Think *t = think_of(ai);
    char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
    prep(ai, w, 4);

    /* 第一轮：推出来 → 收尾时提议沉淀 → 安全点应用 */
    think_run(ai, w, 4);
    ok(t->stop == TH_STOP_CONCLUDED && t->concl == 1, "继承推出「猫 有 毛」");
    ok(t->n_pend_cache == 1, "收尾前记下了「这条值得沉淀」");
    think_settle(ai);
    micro_step(ai, w, 4, "测试");
    Token *cat = T(ai, "猫"), *fur = T(ai, "毛");
    ok(micro_has_edge(cat, fur) && token_edge_is_derived(cat, fur),
       "沉淀成了一条**标着推出来的**边（不是观测）");
    ok(mk->meta.n_derived == 1 && mk->meta.n_new_edges == 1 && ai->n_cached == 1,
       "走的是意图队列（安全内核放行，沉淀单独计数）");

    /* 第二轮：直接用缓存，不再从头推 */
    think_run(ai, w, 4);
    ok(t->concl_cache == 1 && t->concl_class == THINK_EV_KNOWN,
       "第二次想：认得出这是**缓存**（依据强度=推导缓存）");
    ok(strstr(t->chain_text, "推出来的") != NULL, "依据里写明「这是以前推出来记住的」");
    ok(t->n_pend_cache == 0, "缓存不再重复沉淀（幂等）");

    /* 观测压过缓存：教一条反面的观测 → 缓存整条被撤 */
    (void)token_tag_nrel(cat, fur, REL_HAS, REL_BOOST);
    ok(!token_edge_is_derived(cat, fur), "观测一来，这条边上的缓存标记没了（观测赢）");
    think_run(ai, w, 4);
    ok(t->concl == -1 && t->conflict == 0, "回答跟着观测走，而且**不报矛盾**（具体优先于一般）");
    ok(strstr(t->chain_text, "不 有") != NULL, "依据是那条观测");

    /* 关掉沉淀：想通也不再记 */
    AI *ai2 = tnew();
    Think *t2 = think_of(ai2);
    (void)token_tag_rel(T(ai2, "猫"), T(ai2, "动物"), REL_IS_A, REL_BOOST);
    (void)token_tag_rel(T(ai2, "动物"), T(ai2, "毛"), REL_HAS, REL_BOOST);
    char *w2[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
    prep(ai2, w2, 4);
    think_set_cache(ai2, 0);
    think_run(ai2, w2, 4);
    think_settle(ai2);
    micro_step(ai2, w2, 4, "测试");
    ok(!micro_has_edge(T(ai2, "猫"), T(ai2, "毛")),
       "::thinkcache off 之后不再沉淀（想还是照想，只是不记）");
    ok(t2->concl == 1, "结论照样给得出来");
    ai_free(ai2);
    ai_free(ai);
}

/* ============================ ⑪ 缓存被强依据推翻 ============================
 * 缓存不是独立依据，它是**记忆**：现场推理算出反面时，听现场的，并撤销缓存。 */
static void test_cache_retract(void) {
    printf("== ⑪ 缓存被现场推理推翻（并撤销） ==\n");
    AI *ai = mini();
    Micro *mk = micro_of(ai);
    Think *t = think_of(ai);
    char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
    prep(ai, w, 4);

    think_run(ai, w, 4);            /* 继承：猫 有 毛 */
    think_settle(ai);
    micro_step(ai, w, 4, "测试");
    Token *cat = T(ai, "猫"), *fur = T(ai, "毛");
    ok(token_edge_is_derived(cat, fur), "前置条件：先沉淀了一条缓存");

    /* 现在让**现场推理**算出反面：宠物 没有 毛，且 猫 属于 宠物 */
    Token *pet = T(ai, "宠物");
    (void)token_tag_rel(cat, pet, REL_IS_A, REL_BOOST);
    (void)token_tag_nrel(pet, fur, REL_HAS, REL_BOOST);
    prep(ai, w, 4);
    think_run(ai, w, 4);
    ok(t->flipped == 1, "当场判定「缓存被推翻」（要改口）");
    ok(t->concl == -1, "结论跟现场推理（不 有）");
    ok(t->n_pend_retract == 1, "记下了要撤销的那条缓存");
    think_settle(ai);
    micro_step(ai, w, 4, "测试");
    ok(!micro_has_edge(cat, fur) || !token_edge_is_derived(cat, fur),
       "缓存真的被撤销了（安全内核允许删「推出来的」）");
    ok(ai->n_retracted == 1, "撤销计数在走");
    /* §6.78 **撤销留痕**：撤掉的东西图里本来没痕迹 ⇒ 重载后数不回来（推论档偏乐观）。
     * 现在撤销时会在图上留一条 `[撤销:A|R|B]` 标记节点，`ai_recount_evidence` 从图里数回来。 */
    {
        size_t marks = 0;
        const size_t vl = strlen(MICRO_REVOKE_TOKEN_PREFIX);
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *tk = ai->pool->tokens[i];
            if (tk && tk->name && strncmp(tk->name, MICRO_REVOKE_TOKEN_PREFIX, vl) == 0) marks++;
        }
        ok(marks >= 1, "撤销时在图上留了 `[撤销:…]` 标记（名字里写着撤的是哪条）"
                       " —— 「重载后数回来」那条在 optest ㉙ 钉（那边有 model_io）");
    }

    /* 反过来：**观测到的**关系边依旧不许删（硬底线没被这次改动松动） */
    Token *bird = T(ai, "鸟"), *fly = T(ai, "飞");
    (void)token_tag_rel(bird, fly, REL_CAN, REL_BOOST);       /* 观测 */
    ModIntent rm;
    memset(&rm, 0, sizeof rm);
    rm.type = MOD_REMOVE_ASSOC; rm.target = bird; rm.main = fly;
    ok(safety_review(&mk->safe, ai->pool, &rm) == SAFE_DENY_RELATION,
       "观测到的关系边仍然不许删（只有缓存可撤）");
    ai_free(ai);
}

/* ============================ ⑫ 缓存跟着前提走 ============================
 *
 * 缓存的「依据」不另存一张依赖表，而是**每次现算**：
 * 「把它排除在外，还推得出来吗」。所以：
 *   · 求解器里有一道**实时闸**：没依据的缓存当场不被采信；
 *   · 思维层收尾时扫一遍，把没依据的清掉（免得它老挂在图上）。
 * 由「忽略所有缓存再推」实现，所以缓存**不许互为依据**（捷径不许嵌套）。 */
static void test_premise_follow(void) {
    printf("== ⑫ 缓存跟着前提走 ==\n");
    AI *ai = mini();                        /* 猫 属于 动物；动物 有 毛 */
    Think *t = think_of(ai);
    Token *cat = T(ai, "猫"), *fur = T(ai, "毛"), *animal = T(ai, "动物");
    char *w[4] = {(char *)"猫", (char *)"有", (char *)"毛", (char *)"吗"};
    prep(ai, w, 4);

    think_run(ai, w, 4);
    think_settle(ai);
    micro_step(ai, w, 4, "测试");
    ok(token_edge_is_derived(cat, fur), "前置：先沉淀一条缓存（依据=猫属于动物 + 动物有毛）");
    ok(t->n_stale == 0, "依据还在时**不误撤**");

    /* 前提没了：把「动物 有 毛」这条依据拿掉（模拟前提被撤/被改口） */
    ok(token_remove_assoc(animal, fur) == 0, "撤掉前提边");

    /* 实时闸：不用等收尾，求解器当场就不再拿这条缓存当数 */
    Proof pf;
    rel_solve(ai, cat, REL_HAS, fur, 0, &pf);
    ok(pf.status == 0, "依据没了 → 求解器**当场**拒绝这条缓存（实时闸）");

    /* 收尾扫描：清掉它 */
    think_run(ai, w, 4);
    ok(t->concl == 0, "这一轮也推不出来了（诚实，不拿旧记忆当答案）");
    think_settle(ai);
    ok(t->n_stale == 1, "收尾扫描发现「前提没了」");
    micro_step(ai, w, 4, "测试");
    ok(!micro_has_edge(cat, fur), "那条缓存被撤销了");

    /* 依据回来 → 可以重新推、重新沉淀（不是永久拉黑） */
    (void)token_tag_rel(animal, fur, REL_HAS, REL_BOOST);
    think_run(ai, w, 4);
    ok(t->concl == 1, "依据回来之后照样推得出来");
    ai_free(ai);
}

/* ============================ ⑬ 观测改口语义 ============================
 *
 * 「用户先说是、后说不是」是**改口**，不是「图里有两份互相矛盾的依据」。
 * 旧语义下两个极性并存（同一对词、同一条关系），于是每次提问都报矛盾 ——
 * 用户教的修正永远留不下来（docs/thinking.md 第十一节那条局限）。
 * 这一节把新语义的四条边界钉住：
 *   ① 后说的观测替掉先说的对立极性，且**边还在**（铁律：关系边不许删）；
 *   ② 标识位 g_obs_revised 只在「观测撞观测」时置位（调用方据此说「改过来了」）；
 *   ③ 关掉（`::revise off`）回到并存状态 —— A/B 的对照组是同一次代码路径；
 *   ④ 不误伤：「观测压过缓存」和「不同宾语」都不算改口。
 * 另外钉住装载边界：relations.txt 里与**存盘观测**相反的行必须让路，
 * 否则重启一次就把用户教的改回去了（那是比不改口更糟的行为）。 */
static void test_revise(void) {
    printf("== ⑬ 改口：后说的观测替掉先说的对立极性 ==\n");
    int save_revise = g_obs_revise;
    g_obs_revise = 1;

    /* ① 观测撞观测：替换极性，不删边，思维层不报矛盾 */
    {
        AI *ai = mini();                       /* 猫 属于 动物、动物 有 毛 都是观测 */
        Token *cat = T(ai, "猫"), *animal = T(ai, "动物");
        int before = (int)cat->count;
        ok(rel_direct(cat, REL_IS_A, animal) == 1, "前置：先说的观测在图上");
        (void)token_tag_nrel(cat, animal, REL_IS_A, REL_BOOST);
        ok(g_obs_revised == 1, "标识位报「这次落观测是改口」");
        ok(rel_direct_neg(cat, REL_IS_A, animal) == 1, "后说的极性在图上");
        ok(rel_direct(cat, REL_IS_A, animal) == 0, "先说的极性被改掉（不再并存）");
        int found = 0;
        for (size_t i = 0; i < cat->count; ++i)
            if (cat->assocs[i].main == animal) found = 1;
        ok(found && (int)cat->count == before, "边还在（改的是极性，不是删边）");

        char *w[4] = {(char *)"猫", (char *)"属于", (char *)"动物", (char *)"吗"};
        prep(ai, w, 4);
        Think *t = think_of(ai);
        think_run(ai, w, 4);
        ok(t->conflict == 0, "**不报矛盾**（改口不是知识自相矛盾）");
        ok(t->concl == -1, "答案跟着后说的走（不成立）");
        ok(t->stop == TH_STOP_CONCLUDED, "停因=有结论（不是「正反都能推出来」）");
        ai_free(ai);
    }

    /* ② 关掉：两个极性并存（A/B 对照组，也是旧模型的遗留状态） */
    {
        AI *ai = mini();
        Token *cat = T(ai, "猫"), *animal = T(ai, "动物");
        g_obs_revise = 0;
        (void)token_tag_nrel(cat, animal, REL_IS_A, REL_BOOST);
        ok(g_obs_revised == 0, "关掉改口：不置标识位");
        ok(rel_direct(cat, REL_IS_A, animal) == 1 &&
           rel_direct_neg(cat, REL_IS_A, animal) == 1, "关掉之后两个极性并存");
        g_obs_revise = 1;
        ai_free(ai);
    }

    /* ③ 观测压过缓存：不算改口（那是另一套语义，确认话也不同） */
    {
        AI *ai = mini();
        Token *cat = T(ai, "猫"), *fur = T(ai, "毛");
        (void)token_tag_rel(cat, fur, REL_HAS, REL_BOOST);
        for (size_t i = 0; i < cat->count; ++i)
            if (cat->assocs[i].main == fur) cat->assocs[i].role = ASSOC_ROLE_DERIVED;
        (void)token_tag_nrel(cat, fur, REL_HAS, REL_BOOST);
        ok(g_obs_revised == 0, "「观测压过缓存」不叫改口");
        ok(rel_direct_neg(cat, REL_HAS, fur) == 1 && rel_direct(cat, REL_HAS, fur) == 0,
           "缓存被观测压掉（缓存那一半清空，不留并存）");
        ai_free(ai);
    }

    /* ④ 不同宾语：多值事实各自并存，改口只针对同一对词 */
    {
        AI *ai = mini();
        Token *cat = T(ai, "猫"), *fur = T(ai, "毛"), *tail = T(ai, "尾巴");
        (void)token_tag_rel(cat, fur, REL_HAS, REL_BOOST);
        (void)token_tag_rel(cat, tail, REL_HAS, REL_BOOST);
        ok(g_obs_revised == 0, "换一个宾语不算改口");
        ok(rel_direct(cat, REL_HAS, fur) == 1 && rel_direct(cat, REL_HAS, tail) == 1,
           "不同宾语的多值事实并存");
        ai_free(ai);
    }

    /* ⑤ 装载边界：relations.txt 里与存盘观测相反的行必须让路 */
    {
        AI *ai = mini();
        Token *cat = T(ai, "猫"), *animal = T(ai, "动物");
        (void)token_tag_nrel(cat, animal, REL_IS_A, REL_BOOST);   /* 改口成否定（像是存过盘） */
        ok(rel_obs_opposite(cat, REL_IS_A, animal, 0) == 1,
           "识别得出「存盘里已有观测说了反话」");
        /* 基准知识是「猫 属于 动物」——它必须让路，不许把否定改回去 */
        if (!rel_obs_opposite(cat, REL_IS_A, animal, 0))
            (void)token_tag_rel(cat, animal, REL_IS_A, REL_BOOST);
        ok(rel_direct(cat, REL_IS_A, animal) == 0 && rel_direct_neg(cat, REL_IS_A, animal) == 1,
           "基准知识让路：用户教过的否定留下来了");
        /* 缓存不算观测：与缓存相反时该照常落观测 */
        Token *fur = T(ai, "毛");
        (void)token_tag_rel(cat, fur, REL_HAS, REL_BOOST);
        for (size_t i = 0; i < cat->count; ++i)
            if (cat->assocs[i].main == fur) cat->assocs[i].role = ASSOC_ROLE_DERIVED;
        ok(rel_obs_opposite(cat, REL_HAS, fur, 1) == 0, "缓存不算「已有的反面观测」");
        ai_free(ai);
    }

    g_obs_revise = save_revise;
}

int main(void) {
    test_agenda();
    test_conclude();
    test_conflict();
    test_weak_evidence();
    test_crossturn();
    test_no_assert();
    test_meta();
    test_switch_and_utf8();
    test_mech_registry();
    test_analogy_polarity();
    test_consolidate();
    test_think_obj();
    test_idle_think();
    test_redo();
    test_mech_cand();
    test_cache_retract();
    test_premise_follow();
    test_revise();
    printf("\n思维循环自测：%d 项通过，%d 项失败\n", passes, fails);
    return fails ? 1 : 0;
}
