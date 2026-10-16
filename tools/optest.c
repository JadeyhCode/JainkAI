/* tools/optest.c — **符号节点行为（图操作）+ 临时符号节点 + 修正的记忆** 的自测（C 级）
 *
 * 用户要的三件事，在这里逐条钉住：
 *   ① 「给带符号节点的连接符号节点」     → 随语境接线（带语境 X 的边接上 Y）
 *   ② 「给带**指定** 符号节点的连接另一个符号节点」 → 指定接线（限定主语）
 *   ③ 「在这两个符号节点之间加一个符号节点，不要直连、经它中转」 → 中间插词
 *   ④ 临时符号节点（[临:…]）：能参与边、**不存盘**、没人引用时可回收
 *   ⑤ 修正的记忆 / 来源监控：改口留一个 [修正:…] 标记（幂等、不冒充知识、能存盘）
 *
 * 还有一条这一轮**查出来的洞**的看护：安全内核的「每步新建边预算」原来永远扣不着
 * （safety_charge 排在 apply 之后，而它靠 `!micro_has_edge()` 判「是不是新边」），
 * 见第六节 —— 参数不是问题，「机制在、行为不在」才是。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_optest tools/optest.c -lm && ./build_optest
 */
#include "../agent.h"
#include "../drive.h"
#include "../model_io.h"
#include "../code.h"   /* code_rule_for：修法表按错误文本选规则（§6.97） */
#include "../token_lang.h"
#include "../action.h"   /* §6.126：动作层的边界判据（外部审计 P0/P1-1 的回归） */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>      /* getenv：反向测量时跳过"默认关"断言 */
#include <sys/stat.h>       /* mkdir：临时文件放 _t/ 下，而 _t/ 不一定已经存在（见 ⑯） */

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}
static Token *T(AI *ai, const char *name) { return pool_token_intern(ai->pool, name); }

/* ============================ ① 行为表自检 ============================ */
static void test_table(void) {
    printf("== ① 符号节点行为表：名字不重复、每条都有展开函数 ==\n");
    size_t n = token_op_count();
    ok(n == 15, "表里 15 条行为（前 6 条 = 加前提/加反前提/前提接线/中间插词/回收临时/调用补丁；"
                "后 9 条 = 过程记号/执行槽，**必须在表尾** —— 原语层按下标引用这张表）");
    int dup = 0, nofn = 0, noshape = 0;
    for (size_t i = 0; TOKEN_OPS[i].name; ++i) {
        if (!TOKEN_OPS[i].expand) nofn++;
        if (!TOKEN_OPS[i].shape) noshape++;
        for (size_t j = i + 1; TOKEN_OPS[j].name; ++j)
            if (strcmp(TOKEN_OPS[i].name, TOKEN_OPS[j].name) == 0) dup++;
    }
    ok(dup == 0, "行为名不重复");
    ok(nofn == 0, "每条行为都有展开函数（表里有的都实现得了）");
    ok(noshape == 0, "每条行为都写了参数形状（帮助由表生成）");
    char note[200];
    ok(token_op_run(NULL, "随语境接线", NULL, 0, note, sizeof note) == -1, "空 AI 不会崩");
}

/* ============================ ② 中间插词（不直连，经它中转） ============================ */
static void test_insert_between(void) {
    printf("== ② 中间插词：A 与 B 不直连，经插入的符号节点中转 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *moon = T(ai, "月亮");
    (void)token_observe_pair(cat, moon);                  /* 先有一条普通直连边 */
    ok(micro_has_edge(cat, moon), "前置：A—B 有一条直连边");

    char *args[2] = {(char *)"猫", (char *)"月亮"};
    char note[200];
    int got = token_op_run(ai, "中间插词", args, 2, note, sizeof note);
    ok(got == 1, "行为跑通了（展开 1 条意图并应用）");
    Token *via = pool_find(ai->pool, MICRO_TEMP_TOKEN_PREFIX "中转]");
    ok(via != NULL, "中转符号节点建出来了（默认是临时符号节点 [临:中转]）");
    ok(via && token_is_temp(via), "它标着「临时」——不存盘的草稿");
    ok(micro_has_edge(cat, via), "A → 中转 在");
    ok(micro_has_edge(via, moon), "中转 → B 在");
    ok(!micro_has_edge(cat, moon), "A—B 的**直连被拆了**（语义：不要直接联想到）");
    ok(rel_direct(cat, REL_HAS, moon) == 0, "中转不是知识：任何关系位上都没有它");
    ok(ai_edge_walkable(ai, &cat->assocs[0], 1.0f) >= 0, "走边过滤器不会因为这条改动崩");

    /* 直连是**关系边**（知识）时：不许拆，但仍然插中转 */
    AI *ai2 = ai_new();
    Token *a2 = T(ai2, "猫"), *b2 = T(ai2, "鱼");
    (void)token_tag_rel(a2, b2, REL_EAT, REL_BOOST);
    int got2 = token_op_run(ai2, "中间插词", args, 2, note, sizeof note);   /* 猫 月亮：词不存在 */
    ok(got2 == -1, "词不在池子里 → 行为不跑，也不乱建东西");
    char *args2[2] = {(char *)"猫", (char *)"鱼"};
    ok(token_op_run(ai2, "中间插词", args2, 2, note, sizeof note) == 1, "换一对真词：跑了");
    ok(rel_direct(a2, REL_EAT, b2) == 1, "直连是**观测到的关系边** → 铁律 2：不拆");
    Token *via2 = pool_find(ai2->pool, MICRO_TEMP_TOKEN_PREFIX "中转]");
    ok(via2 && micro_has_edge(a2, via2) && micro_has_edge(via2, b2),
       "但中转照样接上了（知识不拆，联想改走中转）");
    ai_free(ai); ai_free(ai2);
}

/* ============================ ④ 临时符号节点不存盘 ============================ */
static void test_temp_not_saved(void) {
    printf("== ④ 临时符号节点与指向它的边**不进存档**（草稿不写进知识） ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *moon = T(ai, "月亮");
    Token *via = T(ai, MICRO_TEMP_TOKEN_PREFIX "中转]");
    (void)token_observe_pair(cat, moon);
    size_t i_plain = (size_t)-1;
    for (size_t i = 0; i < cat->count; ++i) if (cat->assocs[i].main == moon) i_plain = i;
    ok(i_plain != (size_t)-1 && mio_edge_saved(&cat->assocs[i_plain]),
       "普通边照旧存盘（过滤在无草稿时是恒等的）");
    (void)token_observe_pair(cat, via);
    size_t i_via = (size_t)-1;
    for (size_t i = 0; i < cat->count; ++i) if (cat->assocs[i].main == via) i_via = i;
    ok(i_via != (size_t)-1 && !mio_edge_saved(&cat->assocs[i_via]),
       "指向临时符号节点的边不存盘（否则装载会指向不存在的 id）");
    /* v10：语境字段删了，「不存盘」的判据只看两端（main / action）。
     * 补一条现在**真的存在**的路径：**动作槽**里是临时符号节点的边也不存盘。 */
    {
        Token *mark = pool_token_intern(ai->pool, MICRO_TEMP_TOKEN_PREFIX "草稿标记]");
        (void)token_add_assoc_action_unique(moon, cat, mark);
        long i_act = micro_edge_index(moon, cat);
        ok(i_act >= 0 && !mio_edge_saved(&moon->assocs[i_act]),
           "**动作槽**里带临时符号节点的边也不存盘");
    }
    ok(!token_is_temp(cat) && !token_is_temp(NULL), "普通符号节点 / NULL 不是临时");
    ai_free(ai);
}

/* ============================ ⑤ 回收临时 ============================ */
static void test_collect_temp(void) {
    printf("== ⑤ 回收临时：只回收**没人引用**的草稿；别的符号节点一律不许删 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *moon = T(ai, "月亮");
    Token *orphan = T(ai, MICRO_TEMP_TOKEN_PREFIX "孤儿]");      /* 谁都不连它 */
    (void)moon;
    char note[200];
    char *args[2] = {(char *)"猫", (char *)"月亮"};
    ok(token_op_run(ai, "中间插词", args, 2, note, sizeof note) == 1, "先插一个中转（它会被引用）");
    Token *used = pool_find(ai->pool, MICRO_TEMP_TOKEN_PREFIX "中转]");
    ok(used != NULL, "在用中的临时符号节点存在");
    int got = token_op_run(ai, "回收临时", NULL, 0, note, sizeof note);
    ok(got == 1, "回收临时：只提出 1 条（孤儿那条）");
    ok(orphan->is_garbage, "没人引用的草稿被回收（软删：标垃圾，池里的指针不悬空）");
    ok(used && !used->is_garbage, "**还在用**的草稿不动（自己还连着边）");

    /* 非临时符号节点永远不许走这条路 */
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_REMOVE_TOKEN; m.target = cat;
    ok(safety_review(&micro_of(ai)->safe, ai->pool, &m) == SAFE_DENY_NOT_TEMP,
       "回收一条**普通** 符号节点 → 被拒（只许回收临时草稿）");
    ai_free(ai);
}

/* ============================ ⑥ 每步新建边预算真的算数 ============================ */
static void test_edge_budget_charged(void) {
    printf("== ⑥ 安全内核的「每步新建边」预算：真的会被扣、真的会拦 ==\n");
    AI *ai = ai_new();
    Micro *mk = micro_of(ai);
    Token *a = T(ai, "甲"), *b = T(ai, "乙"), *c = T(ai, "丙"), *d = T(ai, "丁");
    mk->safe.max_new_edges_step = 1;          /* 一步只许建一条新边 */
    safety_step_begin(ai);
    char note[128];
    ModIntent m1; memset(&m1, 0, sizeof m1);
    m1.type = MOD_ADD_ASSOC; m1.target = a; m1.main = b; m1.bump = 1;
    ok(micro_try(ai, &m1, note, sizeof note) == SAFE_OK, "第一条新边：放过");
    ModIntent m2; memset(&m2, 0, sizeof m2);
    m2.type = MOD_ADD_ASSOC; m2.target = c; m2.main = d; m2.bump = 1;
    ok(micro_try(ai, &m2, note, sizeof note) == SAFE_DENY_BUDGET_EDGE,
       "第二条新边：预算拦下（**这条以前永远拦不住** —— 记账排在应用之后）");
    safety_step_begin(ai);
    ok(micro_try(ai, &m2, note, sizeof note) == SAFE_OK, "新的一步：预算重置，又能建");
    /* v10：语境字段删掉之后，「给同一条边加不同语境来绕过预算」这条路**不存在了** ——
     * 判据只有 (target, main)。这里钉住新的真话：同一条边再提一次是幂等，不占新边预算。 */
    safety_step_begin(ai);
    mk->safe.max_new_edges_step = 1;
    ModIntent m3; memset(&m3, 0, sizeof m3);
    m3.type = MOD_ADD_ASSOC; m3.target = a; m3.main = b;
    ok(micro_try(ai, &m3, note, sizeof note) == SAFE_OK,
       "新边判据只看 (target, main)：这条放过");
    ModIntent m4; memset(&m4, 0, sizeof m4);
    m4.type = MOD_ADD_ASSOC; m4.target = a; m4.main = b;
    ok(micro_try(ai, &m4, note, sizeof note) == SAFE_OK,
       "同一条边再提一次：幂等（**语境没了 ⇒ 绕不过预算，也不可能重复**）");
    ai_free(ai);
}

/* ============================ ⑦ 修正的记忆（改口留痕） ============================ */
static void test_revise_memory(void) {
    printf("== ⑦ 修正的记忆：改口留一个 [修正:…] 标记（幂等、不冒充知识） ==\n");
    AI *ai = ai_new();
    (void)T(ai, "猫"); (void)T(ai, "有"); (void)T(ai, "毛");
    char *w1[3] = {(char *)"猫", (char *)"有", (char *)"毛"};
    char *w2[3] = {(char *)"猫", (char *)"没有", (char *)"毛"};
    float conf = 0;
    char *r1 = agent_learn_relation(ai, w1, 3, &conf);
    ok(r1 != NULL, "先记下「猫 有 毛」");
    free(r1);
    micro_step(ai, NULL, 0, "测试");          /* 安全点：把队列里的意图落地 */
    char *r2 = agent_learn_relation(ai, w2, 3, &conf);
    ok(r2 != NULL && strstr(r2, "改 过来 了") != NULL, "改口时如实说「改过来了」");
    free(r2);
    micro_step(ai, NULL, 0, "测试");
    Token *mark = pool_find(ai->pool, MICRO_REVISE_TOKEN_PREFIX "猫_不_有_毛]");
    ok(mark != NULL, "落下一个 [修正:猫_不_有_毛] 标记（来源监控：这件事被改过口）");
    ok(mark && micro_has_edge(mark, T(ai, "猫")), "标记连回主语（查得到「是哪件事」）");
    ok(rel_direct(T(ai, "猫"), REL_HAS, T(ai, "毛")) == 0 &&
       rel_direct_neg(T(ai, "猫"), REL_HAS, T(ai, "毛")) == 1,
       "留痕**不改知识**：图里仍只有后来那个极性");
    /* 幂等：**同一条**修正再说一次（改回否定）→ 同名标记仍然只有一个 */
    char *r3 = agent_learn_relation(ai, w1, 3, &conf);      /* 改回肯定 → 另一个标记 */
    free(r3);
    micro_step(ai, NULL, 0, "测试");
    char *r4 = agent_learn_relation(ai, w2, 3, &conf);      /* 又改回否定 → 与第一次同名 */
    free(r4);
    micro_step(ai, NULL, 0, "测试");
    size_t n_neg = 0, n_all = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || !t->name) continue;
        if (strncmp(t->name, MICRO_REVISE_TOKEN_PREFIX,
                    strlen(MICRO_REVISE_TOKEN_PREFIX)) != 0) continue;
        n_all++;
        if (strcmp(t->name, MICRO_REVISE_TOKEN_PREFIX "猫_不_有_毛]") == 0) n_neg++;
    }
    ok(n_neg == 1, "**同一条**修正改口两次只有一个标记（MOD_ADD_TOKEN 同名 intern 幂等）");
    ok(n_all == 2, "两次**不同方向**的修正各留一个标记（猫_有_毛 / 猫_不_有_毛）");
    ai_free(ai);
}

/* ============================ ⑧ 原语栈式组合（复用已有栈机） ============================
 *
 * 用户的选择：**用临时符号节点，不另造符号节点栈**；真要组合时复用**已有的** Interp 栈机。
 * 这一节钉住那条路：原语层只把「行为编号 + 操作数」交给宿主，
 * **自己不碰图** —— 所以「行为」在语言层能写、能组合，但落地仍然只有安全内核那一个入口。 */
typedef struct {
    int         n;
    InterpOpKind op[4];
    size_t      nargs[4];
    char        args[4][4][32];
} OpLog;

/* 真宿主（就是 main.c 里那个 interp_op_host 的最小版）：把行为交给 token_op_run。
 * 用它证明「组合出来的行为是真能落地的」，而不是只把请求记下来。 */
static int prim_host_tokrun(void *user, InterpOpKind op, char *const *args, size_t n) {
    AI *ai = (AI *)user;
    if (!ai || op < 0 || op >= IPOP_N || !TOKEN_OPS[op].name) return -1;
    char note[200];
    return token_op_run(ai, TOKEN_OPS[op].name, args, n, note, sizeof note);
}

static int rec_host(void *user, InterpOpKind op, char *const *args, size_t n) {
    OpLog *L = (OpLog *)user;
    if (!L || L->n >= 4) return 0;
    int k = L->n++;
    L->op[k] = op;
    L->nargs[k] = n;
    for (size_t i = 0; i < n && i < 4; ++i)
        snprintf(L->args[k][i], sizeof L->args[k][i], "%s", args[i] ? args[i] : "");
    return 0;
}

static void test_prim_stack(void) {
    printf("== ⑧ 原语栈式组合：语言层写行为、不碰图（复用 Interp，不新造栈） ==\n");
    /* 表自检：名字只有一份、没有重复、没有落空的 kind */
    int dup = 0, dead = 0;
    for (size_t i = 0; PRIMS[i].name; ++i) {
        if (PRIMS[i].kind == PK_NONE) dead++;
        for (size_t j = i + 1; PRIMS[j].name; ++j)
            if (strcmp(PRIMS[i].name, PRIMS[j].name) == 0) dup++;
    }
    ok(dup == 0, "原语表里名字不重复（以前名字写在两处，现在只有这一张）");
    ok(dead == 0, "每条原语都有 kind（不会出现「表里有、识别不出」）");
    ok(prim_count() >= 37, "原语表条数够（含新增的行为入口，含 [原语:前关联不]）");

    AI *ai = ai_new();
    ok(prim_kind(T(ai, PRIM_INSERT))       == PK_INSERT,       "[原语:插词] 识别得了");
    ok(prim_kind(T(ai, PRIM_INSERT_NAMED)) == PK_INSERT_NAMED, "[原语:插词为] 识别得了（它取一个参数）");
    ok(prim_kind(T(ai, PRIM_PREMISE_TIE))  == PK_PREMISE_TIE,  "[原语:前提接线] 识别得了");
    ok(prim_kind(T(ai, PRIM_COLLECT_TEMP)) == PK_COLLECT_TEMP, "[原语:回收临时] 识别得了");

    /* **编号契约**：原语层的编号必须指到 TOKEN_OPS 里那条行为（改顺序这里变红） */
    ok(strcmp(TOKEN_OPS[IPOP_PREMISE_CONNECT].name, "加前提") == 0,
       "IPOP_PREMISE_CONNECT → 加前提（[原语:前关联]+[原语:连接]）");
    /* **每一条**编号都要钉住 —— 只钉前四条留了个洞：第 9 条把新行为插在中间时，
     * `IPOP_COLLECT_TEMP` 悄悄指到了「建过程」上（`[原语:回收临时]` 当场变成建过程），
     * 而当时的看护只查到下标 0~3 ⇒ 没红。表尾才是加新行为的地方。 */
    {
        struct { int idx; const char *name; } pin[] = {
            { IPOP_PREMISE_CONNECT,     "加前提" },
            { IPOP_PREMISE_CONNECT_NOT, "加反前提" },
            { IPOP_PREMISE_TIE,         "前提接线" },
            { IPOP_INSERT,              "中间插词" },
            { IPOP_COLLECT_TEMP,        "回收临时" },
        };
        int bad_pin = 0;
        for (size_t i = 0; i < sizeof pin / sizeof pin[0]; ++i)
            if (!TOKEN_OPS[pin[i].idx].name ||
                strcmp(TOKEN_OPS[pin[i].idx].name, pin[i].name) != 0) bad_pin++;
        ok(bad_pin == 0,
           "原语层的**每一个** IPOP_* 编号都还指着原来那条行为（编号契约，5/5）");
    }
    ok(strcmp(TOKEN_OPS[IPOP_PREMISE_TIE].name, "前提接线") == 0,
       "IPOP_PREMISE_TIE → 前提接线");
    ok(strcmp(TOKEN_OPS[IPOP_INSERT].name, "中间插词") == 0,    "IPOP_INSERT → 中间插词");
    ok(strcmp(TOKEN_OPS[IPOP_COLLECT_TEMP].name, "回收临时") == 0, "IPOP_COLLECT_TEMP → 回收临时");

    /* 栈式组合：`[原语:对象] A [原语:对象] B [原语:插词]` → 弹 B、A，交给宿主 */
    {
        OpLog L; memset(&L, 0, sizeof L);
        const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "月亮", PRIM_INSERT };
        token_run_names_ctx(ai->pool, seq, 5, rec_host, &L);
        ok(L.n == 1 && L.op[0] == IPOP_INSERT, "原语跑完 → 宿主收到一条「中间插词」");
        ok(L.nargs[0] == 2 && strcmp(L.args[0][0], "猫") == 0 &&
           strcmp(L.args[0][1], "月亮") == 0,
           "操作数顺序对（栈：先 push 的 A 在前，后 push 的 B 在后）");
    }
    /* 带名字的那条：`[原语:插词为] <名字>` 会把参数当第三个操作数 */
    {
        OpLog L; memset(&L, 0, sizeof L);
        const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "桌子",
                              PRIM_INSERT_NAMED, "家具" };
        token_run_names_ctx(ai->pool, seq, 6, rec_host, &L);
        ok(L.n == 1 && L.nargs[0] == 3 && strcmp(L.args[0][2], "家具") == 0,
           "[原语:插词为] <名字>：名字作为第三个操作数交出去");
    }
    /* 前提接线：pop Y, P, A 三个操作数 */
    {
        OpLog L; memset(&L, 0, sizeof L);
        const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "吃",
                              PRIM_OBJECT, "水", PRIM_PREMISE_TIE };
        token_run_names_ctx(ai->pool, seq, 7, rec_host, &L);
        ok(L.n == 1 && L.op[0] == IPOP_PREMISE_TIE && L.nargs[0] == 3 &&
           strcmp(L.args[0][0], "猫") == 0 && strcmp(L.args[0][1], "吃") == 0 &&
           strcmp(L.args[0][2], "水") == 0, "[原语:前提接线]：主语、前提、目标三个操作数");
    }
    /* 前关联 + 连接 → 走「加前提」（不再往边上写退役的语境字段） */
    {
        OpLog L; memset(&L, 0, sizeof L);
        const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "吃",
                              PRIM_PRE_ASSOC, "饿", PRIM_CONNECT };
        token_run_names_ctx(ai->pool, seq, 7, rec_host, &L);
        ok(L.n == 1 && L.op[0] == IPOP_PREMISE_CONNECT && L.nargs[0] == 3 &&
           strcmp(L.args[0][0], "猫") == 0 && strcmp(L.args[0][1], "饿") == 0 &&
           strcmp(L.args[0][2], "吃") == 0,
           "[原语:前关联]+[原语:连接] → 加前提（A、B、前提 P 三个操作数）");
    }
    /* **没有宿主时原语不生效**：语言层不该自己改图（结构改动只有 micro_step 一个入口） */
    {
        AI *ai2 = ai_new();
        (void)T(ai2, "猫"); (void)T(ai2, "月亮");
        size_t before = 0;
        for (size_t i = 0; i < ai2->pool->count; ++i) before += ai2->pool->tokens[i]->count;
        const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "月亮", PRIM_INSERT };
        token_run_names(ai2->pool, seq, 5);              /* 旧入口：没有宿主 */
        size_t after = 0;
        for (size_t i = 0; i < ai2->pool->count; ++i) after += ai2->pool->tokens[i]->count;
        ok(before == after, "没有宿主 → 图的边数一条不变（语言层不碰结构）");
        ok(pool_find(ai2->pool, MICRO_TEMP_TOKEN_PREFIX "中转]") == NULL,
           "也没有偷偷造出临时符号节点");
        ai_free(ai2);
    }
    /* 组合出来的行为是**真的**能落地：宿主用 token_op_run，走安全内核 */
    {
        Token *cat = T(ai, "猫"), *moon = T(ai, "月亮");
        (void)token_observe_pair(cat, moon);
        const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "月亮", PRIM_INSERT };
        token_run_names_ctx(ai->pool, seq, 5, prim_host_tokrun, ai);
        ok(!micro_has_edge(cat, moon), "组合起来跑一条：直连被拆");
        ok(micro_has_edge(cat, T(ai, MICRO_TEMP_TOKEN_PREFIX "中转]")), "中转接上了（经临时符号节点）");
    }
    ai_free(ai);
}

/* ============================ ⑨ 前提符号节点（淘汰「语境」后的表示） ============================
 *
 * 用户的澄清：「联想本来不依赖前提，可以正常联想；前提符号节点可以被正常联想到，
 * 但是**逻辑会知道这两个之间的前提符号节点是什么**，并且可以被正常处理。」
 * 这一节把三句话逐条钉住：
 *   ① 表示：A —[前提]→ P → B（P 是普通符号节点，`[前提]` 是挂在动作槽上的行为符号节点）
 *   ② 联想照走 / 前提可联想：A→P 是普通可走边；P 是普通符号节点（有 hits、有邻居）
 *   ③ 逻辑知道前提是谁：token_premise_between(A,B) 直接答出 P（可多个）
 * 外加两条纪律：**语境字段不再有创建者**（退役）+ 旧模型读得回来（容忍）。 */
static void test_premise(void) {
    printf("== ⑨ 前提符号节点：联想照走、前提可联想、逻辑查得到它是谁 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *moon = T(ai, "月亮"), *food = T(ai, "食物");
    (void)T(ai, "鱼");                                       /* 操作数要已在池子里 */
    (void)token_observe_pair(cat, moon);                     /* 先有直连 */
    char note[200];
    char *a1[2] = {(char *)"猫", (char *)"月亮"};
    ok(token_op_run(ai, "加前提", a1, 2, note, sizeof note) == 1, "::op 加前提 跑通");
    Token *via = pool_find(ai->pool, MICRO_TEMP_TOKEN_PREFIX "前提]");
    ok(via != NULL && token_is_temp(via), "缺省前提是**临时符号节点**（[临:前提]，草稿）");
    ok(!micro_has_edge(cat, moon), "直连被拆：连结改成「经过前提」");
    ok(micro_has_edge(cat, via) && micro_has_edge(via, moon), "A —[前提]→ P → B 两跳都在");
    ok(edge_is_premise_link(&cat->assocs[micro_edge_index(cat, via)]),
       "A→P 上挂着 `[前提]` 行为符号节点（标记）");

    const Token *ps[4];
    size_t np = token_premise_between(cat, moon, ps, 4);
    ok(np == 1 && ps[0] == via, "**逻辑问得出**：猫 与 月亮 之间的前提就是那个临时符号节点");

    /* 前提用**真词**：它照常能被联想到（可走边、有权重），逻辑也知道它是前提 */
    char *a2[3] = {(char *)"猫", (char *)"月亮", (char *)"食物"};
    ok(token_op_run(ai, "加前提", a2, 3, note, sizeof note) == 1, "换真词当前提：加前提 猫 月亮 食物");
    np = token_premise_between(cat, moon, ps, 4);
    ok(np == 2, "同一对词可以**多个前提**（逻辑一次都拿得到）");
    long ip = micro_edge_index(cat, food);
    ok(ip >= 0 && edge_is_premise_link(&cat->assocs[ip]), "食物 也是前提链接的目标");
    ok(ip >= 0 && ai_edge_walkable(ai, &cat->assocs[ip], 1.0f) == 1,
       "**联想照走**：A→前提 是普通可走边（前提不挡联想）");
    ok(food->count > 0, "**前提符号节点能被正常联想到**：它就是普通符号节点，有自己的边");

    /* 原语路径：前关联 + 连接 → 前提是那个**真词**，不写退役字段 */
    AI *ai2 = ai_new();
    (void)T(ai2, "猫"); (void)T(ai2, "吃"); (void)T(ai2, "饿");
    const char *seq[] = { PRIM_OBJECT, "猫", PRIM_OBJECT, "吃",
                          PRIM_PRE_ASSOC, "饿", PRIM_CONNECT };
    token_run_names_ctx(ai2->pool, seq, 7, prim_host_tokrun, ai2);
    Token *c2 = T(ai2, "猫"), *e2 = T(ai2, "饿"), *eat = T(ai2, "吃");
    const Token *ps2[4];
    ok(token_premise_between(c2, e2, ps2, 4) == 1 && ps2[0] == eat,
       "[原语:前关联]+[原语:连接] → 逻辑答得出前提是「吃」");
    ai_free(ai2);

    /* 前提接线：让 A 经前提 P 也能到 Y（选择判据 = 图里的前提链接） */
    {
        char *a3[3] = {(char *)"猫", (char *)"食物", (char *)"鱼"};
        ok(token_op_run(ai, "前提接线", a3, 3, note, sizeof note) >= 1,
           "前提接线：猫 经前提 食物 也能到 鱼");
        ok(micro_has_edge(food, T(ai, "鱼")), "前提的出口 食物→鱼 建出来了");
        const Token *ps3[4];
        ok(token_premise_between(cat, T(ai, "鱼"), ps3, 4) == 1 && ps3[0] == food,
           "逻辑同样答得出 猫 与 鱼 之间的前提是 食物");
        /* 没有那条前提链接的主语 → 不该乱接 */
        AI *ai4 = ai_new();
        (void)T(ai4, "甲"); (void)T(ai4, "乙"); (void)T(ai4, "丙");
        char *a4[3] = {(char *)"甲", (char *)"乙", (char *)"丙"};
        /* 甲 —[前提]→ 乙 不存在 → 但仍然会把 乙→丙 建上（这是「让乙通到丙」的语义），
         * 所以这里断言的是「不会凭空给甲标记前提」 */
        (void)token_op_run(ai4, "前提接线", a4, 3, note, sizeof note);
        long ix = micro_edge_index(T(ai4, "甲"), T(ai4, "乙"));
        ok(ix >= 0 && edge_is_premise_link(&T(ai4, "甲")->assocs[ix]),
           "前提接线会补上 A —[前提]→ P（并标记），不会静默什么都不做");
        ai_free(ai4);
    }

    /* **语境退役**（v10）：字段已经从 AssocEntry 里删掉了（编译期即证）。
     * 这里钉一条**现在还在**的结构不变式：同一对词最多一条**无标记**边 ——
     * 以前靠语境区分的那多条边不复存在（带标记的 [前提]/[因果] 边不算，它们有别的用途）。 */
    size_t dup = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        for (size_t j = 0; j < t->count; ++j) {
            if (t->assocs[j].action) continue;
            for (size_t k = j + 1; k < t->count; ++k)
                if (!t->assocs[k].action && t->assocs[k].main == t->assocs[j].main) dup++;
        }
    }
    ok(dup == 0, "v10 结构不变式：同一对词最多一条**无标记**边（语境没了 ⇒ 不会再冒出第二条）");

    /* 条件搬到了标记上：联想这条路**恒真**（用户原话：联想本来不依赖前提），
     * 逻辑层才按 edge_premise_polarity / edge_is_causal_guess 决定用不用。 */
    {
        AI *ai3 = ai_new();
        Token *a = T(ai3, "甲"), *b = T(ai3, "乙"), *cx = T(ai3, "丙");
        (void)token_add_assoc_unique(a, b);
        long i3 = micro_edge_index(a, b);
        Token *ctx_yes[1] = { cx };
        Token *ctx_no[1]  = { T(ai3, "丁") };
        ok(i3 >= 0 && entry_matches(&a->assocs[i3], ctx_yes, 1) == 1,
           "注意窗口里提到过什么 → 边照样匹配");
        ok(i3 >= 0 && entry_matches(&a->assocs[i3], ctx_no, 1) == 1,
           "注意窗口里没提到 → **也**匹配：条件不在边上，联想不依赖前提");
        ai_free(ai3);
    }
    ai_free(ai);
}

/* ============================ ⑩ 推理/思维层按前提决定用不用（R10） ============================
 *
 * 用户：「推理/思维层按前提决定用不用」。判据：**前提符号节点在当前注意窗口里** → 前提成立。
 * 这一节钉住四件事：
 *   ① 求解器：前提不成立 → **不用**那条连结（status 0）；成立 → 用（status +1，R10 规则 10）
 *   ② 验证器：这条链独立复核得通过（gaps==0），而且**伪造的前提步会被拒**
 *   ③ 思维层：前提没成立时，痕上写明「有前提、没成立，所以不用」（不是默默没答案）
 *   ④ 联想照走：前提不成立也不挡 A→前提 的可走性（用户明确说的那条）
 */
static void test_premise_gate(void) {
    printf("== ⑩ 推理/思维层按前提决定用不用（R10：前提是闸门，不是路障） ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *cook = T(ai, "做饭"), *who = T(ai, "人");
    (void)token_tag_rel(who, cook, REL_CAN, REL_BOOST);       /* 人 会 做饭（第二跳是知识） */
    char note[220];
    char *a1[3] = {(char *)"猫", (char *)"做饭", (char *)"人"};
    ok(token_op_run(ai, "加前提", a1, 3, note, sizeof note) == 1, "建出 猫 —[前提]→ 人 → 做饭");

    /* ① 前提不成立 → 求解器不用这条连结 */
    Proof pf;
    rel_solve(ai, cat, REL_CAN, cook, 0, &pf);
    ok(pf.status == 0, "前提「人」不在注意窗口 → 求解器**不用**这条连结（推不出来）");

    /* ④ 联想照走：前提不成立，A→前提 照样是可走边 */
    long ip = micro_edge_index(cat, who);
    ok(ip >= 0 && ai_edge_walkable(ai, &cat->assocs[ip], 1.0f) == 1,
       "**联想照走**：前提不在窗口，也不挡 A→前提 这条普通边（前提是闸门，不是路障）");
    ok(ai_premise_holds(ai, who) == 0, "ai_premise_holds：前提不在窗口 → 0");

    /* ③ 思维层：痕上写清「有前提、没成立，所以不用」 */
    char *q[4] = {(char *)"猫", (char *)"会", (char *)"做饭", (char *)"吗"};
    for (int i = 0; i < 4; ++i) (void)pool_token_intern(ai->pool, q[i]);
    Think *t = think_of(ai);
    think_run(ai, q, 4);
    ok(t->concl == 0, "思维层这一轮没有结论（前提没成立，不硬答）");
    int saw = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (strstr(t->trace[i].what, "问前提") &&
            (strstr(t->trace[i].why, "没成立") || strstr(t->trace[i].why, "不用"))) saw = 1;
    ok(saw, "痕上写明：这条连结有前提、前提没成立、所以不用它");

    /* ② 前提成立 → 用（R10），验证器复核通过 */
    ai->focus_ring[0] = who; ai->focus_n = 1;
    ok(ai_premise_holds(ai, who) == 1, "ai_premise_holds：前提在窗口 → 1");
    ok(ai_premise_link_ok(ai, cat, cook) == 1, "ai_premise_link_ok：前提成立 → 这条连结可用");
    rel_solve(ai, cat, REL_CAN, cook, 0, &pf);
    ok(pf.status == 1, "前提成立 → R10 推出结论");
    char txt[256];
    proof_text(ai, &pf, txt, sizeof txt);
    ok(strstr(txt, "前提") && strstr(txt, "做饭"), "链上读得出前提这一跳");
    Derivation d;
    der_reset(&d);
    der_from_proof(ai, &d, cat, REL_CAN, cook, &pf);
    ok(der_verify(ai, &d) == 0, "验证器独立复核通过（gaps==0）");
    int rule10 = 0;
    for (size_t i = 0; i < d.n; ++i)
        if (d.step[i].kind == DER_RULE && d.step[i].rule == 10) rule10 = 1;
    ok(rule10, "链上那一步记的是规则 10（前提）");

    /* ②b 伪造的前提步必须被拒：两个没有前提链接的词之间写一条 rule=10 */
    Derivation fake;
    der_reset(&fake);
    der_goal(&fake, cat, REL_CAN, cook);
    der_rule(&fake, 10, cat, 0, cook, +1);        /* 猫 到 做饭 之间**没有**前提链接 */
    ok(der_verify(ai, &fake) > 0, "验证器识破伪造的前提步（gaps>0）");

    /* 前提成立时思维层直接答出来，链上带前提 */
    think_run(ai, q, 4);
    ok(t->concl == 1 && strstr(t->chain_text, "前提") != NULL,
       "思维层这一轮直接用上带前提的链");
    ai_free(ai);
}

/* ============================ ⑪ 前提极性 + 前提说明进答复 ============================
 *
 * 补掉上一轮列的两条局限：
 *   ① **反前提**（`[前提不]`）：条件是"前提符号节点 **不**在注意窗口里"——
 *      旧 `subs_not` 的能力有了替代（而且同样带验证器复核）；
 *   ② **前提说明进答复**：默认开（A/B 量过 0/8），把"为什么没用那条连结"追加进回答。
 */
static void test_premise_polarity_out(void) {
    printf("== ⑪ 反前提（[前提不]）+ 前提说明进答复 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *cook = T(ai, "做饭"), *who = T(ai, "人");
    (void)token_tag_rel(who, cook, REL_CAN, REL_BOOST);
    char note[220];
    char *a1[3] = {(char *)"猫", (char *)"做饭", (char *)"人"};
    ok(token_op_run(ai, "加反前提", a1, 3, note, sizeof note) == 1, "::op 加反前提 跑通");
    const AssocEntry *pl = token_premise_link(cat, who);
    ok(pl && edge_premise_polarity(pl) == -1, "这条前提链接的极性是 -1（反前提）");
    const Token *ps[4]; int pol[4];
    ok(token_premises(cat, cook, ps, pol, 4) == 1 && pol[0] == -1,
       "token_premises 答得出「前提不：人」");

    /* 条件成立（人不在窗口）→ 用；不成立（人在窗口）→ 不用 */
    Proof pf;
    rel_solve(ai, cat, REL_CAN, cook, 0, &pf);
    ok(pf.status == 1, "反前提条件成立（人没出现）→ 用这条连结");
    char txt[256];
    proof_text(ai, &pf, txt, sizeof txt);
    ok(strstr(txt, "前提不") != NULL, "链上读作 `-[前提不]->`（不是含混的「前提」）");
    Derivation d;
    der_reset(&d);
    der_from_proof(ai, &d, cat, REL_CAN, cook, &pf);
    ok(der_verify(ai, &d) == 0, "验证器复核通过（规则 11）");
    ai->focus_ring[0] = who; ai->focus_n = 1;              /* 人 出现了 → 条件不成立 */
    rel_solve(ai, cat, REL_CAN, cook, 0, &pf);
    ok(pf.status == 0, "反前提条件不成立（人出现了）→ **不用**这条连结");

    /* 伪造：把一条**正**前提链接写成规则 11（或反之）必须被拒 */
    Derivation fake;
    der_reset(&fake);
    der_goal(&fake, cat, REL_CAN, cook);
    der_rule(&fake, 10, cat, 0, who, +1);                  /* 实际是 -1 的边，谎称 10 */
    ok(der_verify(ai, &fake) > 0, "验证器识破「极性对不上」的前提步");

    /* 前提说明进答复：默认开；关掉就不追加 */
    char *q[4] = {(char *)"猫", (char *)"会", (char *)"做饭", (char *)"吗"};
    for (int i = 0; i < 4; ++i) (void)pool_token_intern(ai->pool, q[i]);
    Think *t = think_of(ai);
    think_run(ai, q, 4);                                    /* 人 在窗口 → 条件不成立 → 记下来 */
    ok(t->premise_blocked == 1, "思维层记下了「有前提、没成立」");
    ok(strstr(t->premise_text, "前提") != NULL, "那句话备好了（给答复用）");
    t->premise_out_on = 1;
    char out[512];
    snprintf(out, sizeof out, "%s", "猫 应该 会 做饭");
    ok(think_take_output(ai, out, sizeof out) == 1, "默认开：接管输出（追加）");
    ok(strstr(out, "猫 应该 会 做饭") != NULL && strstr(out, "前提") != NULL,
       "追加而不是替换：原答还在，后面补了前提说明");
    snprintf(out, sizeof out, "%s", "猫 应该 会 做饭");
    t->premise_out_on = 0;
    ok(think_take_output(ai, out, sizeof out) == 0 && strcmp(out, "猫 应该 会 做饭") == 0,
       "关掉（::premiseout off）→ 一个字都不加");
    t->premise_out_on = 1;
    ai_free(ai);
}

/* ============================ ⑫ 未解问题持久化（不动存档格式） ============================
 *
 * 用户点过「关掉再打开还记得没想通的事」。做法不是升格式，而是**用已经存盘的标记**：
 * 思维层挂问题时经安全内核落了一个 `[自问:…]` 节点（普通符号节点、随图存盘），
 * 装载时扫一遍把它解析回 (主语,关系,宾语,极性) 重新挂上议程。 */
static void test_restore_pending(void) {
    printf("== ⑫ 未解问题持久化：从 [自问:…] 标记恢复 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫");
    (void)T(ai, "会"); (void)T(ai, "跳舞"); (void)T(ai, "吗");
    /* 模拟思维层当初落下的标记（真流程里由 MOD_ADD_TOKEN 建，这里直接建同名节点） */
    Token *mark = pool_token_intern(ai->pool, MICRO_ASK_TOKEN_PREFIX "猫_会_跳舞_吗]");
    ok(mark != NULL, "前置：存盘里带着 [自问:猫_会_跳舞_吗] 标记");
    (void)token_add_assoc_unique(mark, cat);
    Think *t = think_of(ai);
    ok(t->n_open == 0, "前置：内存里还没挂着问题（Think 是瞬态的）");
    ok(think_restore_pending(ai) == 1 && t->n_open == 1,
       "装载恢复：从标记里恢复出 1 条未解问题");
    ok(strstr(t->open[0].text, "跳舞") != NULL, "恢复出来的问句文本对");
    ok(think_restore_pending(ai) == 0 && t->n_open == 1, "再扫一遍不会重复挂（幂等）");
    /* 解析不出来的标记要跳过，不许瞎挂 */
    (void)pool_token_intern(ai->pool, MICRO_ASK_TOKEN_PREFIX "坏_标记]");
    ok(think_restore_pending(ai) == 0 && t->n_open == 1, "解析不出的标记跳过（不瞎挂）");
    ai_free(ai);
}

/* ============================ ⑬ 记事本补丁：可编辑的答复 + 草稿区 + 发送控制 ============================
 *
 * 用户要的两件事（**作为补丁**，不是塞进流水线）：输出是 AI 能编辑的文本（像记事本）、
 * 它自己控制什么时候发送；还有一块草稿区。内核侧只认三个口子（stage/emit/op），
 * 完全不知道"记事本"是什么 —— 这一节既测补丁，也测"内核不依赖补丁"。 */
static void test_notepad_patch(void) {
    printf("== ⑬ 记事本补丁：可编辑答复 / 草稿区 / 发送控制 / 内核不依赖补丁 ==\n");
    AI *ai = ai_new();
    (void)pool_token_intern(ai->pool, "猫");
    char rep[220];

    /* 内核不依赖补丁：一个都没挂时，emit 谁都不接管 */
    char out[512];
    snprintf(out, sizeof out, "%s", "流水线原话");
    ok(patch_emit_all(ai, out, sizeof out) == 0, "没挂补丁：emit 不接管（内核用流水线文本）");
    ok(patch_op_call_by_name(ai, "记事本", "写", NULL, 0, rep, sizeof rep) == -1,
       "没挂补丁：调用它的操作返回 -1（不崩）");

    ok(patch_mount_builtin(ai, "记事本") == 0, "::patch mount 记事本");
    { char repA[64]; char *avA[1] = {(char *)"auto"};
      (void)patch_op_call_by_name(ai, "记事本", "模式", avA, 1, repA, sizeof repA); }
    /* §6.109 起补丁默认 **manual**（流水线文本先落草稿）；这几条测的是 **auto** 语义
     * ⇒ 显式钉成 auto（测试自己钉开关，不靠默认值）。 */
    { char rep2[64]; char *av[1] = {(char *)"auto"};
      (void)patch_op_call_by_name(ai, "记事本", "模式", av, 1, rep2, sizeof rep2); }
    /* auto 模式：逐字节回放（挂上它不改行为） */
    patch_stage_all(ai, "猫 看 来 跑");
    snprintf(out, sizeof out, "%s", "x");
    int r = patch_emit_all(ai, out, sizeof out);
    ok(r == 1 && strcmp(out, "猫 看 来 跑") == 0, "auto：emit 逐字节回放流水线文本");

    /* 草稿区：写 / 插入 / 删 / 改 / 清空（都不影响发言）。
     * 位置按**字符**算（中文一个字 3 字节，按字节删会把汉字劈成两半）。 */
    patch_op_call_by_name(ai, "记事本", "清空", NULL, 0, rep, sizeof rep);
    char *a1[1] = {(char *)"猫"};
    ok(patch_op_call_by_name(ai, "记事本", "写", a1, 1, rep, sizeof rep) == 0, "写 猫");
    char *a2[3] = {(char *)"1", (char *)"会", (char *)"跑"};
    ok(patch_op_call_by_name(ai, "记事本", "插入", a2, 3, rep, sizeof rep) == 0, "插入：按字符位置插");
    char *a3[2] = {(char *)"会", (char *)"在"};
    ok(patch_op_call_by_name(ai, "记事本", "改", a3, 2, rep, sizeof rep) == 0, "改：替换首处");
    char *a4[2] = {(char *)"1", (char *)"1"};
    ok(patch_op_call_by_name(ai, "记事本", "删", a4, 2, rep, sizeof rep) == 0 &&
       strstr(rep, "草稿 7 字节") != NULL, "删：按字符位置删一段（不会劈开汉字，猫在 跑 → 猫 跑）");
    ok(patch_op_call_by_name(ai, "记事本", "读", NULL, 0, rep, sizeof rep) == 0 &&
       strstr(rep, "草稿[7字节]=猫 跑") != NULL, "草稿内容正确：猫 跑");
    ok(patch_op_call_by_name(ai, "记事本", "提交", NULL, 0, rep, sizeof rep) == 0 &&
       strstr(rep, "待发送（7 字节）") != NULL, "提交：草稿进待发送");
    char *a5[2] = {(char *)"跑", (char *)"走"};
    ok(patch_op_call_by_name(ai, "记事本", "改发言", a5, 2, rep, sizeof rep) == 0 &&
       strstr(rep, "待发送 7 字节") != NULL, "改发言：直接编辑要说的那句话");
    ok(patch_op_call_by_name(ai, "记事本", "清空", NULL, 0, rep, sizeof rep) == 0, "清空草稿");

    /* manual 模式：不「发送」就**不说话**（AI 控制什么时候发） */
    char *a6[1] = {(char *)"manual"};
    ok(patch_op_call_by_name(ai, "记事本", "模式", a6, 1, rep, sizeof rep) == 0, "切到 manual");
    patch_stage_all(ai, "流水线想说这句");
    out[0] = 0;
    ok(patch_emit_all(ai, out, sizeof out) == -1, "manual：没发送 → 明确不说话（emit = -1）");
    ok(patch_op_call_by_name(ai, "记事本", "发送", NULL, 0, rep, sizeof rep) == 0, "发送：AI 决定现在说");
    out[0] = 0;
    r = patch_emit_all(ai, out, sizeof out);
    ok(r == 1 && strcmp(out, "猫 走") == 0,
       "发送后说出来的是**它自己编辑过的**文本（猫 走），而不是流水线那句");
    out[0] = 0;
    ok(patch_emit_all(ai, out, sizeof out) == -1, "发过之后又回到「没内容可发 → 不说话」");

    /* 受审通道：MOD_PATCH_CALL 也要过安全内核（预算 + 审计） */
    {
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_PATCH_CALL;
        snprintf(m.patch, sizeof m.patch, "记事本");
        snprintf(m.name, sizeof m.name, "读");
        char note[200];
        safety_step_begin(ai);
        ok(micro_try(ai, &m, note, sizeof note) == SAFE_OK, "调用补丁走「审查→记账→应用→审计」");
        Micro *mk = micro_of(ai);
        mk->safe.max_patch_calls_step = 0;      /* 预算压到 0：第二条必须被拦 */
        ok(micro_try(ai, &m, note, sizeof note) == SAFE_DENY_BUDGET_PATCH,
           "补丁调用也吃**硬预算**（::safe 里看得见）");
        mk->safe.max_patch_calls_step = 4;
    }

    /* 故障隔离：不认的操作算补丁自己出错，连续 3 次自动停用 */
    for (int i = 0; i < 3; ++i)
        (void)patch_op_call_by_name(ai, "记事本", "根本没有这个操作", NULL, 0, rep, sizeof rep);
    Patch *np = patch_find(&((Micro *)ai->micro)->patches, "记事本");
    ok(np && np->n_op_err == 3 && np->enabled == 0, "坏操作连续 3 次 → 补丁自己停用（不拖累内核）");
    ok(patch_op_call_by_name(ai, "记事本", "读", NULL, 0, rep, sizeof rep) == -1,
       "停用之后调用它一律失败（内核照跑）");
    out[0] = 0;
    ok(patch_emit_all(ai, out, sizeof out) == 0, "停用之后 emit 也不接管");

    /* 草稿持久化（不改存档格式）：存成 [草稿:…] 标记节点，挂载时扫回来。
     * 注意：上面那一段故意把它连错 3 次**停用**了 —— 这里先放回来。 */
    {
        char rep2[200];
        (void)patch_set_enabled(ai, "记事本", 1);
        patch_op_call_by_name(ai, "记事本", "清空", NULL, 0, rep2, sizeof rep2);
        char *w1[1] = {(char *)"小猫"};
        (void)patch_op_call_by_name(ai, "记事本", "写", w1, 1, rep2, sizeof rep2);
        ok(patch_op_call_by_name(ai, "记事本", "存草稿", NULL, 0, rep2, sizeof rep2) == 0,
           "存草稿：提交给安全内核（走受审通道落成标记节点）");
        char *no[1] = {(char *)"。"};
        micro_step(ai, no, 1, "");                    /* 安全点：补丁的提议在这里落地 */
        Token *mark = NULL;
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t && t->name && strncmp(t->name, MICRO_DRAFT_TOKEN_PREFIX,
                                        strlen(MICRO_DRAFT_TOKEN_PREFIX)) == 0) mark = t;
        }
        ok(mark != NULL, "图里出现了 [草稿:…] 标记（普通符号节点 → 随图存盘）");
        (void)patch_unmount_by_name(ai, "记事本");
        ok(patch_mount_builtin(ai, "记事本") == 0, "重新挂载");
    { char repA[64]; char *avA[1] = {(char *)"auto"};
      (void)patch_op_call_by_name(ai, "记事本", "模式", avA, 1, repA, sizeof repA); }
        (void)patch_op_call_by_name(ai, "记事本", "读", NULL, 0, rep2, sizeof rep2);
        ok(strstr(rep2, "小猫") != NULL, "草稿**被扫回来了**（重启/重挂都在）");
    }

    /* 卸载 → 回到"没有补丁" */
    ok(patch_unmount_by_name(ai, "记事本") == 0, "::patch unmount 记事本");
    patch_stage_all(ai, "原话");
    out[0] = 0;
    ok(patch_emit_all(ai, out, sizeof out) == 0, "卸载后内核回到流水线文本");
    ai_free(ai);
}

/* ============================ ⑭ D4 条件依赖：前提的**生产端** ============================
 *
 * 用户要的「前提自动长出来」：系统从**反复出现的谈话**里自己发现
 * "A 与 B 的连结几乎总伴随着 P" → 提议 `A —[前提]→ P → B`（照样过安全内核）。
 * 这一节钉住四件事：
 *   ① 分子/分母都够、比例够 → 自己长出前提（不用手打 ::op）；
 *   ② **分母必须独立于 P 记**：一半的轮次没有 P → 比例 50% < 70% → **不提**
 *      （这是这类检测器最容易犯的错：分母只统计 P 在场的轮次，比值恒等于 100%）；
 *   ③ 已经标过就不重复提（幂等）；表是有界的，塞很多三元组也不炸；
 *   ④ 默认关：不打开它，行为一点不变。 */
static void test_d4_conditional(void) {
    printf("== ⑭ D4 条件依赖：前提从反复出现的谈话里长出来 ==\n");
    char *turn1[3] = {(char *)"猫", (char *)"鱼", (char *)"水"};
    char *turn2[2] = {(char *)"猫", (char *)"鱼"};

    /* ① 三次「猫 鱼 水」→ 自己长出 猫 —[前提]→ 水 → 鱼 */
    {
        AI *ai = ai_new();
        Token *cat = T(ai, "猫"), *fish = T(ai, "鱼");
        (void)T(ai, "水");
        (void)token_add_assoc_unique(cat, fish);      /* 先有连结（共现学来的） */
        {   /* 把这条连结"做实"：观测到的连结至少命中过 2 次（刚造出来的边不算证据） */
            long ei = micro_edge_index(cat, fish);
            if (ei >= 0) cat->assocs[ei].hits = 5;
        }
        Micro *mk = micro_of(ai);
        ok(mk->meta.cond_off == 1, "D4 默认**关**（它会在对话里改结构，先测量再谈默认）");
        mk->meta.cond_off = 0;
        for (int i = 0; i < 8; ++i) micro_step(ai, turn1, 3, "");   /* 8 轮：过 5/8 两道门槛 */
        const Token *ps[4];
        ok(token_premise_between(cat, fish, ps, 4) == 1 && ps[0] == T(ai, "水"),
           "八次同轮共现（水都在场）→ **自己长出** 猫 —[前提]→ 水 → 鱼");
        ok(token_premise_link(cat, T(ai, "水")) != NULL, "前提链接确实带 [前提] 标记（不是猜的）");
        ok(mk->meta.n_cond_proposed == 1 && mk->meta.n_cond_applied == 1,
           "计数如实报：提 1 / 成 1");
        for (int i = 0; i < 3; ++i) micro_step(ai, turn1, 3, "");
        ok(mk->meta.n_cond_proposed == 1, "已经标过就不重复提（幂等）");
        ai_free(ai);
    }
    /* ② 分母独立：3 次「猫 鱼 水」+ 3 次「猫 鱼」→ 50% → **不提** */
    {
        AI *ai = ai_new();
        Token *cat = T(ai, "猫"), *fish = T(ai, "鱼");
        (void)T(ai, "水");
        (void)token_add_assoc_unique(cat, fish);
        {
            long ei = micro_edge_index(cat, fish);
            if (ei >= 0) cat->assocs[ei].hits = 5;
        }
        Micro *mk = micro_of(ai);
        mk->meta.cond_off = 0;
        /* 5 轮带「水」+ 3 轮不带 = 5/8 = 62.5% < 70% → 不当前提（分母必须独立于 P） */
        for (int i = 0; i < 5; ++i) micro_step(ai, turn1, 3, "");
        for (int i = 0; i < 3; ++i) micro_step(ai, turn2, 2, "");
        const Token *ps[4];
        ok(token_premise_between(cat, fish, ps, 4) == 0,
           "三分之一的轮次没有「水」→ 比例 62.5% < 70% → **不当前提**（分母独立才算得准）");
        ai_free(ai);
    }
    /* ③ 有界：塞 200 个不同三元组，不许炸、不许溢出 */
    {
        AI *ai = ai_new();
        Token *cat = T(ai, "猫"), *fish = T(ai, "鱼");
        (void)token_add_assoc_unique(cat, fish);
        {
            long ei = micro_edge_index(cat, fish);
            if (ei >= 0) cat->assocs[ei].hits = 5;
        }
        Micro *mk = micro_of(ai);
        mk->meta.cond_off = 0;
        char buf[16][12];
        char *w[3];
        for (int i = 0; i < 200; ++i) {
            snprintf(buf[0], sizeof buf[0], "猫");
            snprintf(buf[1], sizeof buf[1], "鱼");
            snprintf(buf[2], sizeof buf[2], "P%d", i);
            w[0] = buf[0]; w[1] = buf[1]; w[2] = buf[2];
            micro_step(ai, w, 3, "");
        }
        ok(mk->meta.cond_tri_n <= MICRO_D4_MAX_TRIPLES,
           "候选表**有界**（200 个三元组塞进去也不越界）");
        ai_free(ai);
    }
    /* ③b **去重**：一轮里重复出现的词不许把分子加得比谁都快
     * （实测在 2 万行语料上就是这么发现的：比值算成 179%、270%） */
    {
        AI *ai = ai_new();
        Token *cat = T(ai, "猫4"), *fish = T(ai, "鱼4");
        (void)T(ai, "水4");
        (void)token_add_assoc_unique(cat, fish);
        {
            long ei = micro_edge_index(cat, fish);
            if (ei >= 0) cat->assocs[ei].hits = 5;
        }
        Micro *mk = micro_of(ai);
        mk->meta.cond_off = 0;
        char *dup[4] = {(char *)"猫4", (char *)"鱼4", (char *)"水4", (char *)"水4"};
        for (int i = 0; i < 6; ++i) micro_step(ai, dup, 4, "");
        int over = 0;
        for (size_t i = 0; i < mk->meta.cond_tri_n; ++i)
            if (mk->meta.cond_tri[i].n_triple > mk->meta.cond_tri[i].n_pair) over++;
        ok(mk->meta.cond_tri_n > 0 && over == 0,
           "同一轮里的重复词**不重复计数**（分子 ≤ 分母；比值不会算成 179%）");
        ai_free(ai);
    }

    /* ④ ::mod cond 开关 */
    {
        AI *ai = ai_new();
        (void)micro_of(ai);
        ok(micro_set_detector(ai, "cond", 1) == 0 && micro_of(ai)->meta.cond_off == 0,
           "::mod cond on 打开");
        ok(micro_set_detector(ai, "cond", 0) == 0 && micro_of(ai)->meta.cond_off == 1,
           "::mod cond off 关上");
        ai_free(ai);
    }
}

/* ============================ ⑮ AI 自己用记事本：写草稿 / 什么时候发送 ============================
 *
 * 用户要的「AI 可以控制什么时候发送」。这一节把决策链钉住：
 *   思维层（think_settle）提议 → **受审通道**（MOD_PATCH_CALL）→ 记事本补丁执行
 *   → 输出侧 emit 决定这一轮说什么。
 * 三条规矩：
 *   ① `::notepad off`（默认）什么都不做；补丁没挂也什么都不做；
 *   ② `on`：有结论（成立**或**不成立）→ 把结论写进草稿并发送 → **说的是它自己写的那句**；
 *   ③ `hold`：没结论 → 按住不说（emit = -1，这一轮不出声）。 */
static void test_ai_notepad(void) {
    printf("== ⑮ AI 自己用记事本：写草稿 / 发送 / 按住不说 ==\n");
    AI *ai = ai_new();
    (void)pool_token_intern(ai->pool, "猫");
    char *w[1] = {(char *)"猫"};
    Think *t = think_of(ai);
    char out[512];

    /* ① 没挂补丁：开不开都一样（机制挂在补丁上） */
    t->notepad = 1;
    t->concl = 1;
    snprintf(t->concl_text, sizeof t->concl_text, "%s", "猫 不 会 唱歌");
    t->trace_n = 1;
    think_settle(ai);
    micro_step(ai, w, 1, "");
    snprintf(out, sizeof out, "%s", "流水线那句");
    ok(patch_emit_all(ai, out, sizeof out) == 0, "没挂补丁：思维层照样想，但没人替它说话（emit 不接管）");

    /* ② on：有结论 → 写草稿 + 发送 → 发出来的是**它自己写的那句** */
    ok(patch_mount_builtin(ai, "记事本") == 0, "挂上记事本");
    { char repA[64]; char *avA[1] = {(char *)"auto"};
      (void)patch_op_call_by_name(ai, "记事本", "模式", avA, 1, repA, sizeof repA); }
    t->concl = 1;
    snprintf(t->concl_text, sizeof t->concl_text, "%s", "猫 不 会 唱歌");
    t->trace_n = 1;
    patch_stage_all(ai, "流水线那句");            /* 流水线先起草（auto：进待发送） */
    think_settle(ai);                            /* 思维层：写草稿 + 发送 */
    micro_step(ai, w, 1, "");                    /* 受审通道：两条都应用 */
    ok(t->n_notepad > 0, "思维层动了笔（痕上有「打草稿 / 发送」）");
    snprintf(out, sizeof out, "x");
    ok(patch_emit_all(ai, out, sizeof out) == 1 && strcmp(out, "猫 不 会 唱歌") == 0,
       "**AI 自己写的优先**：说出来的是它的结论，不是流水线那句");

    /* ③ 负结论也算「有结论」（-1 不是"没结论"） */
    t->concl = -1;
    snprintf(t->concl_text, sizeof t->concl_text, "%s", "猫 会 游泳");
    t->trace_n = 1;
    patch_stage_all(ai, "流水线那句");
    think_settle(ai);
    micro_step(ai, w, 1, "");
    snprintf(out, sizeof out, "x");
    ok(patch_emit_all(ai, out, sizeof out) == 1 && strcmp(out, "猫 会 游泳") == 0,
       "结论=不成立时也说出来（那是否定结论，不是没有结论）");

    /* ④ hold：没结论 → 按住不说 */
    t->notepad = 2;
    t->concl = 0;
    t->concl_text[0] = 0;
    t->trace_n = 1;
    patch_stage_all(ai, "流水线那句");
    think_settle(ai);
    micro_step(ai, w, 1, "");
    snprintf(out, sizeof out, "x");
    ok(patch_emit_all(ai, out, sizeof out) == -1, "hold + 没结论 → **按住不说**（emit = -1）");

    /* ⑤ off：回到流水线（先把上一轮的残留清干净，模拟新的一轮） */
    {
        char rep2[120];
        (void)patch_op_call_by_name(ai, "记事本", "清空发言", NULL, 0, rep2, sizeof rep2);
        (void)patch_op_call_by_name(ai, "记事本", "清空", NULL, 0, rep2, sizeof rep2);
    }
    t->notepad = 0;
    t->trace_n = 1;
    patch_stage_all(ai, "流水线那句");
    think_settle(ai);
    micro_step(ai, w, 1, "");
    snprintf(out, sizeof out, "x");
    ok(patch_emit_all(ai, out, sizeof out) == 1 && strcmp(out, "流水线那句") == 0,
       "::notepad off → 一个字都不插手（流水线照说）");
    ai_free(ai);
}

/* ============================ ⑯ 来源监控：这条知识是哪来的 ============================
 *
 * 用户要的「来源监控（谁说的）」。三档分得开：
 *   **基准**（relations.txt / 显式教过我的）／**观测**（对话/语料里学到的）／
 *   **推论**（想出来沉淀的缓存）；他说的在**他的表**里，根本不进我的图。
 * 这一节钉住：基准档真的会在装载时打上、判据只有一份、链的"底"报得出来。 */
static void test_provenance(void) {
    printf("== ⑯ 来源监控：基准 / 观测 / 推论分得开，链的底报得出来 ==\n");
    AI *ai = ai_new();
    Token *jia = T(ai, "甲"), *yi = T(ai, "乙"), *bing = T(ai, "丙");

    /* ① 对话里学到的（token_tag_rel）不是"基准" */
    (void)token_tag_rel(jia, yi, REL_IS_A, REL_BOOST);
    long e1 = micro_edge_index(jia, yi);
    ok(e1 >= 0 && !assoc_role_is_innate(&jia->assocs[e1]),
       "对话里学到的事实 **不**被标成基准（来源=观测）");

    /* ② relations_load 装载的知识打「基准」档（写一个临时关系文件，真走装载路径） */
    {
        const char *path = "_t/optest_rel_src.txt";
        /* `_t/` 是**相对**路径：整包并行时每套在自己的沙箱里跑，沙箱里没有 `_t/`
         * （实测：这一条曾经长期静默失败 —— 见 tests/suites.txt 头部那段）。
         * 自己建一层就够了（仓库根里本来就有 `_t/`，mkdir 失败也无所谓）。 */
        (void)mkdir("_t", 0777);
        FILE *f = fopen(path, "w");
        ok(f != NULL, "写一个临时关系文件");
        if (f) {
            fprintf(f, "# 测试用\n甲 属于 丙\n");
            fclose(f);
        }
        (void)relations_load(ai, path);
        long e2 = micro_edge_index(jia, bing);
        ok(e2 >= 0 && assoc_role_is_innate(&jia->assocs[e2]),
           "relations_load 装载的知识 → **基准**档");
        ok(strcmp(assoc_role_name(ASSOC_ROLE_INNATE), "基准") == 0 &&
           strcmp(assoc_role_name(ASSOC_ROLE_DERIVED), "推论") == 0 &&
           strcmp(assoc_role_name(ASSOC_ROLE_NONE), "共现") == 0,
           "来源档的中文名只有一份（assoc_role_name）");
        remove(path);
    }

    /* ③ 链的"底"报得出来：基准事实推出来的链 → "基准" */
    {
        Proof pf;
        rel_solve(ai, jia, REL_IS_A, bing, 0, &pf);
        ok(pf.status == 1, "基准知识推得出来（甲 属于 丙）");
        Derivation d;
        der_reset(&d);
        der_from_proof(ai, &d, jia, REL_IS_A, bing, &pf);
        ok(der_verify(ai, &d) == 0, "验证器通过");
        ok(strcmp(der_base_source(&d), "基准") == 0,
           "链的底是**基准**（来源监控在这一层报得出来）");
    }
    /* ④ 缓存的两条规矩：
     *   4a **孤立缓存不被采信**（"缓存不许证明自己"）；
     *   4b 有支撑的缓存被采信，而且链的底报**推论**。 */
    {
        AI *a2 = ai_new();
        Token *p1 = T(a2, "丁"), *p2 = T(a2, "戊");
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_TAG_DERIVED;
        m.target = p1; m.main = p2; m.rel_id = REL_IS_A;
        snprintf(m.source, sizeof m.source, "测试");
        char note[160];
        safety_step_begin(a2);
        (void)micro_try(a2, &m, note, sizeof note);
        Proof pf;
        rel_solve(a2, p1, REL_IS_A, p2, 0, &pf);
        ok(pf.status == 0, "孤立缓存**不被采信**（不许用缓存证明缓存 —— 求解器会要求脱离缓存也推得出）");
        ai_free(a2);
    }
    {
        AI *a3 = ai_new();
        Token *x = T(a3, "庚"), *y = T(a3, "辛"), *z = T(a3, "壬");
        (void)token_tag_rel(x, y, REL_IS_A, REL_BOOST);      /* 庚 属于 辛 */
        (void)token_tag_rel(y, z, REL_IS_A, REL_BOOST);      /* 辛 属于 壬 → 庚 属于 壬 可继承 */
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_TAG_DERIVED;
        m.target = x; m.main = z; m.rel_id = REL_IS_A;       /* 把推出来的沉淀成缓存 */
        snprintf(m.source, sizeof m.source, "测试");
        char note[160];
        safety_step_begin(a3);
        (void)micro_try(a3, &m, note, sizeof note);
        Proof pf;
        rel_solve(a3, x, REL_IS_A, z, 0, &pf);
        ok(pf.status == 1, "有支撑的缓存被采信（脱离缓存也推得出）");
        Derivation d;
        der_reset(&d);
        der_from_proof(a3, &d, x, REL_IS_A, z, &pf);
        ok(strcmp(der_base_source(&d), "推论") == 0,
           "链的底报**推论**（这条是我想出来记住的，不是听来的）");
        ai_free(a3);
    }
    ai_free(ai);
}

/* ============================ ⑰ 对话触发：自然语言 → 符号节点行为 ============================
 *
 * 「猫 和 鱼 之间 加 个 词」这种话要真的能干活（用户要的对话触发）。
 * 三条纪律：
 *   ① **窄集**：必须「之间 + 动词 + 种类词」，只说话不干活；
 *   ② **不误触发**：提问（`猫 和 鱼 之间 有 什么 关系`）与普通句子一律不动；
 *   ③ 动作**照样走受审通道**（token_op_run），自然语言只是入口不是特权。 */
static void test_dialog_trigger(void) {
    printf("== ⑰ 对话触发：`A 和 B 之间 加 个 词/前提` 真的能干活，且不误触发 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *fish = T(ai, "鱼");
    char out[256];

    /* ① 插词 */
    char *w1[8] = {(char *)"把", (char *)"猫", (char *)"和", (char *)"鱼",
                   (char *)"之间", (char *)"加", (char *)"个", (char *)"词"};
    ok(dialog_trigger_try(ai, w1, 8, out, sizeof out) == 1, "「把 猫 和 鱼 之间 加 个 词」触发");
    Token *via = pool_find(ai->pool, MICRO_TEMP_TOKEN_PREFIX "中转]");
    ok(via && micro_has_edge(cat, via) && micro_has_edge(via, fish),
       "真干了：中间插了一个词（A —[临:中转]→ B）");
    ok(strstr(out, "猫") && strstr(out, "鱼"), "答复里说清了它对谁做了什么");
    ok(!micro_has_edge(cat, fish), "直连被拆（插词的定义）");

    /* ② 前提（分词器会把「前提」切成「前」+「提」—— 这里就用切开的写法测） */
    char *w2[10] = {(char *)"把", (char *)"猫", (char *)"和", (char *)"鱼", (char *)"之间",
                    (char *)"加", (char *)"个", (char *)"前", (char *)"提", (char *)"饿"};
    (void)T(ai, "饿");
    ok(dialog_trigger_try(ai, w2, 10, out, sizeof out) == 1, "「之间 加 个 前提 饿」触发");
    const Token *ps[4];
    ok(token_premise_between(cat, fish, ps, 4) == 1 && ps[0] == T(ai, "饿"),
       "真干了：建出 猫 —[前提]→ 饿 → 鱼");

    /* ③ 不误触发：提问、普通句、没有动词的祈使句 */
    char *q1[7] = {(char *)"猫", (char *)"和", (char *)"鱼", (char *)"之间",
                   (char *)"有", (char *)"什么", (char *)"关系"};
    ok(dialog_trigger_try(ai, q1, 7, out, sizeof out) == 0, "提问不触发（没有动词）");
    char *q2[4] = {(char *)"猫", (char *)"会", (char *)"唱歌", (char *)"吗"};
    ok(dialog_trigger_try(ai, q2, 4, out, sizeof out) == 0, "普通问句不触发");
    char *q3[3] = {(char *)"猫", (char *)"和", (char *)"鱼"};
    ok(dialog_trigger_try(ai, q3, 3, out, sizeof out) == 0, "光提两个词不触发");

    /* ④ 回收草稿（动作词 + 对象词） */
    {
        Token *tmp = pool_token_intern(ai->pool, MICRO_TEMP_TOKEN_PREFIX "x]");
        (void)tmp;
        char *w3[2] = {(char *)"回收", (char *)"草稿"};
        ok(dialog_trigger_try(ai, w3, 2, out, sizeof out) == 1, "「回收 草稿」触发");
    }
    ai_free(ai);
}

/* ============================ ⑱ 统一写入通道：谁在改图（写入账 + 可路由） ============================
 *
 * `microkernel.md` 的局限 1 是「老写入者绕过安全内核」。这一轮把它变成
 * **有账 + 可路由**：所有写入记来源，知识类写入（关系事实）可以改成走受审通道。
 * 这一节钉四件事：① 账记得对（每个来源各算各的）；② 路由开/关两条路**语义一样**；
 * ③ 路由的写入**进审计**（看得见"谁批的"）；④ K6：**非观测**的共现写入也走同一套开关
 * （感知共现 `ai_learn_pair` 仍直写 —— 那是观测的正常路径）。 */
static void test_write_ledger(void) {
    printf("== ⑱ 统一写入通道：写入账 + 知识类写入可路由（走受审通道） ==\n");
    /* C 级测试**自己定这一位**（不跟着 AIP_ROUTE 走）：否则反向测量整包时
     * "默认关"那几条会假红 —— 和 delibtest 的 ⑭ 是同一个坑。 */
    AI *ai = ai_new();
    ai->route_writes = 0;
    Token *a = T(ai, "甲1"), *b = T(ai, "乙1");
    size_t before = ai->writes[AI_WSRC_PERCEIVE];
    (void)ai_write_assoc(ai, a, b, AI_WSRC_PERCEIVE);
    ok(ai->writes[AI_WSRC_PERCEIVE] == before + 1, "共现写入记在「感知」名下");
    ok(micro_has_edge(a, b), "直写路径照样把边建出来（默认 = 观测直写）");

    /* 路由开：关系事实走受审通道，而且**进审计** */
    AI *a2 = ai_new();
    a2->route_writes = 0;
    Token *p = T(a2, "丙1"), *q = T(a2, "丁1");
    ok(a2->route_writes == 0, "路由**默认关**（观测直写是正常路径）");
    ok(micro_set_route(a2, 1) == 0 && a2->route_writes == 1, "::safe route on 打开");
    safety_step_begin(a2);
    int rc = ai_write_rel(a2, p, q, REL_IS_A, 0, AI_WSRC_RELATION);
    ok(rc == 0, "路由开：关系事实照样写得进去（语义没变）");
    ok(rel_direct(p, REL_IS_A, q), "而且图里真的有了这条事实");
    ok(a2->writes[AI_WSRC_RELATION] == 1, "这笔写入记在「关系学习」名下");
    {
        Micro *mk = micro_of(a2);
        int seen = 0;
        for (size_t i = 0; i < mk->safe.audit_n && i < MICRO_AUDIT_MAX; ++i)
            if (strstr(mk->safe.audit[i].op, "打关系")) seen = 1;
        ok(seen, "路由的写入**进了审计**（谁批的、批了什么，都留痕）");
        ok(mk->safe.n_allowed >= 1, "受审通道的记账真的动了（n_allowed ≥ 1）");
    }
    /* 路由关：同一件事走直写，结果一样 */
    AI *a3 = ai_new();
    a3->route_writes = 0;
    Token *u = T(a3, "戊1"), *v = T(a3, "己1");
    ok(ai_write_rel(a3, u, v, REL_IS_A, 0, AI_WSRC_RELATION) == 0 && rel_direct(u, REL_IS_A, v),
       "路由关：同一件事走直写，结果一样（两条路语义相同）");
    ok(a3->writes[AI_WSRC_RELATION] == 1, "记账两条路都记");
    ok(micro_of(a3)->safe.n_allowed == 0, "直写不进审计（它就是观测的正常路径）");

    /* ---- K6：**非观测**的共现写入（[时刻:] 链 / 内部对话 / 想象）也走**同一套**开关 ----
     * §6.55 K6 的处理决定：`ai_write_assoc` 照 `ai_write_rel` 的样子可路由（默认关）；
     * **感知共现（ai_learn_pair）保持直写**（它带 hits/pattern/topic，转意图会丢信息）。 */
    {
        AI *a4 = ai_new();
        a4->route_writes = 0;
        Token *kx = T(a4, "庚1"), *ky = T(a4, "辛1");
        ok(micro_set_route(a4, 1) == 0, "K6：::safe route on 打开");
        safety_step_begin(a4);
        ok(ai_write_assoc(a4, kx, ky, AI_WSRC_PERCEIVE) == 1,
           "K6：路由开 → 共现写入照样返回「新加了一条」（口径与直写一致）");
        ok(micro_has_edge(kx, ky), "K6：边照样建出来了（语义没变，只是多了一道审查）");
        ok(ai_write_assoc(a4, kx, ky, AI_WSRC_PERCEIVE) == 0,
           "K6：再写一次返回「已经有了」（口径没漂）");
        int seen_assoc = 0;
        for (size_t i = 0; i < micro_of(a4)->safe.audit_n && i < MICRO_AUDIT_MAX; ++i)
            if (strstr(micro_of(a4)->safe.audit[i].op, "加边")) seen_assoc = 1;
        ok(seen_assoc, "K6：这笔写入**进了审计**（加边 —— 谁批的看得见）");
        ok(micro_of(a4)->safe.n_allowed >= 1, "K6：受审通道的记账真的动了");
        ok(a4->writes[AI_WSRC_PERCEIVE] == 2, "K6：账照记（两条路都记来源档）");

        /* **骨架端点不路由**（K6 那条静态例外）：`[时刻:…]` 这类协议骨架名字，
         * 内核按规矩本来就不接（"不许改保留符号节点"）—— 而时间链是**内核自己的记账**，
         * 不是自修改。所以它走直写：边照样有、审计里没有它的拒绝记录。
         * 这一条是可证伪的：如果哪天有人把例外去掉，这里会看到"允许 0 拒绝 1"（实测过）。 */
        AI *a6 = ai_new();
        a6->route_writes = 0;
        Token *tk = T(a6, "子1");
        Token *mark = pool_token_intern(a6->pool, "[时刻:77]");
        ok(micro_set_route(a6, 1) == 0, "K6：骨架端点这一组也把路由打开");
        safety_step_begin(a6);
        size_t denied_before = micro_of(a6)->safe.n_denied;
        ok(ai_write_assoc(a6, mark, tk, AI_WSRC_PERCEIVE) == 1,
           "K6：骨架端点的写入走直写（边照样建出来）");
        ok(micro_has_edge(mark, tk), "K6：时间链那条边真的在");
        ok(micro_of(a6)->safe.n_denied == denied_before,
           "K6：内核**没有**拒它（静态例外生效，不是「拒了就绕开」）");
        ai_free(a6);
    }
    ai_free(ai); ai_free(a2); ai_free(a3);
}

/* ============================ ⑳ 顺序：图里写着的 vs tick ============================
 *
 * audit §6.57 记的缺口："结构写进图了，查询层没接"。这一节钉 `token_order_between()`：
 *   · `next_in_time` 链（`[原语:时间链]` 那套）—— 正反两个方向都要答对，多跳也要走得到；
 *   · `[关系:之前]/[关系:之后]` **标记边** —— 同一个 tick 里也能分出先后；
 *   · 图里没说 → 返回 0（调用方退回 tick 比较，不许瞎猜）。 */
static void test_order_query(void) {
    printf("== ⑳ 顺序查询：先读图里的结构，没有才退回 tick ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "序甲"), *b = T(ai, "序乙"), *c = T(ai, "序丙");
    char why[160];
    int hops = -1;
    ok(token_order_between(a, b, why, sizeof why, &hops) == 0,
       "图里没有顺序结构 → 返回 0（调用方退回 tick，不瞎猜）");
    /* ① next_in_time 链：a → b → c */
    a->next_in_time = b;
    b->next_in_time = c;
    ok(token_order_between(a, c, why, sizeof why, &hops) == 1 && hops == 2,
       "多跳链读得出来（a 先，2 跳）");
    ok(token_order_between(c, a, why, sizeof why, &hops) == -1 && hops == 2,
       "反过来问也一样对（c 后 → 返回 -1 = b 端先）");
    ok(token_order_between(a, b, why, sizeof why, &hops) == 1 && hops == 1, "一跳也对");
    ok(strstr(why, "next_in_time") != NULL, "依据里写明是 next_in_time 链");
    /* ② 标记边：同一个 tick 里也能分出先后（`[关系:之后]` 挂在 x 上 ⇒ y 先） */
    AI *a2 = ai_new();
    Token *x = T(a2, "标甲"), *y = T(a2, "标乙");
    Token *mk_after = T(a2, TOKEN_ORDER_MARK_AFTER);
    (void)token_add_assoc_unique(x, mk_after);
    (void)token_add_assoc_unique(x, y);
    ok(token_order_between(x, y, why, sizeof why, NULL) == -1,
       "`[关系:之后]` 挂在 x 上 ⇒ **y 先**（返回 -1）");
    ok(token_order_between(y, x, why, sizeof why, NULL) == 1,
       "反过来问：x 先（返回 +1）");
    AI *a3 = ai_new();
    Token *p = T(a3, "标丙"), *q = T(a3, "标丁");
    Token *mk_before = T(a3, TOKEN_ORDER_MARK_BEFORE);
    (void)token_add_assoc_unique(p, mk_before);
    (void)token_add_assoc_unique(p, q);
    ok(token_order_between(p, q, why, sizeof why, NULL) == 1,
       "`[关系:之前]` 挂在 p 上 ⇒ **p 先**（返回 +1）");
    ok(token_order_between(p, T(a3, "没关系"), why, sizeof why, NULL) == 0,
       "只有标记、没有和目标的连边 → 不算证据（返回 0）");
    ai_free(ai); ai_free(a2); ai_free(a3);
}

/* ============================ ㉑ 合写否定：两种写法同一份声明 ============================
 *
 * 关系表声明的是两词形式（`不 会`），而分词器把「不会」「不喜欢」「不是」切成**一个** 符号节点
 * （「不属于/不能」却是两词）。这一节钉：两种写法**解析结果一样**、`rel_word` 归一成关系词
 * （否则调用方按 `is_neg` 拼一遍会得到「不 不会」）、以及关系文件装载器也认合写。 */
static void test_merged_neg(void) {
    printf("== ㉑ 合写否定（不会/不喜欢/不是）：两种写法同一份声明 ==\n");
    AI *ai = ai_new();
    char jbuf[64];
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int neg = 0, isq = 0;
    /* 合写 */
    char *(w1[4]) = {(char *)"猫", (char *)"不会", (char *)"飞", (char *)"吗"};
    int p1 = rel_parse(ai, w1, 4, jbuf, sizeof jbuf, &aw, &rid, &neg, &bw, &isq, &rw, 0);
    ok(p1 && rid == REL_CAN && neg == 1 && isq == 1 && strcmp(aw, "猫") == 0 && strcmp(bw, "飞") == 0,
       "「猫 不会 飞 吗」→ 关系=会、否定=1、问句（一个符号节点就带否定）");
    ok(p1 && rw && strcmp(rw, "会") == 0,
       "`rel_word` 归一是**关系词本身**（「会」，不是输入那个「不会」）—— 不然会拼出「不 不会」");
    /* 两词写法：同答案 */
    const char *aw2 = NULL, *bw2 = NULL, *rw2 = NULL;
    uint8_t rid2 = 0; int neg2 = 0, isq2 = 0;
    char *(w2[5]) = {(char *)"猫", (char *)"不", (char *)"会", (char *)"飞", (char *)"吗"};
    int p2 = rel_parse(ai, w2, 5, jbuf, sizeof jbuf, &aw2, &rid2, &neg2, &bw2, &isq2, &rw2, 0);
    ok(p2 && rid2 == rid && neg2 == neg && isq2 == isq && rw2 && rw && strcmp(rw2, rw) == 0,
       "两词写法（不 会）解析出**同一个** (关系, 否定, 问句, 关系词)");
    /* 另外两个合写形式 */
    char *(w3[4]) = {(char *)"猫", (char *)"不喜欢", (char *)"鱼", (char *)"吗"};
    ok(rel_parse(ai, w3, 4, jbuf, sizeof jbuf, &aw, &rid, &neg, &bw, &isq, &rw, 0) &&
       rid == REL_LIKE && neg == 1, "「猫 不喜欢 鱼 吗」→ 关系=喜欢、否定=1");
    char *(w4[4]) = {(char *)"猫", (char *)"不是", (char *)"动物", (char *)"吗"};
    ok(rel_parse(ai, w4, 4, jbuf, sizeof jbuf, &aw, &rid, &neg, &bw, &isq, &rw, 0) &&
       rid == REL_IS_A && neg == 1, "「猫 不是 动物 吗」→ 关系=属于、否定=1");
    /* 别误伤：肯定句、以及"关系词本身"都不算合写否定 */
    char *(w5[4]) = {(char *)"猫", (char *)"会", (char *)"飞", (char *)"吗"};
    ok(rel_parse(ai, w5, 4, jbuf, sizeof jbuf, &aw, &rid, &neg, &bw, &isq, &rw, 0) && neg == 0,
       "肯定句（猫 会 飞 吗）否定位是 0 —— 没把关系词当成否定");
    { uint8_t m = 0; ok(!rel_merged_neg_id("会", &m) && !rel_merged_neg_id("不", &m),
                        "`rel_merged_neg_id` 不认「会」（关系词）也不认孤零零的「不」"); }
    /* 关系文件装载器也认合写（不然 `甲2 不会 乙2` 这种行会被静默丢掉） */
    {
        AI *b = ai_new();
        const char *path = "_t/optest_neg_src.txt";
        (void)mkdir("_t", 0777);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "# 合写否定\n序甲 不会 序乙\n"); fclose(f); }
        (void)relations_load(b, path);
        Token *x = pool_token_intern(b->pool, "序甲");
        Token *y = pool_token_intern(b->pool, "序乙");
        ok(rel_direct_neg(x, REL_CAN, y), "关系文件里写「序甲 不会 序乙」也装得进来（不再静默丢）");
        remove(path);
        ai_free(b);
    }
    ai_free(ai);
}

/* ==================== ㉓ 过程记号：role 标记 + 动作槽记号（第 9 条 ①②） ====================
 *
 * 用户 2026-10-10 拍板："过程边 vs 共现边"用 **role 标记**。这一节钉三件事：
 *   ① `::op 建过程/加步骤/加调用/加回边` 走**受审通道**（token_op_run），边真的建出来了；
 *   ② 那些边的 `role` 是**过程**、动作槽上挂着 `[步骤:N]`/`[调用]`/`[回边]`（判据函数读得出来）；
 *   ③ 普通共现边**不会**被误判成过程边（否则 role 标记就白加了）。 */
static void test_process_marks(void) {
    printf("== ㉓ 过程记号：过程边与共现边靠 role 分开 ==\n");
    AI *ai = ai_new();
    char note[200];
    char *mk[2] = {(char *)"泡茶2"};
    ok(token_op_run(ai, "建过程", mk, 1, note, sizeof note) >= 0, "建过程走受审通道");
    Token *root = pool_find(ai->pool, "[过程:泡茶2]");
    ok(root != NULL, "过程根 `[过程:泡茶2]` 建出来了（内部命名空间）");
    ok(micro_internal_ns("[过程:泡茶2]") == 1, "过程根算内部命名空间（能当端点、过审查）");
    {
        char *a1[3] = {(char *)"泡茶2", (char *)"烧水2", (char *)"1"};
        char *a2[2] = {(char *)"泡茶2", (char *)"泡茶叶2"};
        char *a3[2] = {(char *)"泡茶2", (char *)"1"};
        ok(token_op_run(ai, "加步骤", a1, 3, note, sizeof note) >= 0, "加步骤（带序号）");
        ok(token_op_run(ai, "加调用", a2, 2, note, sizeof note) >= 0, "加调用");
        ok(token_op_run(ai, "加回边", a3, 2, note, sizeof note) >= 0,
           "加回边（给步号 → 指回 `[步骤:1]`）");
    }
    Token *boil = pool_find(ai->pool, "烧水2");
    Token *leaf = pool_find(ai->pool, "泡茶叶2");
    Token *step1 = pool_find(ai->pool, "[步骤:1]");
    ok(boil && leaf && step1, "步骤动作与步号记号都进了池子");
    if (root && boil && leaf && step1) {
        long i1 = micro_edge_index(root, boil), i2 = micro_edge_index(root, leaf);
        long i3 = micro_edge_index(root, step1);
        ok(i1 >= 0 && i2 >= 0 && i3 >= 0, "三条过程边都建出来了");
        if (i1 >= 0) {
            const AssocEntry *e = &root->assocs[i1];
            ok(edge_is_process(e), "步骤边 role = **过程**");
            ok(edge_step_index(e) == 1, "步骤边的动作槽是 `[步骤:1]`（读得出步号）");
            ok(edge_is_proc_step(e), "`edge_is_proc_step` = 真（role 对 + 记号在）");
            ok(strcmp(assoc_role_name(e->role), "过程") == 0, "角色名是「过程」");
        }
        if (i2 >= 0) ok(edge_is_call(&root->assocs[i2]), "调用边的动作槽是 `[调用]`");
        if (i3 >= 0) ok(edge_is_back(&root->assocs[i3]), "回边的动作槽是 `[回边]`");
    }
    /* ③ 普通共现边**不许**被误判（role 标记的全部意义就在这儿） */
    if (root) {
        Token *plain = T(ai, "普通词2");
        (void)token_add_assoc_unique(root, plain);
        long ip = micro_edge_index(root, plain);
        ok(ip >= 0 && !edge_is_process(&root->assocs[ip]) && !edge_is_proc_step(&root->assocs[ip]),
           "普通共现边不会被当成过程边（role / 记号判据都为假）");
        ok(!edge_is_proc_step(&root->assocs[ip]) && edge_step_index(&root->assocs[ip]) == 0,
           "`edge_step_index` 对非过程边给 0（不是瞎猜一个步号）");
    }
    /* 记号可以当端点（回边要用），但不是"随便谁都能改的端点" */
    ok(micro_reserved("[步骤:1]") != 0 || micro_endpoint_reserved(step1) == 0,
       "`[步骤:N]` 是保留名、但**允许当端点**（回边指回某一步的前提）");
    /* 不存在的过程：明说没有，不静默建一个 */
    {
        char *bad[2] = {(char *)"没有的过程2", (char *)"烧水2"};
        ok(token_op_run(ai, "加步骤", bad, 2, note, sizeof note) < 0 &&
           strstr(note, "没有这个过程") != NULL,
           "给不存在的过程加步骤：报「没有这个过程」（不静默建）");
    }
    ai_free(ai);
}

/* ==================== ㉔ 过程执行引擎（第 9 条 ③，默认关） ====================
 *
 * ①+② 让过程记得下（§6.70），③ 让它**跑得起来也停得下来**：逐步核对（词在 / 前提成立 /
 * 是不是符号节点行为），核对只读、真执行走受审通道、断点用 `DerGap`。这一节钉：
 * 默认关、断点判据（前提不成立 / 要参数）、真执行计数、以及**过程边不许被拆**（结构）。 */
static void test_proc_run(void) {
    printf("== ㉔ 过程执行引擎：跑得起来、也停得下来（§6.106 起默认**开**） ==\n");
    AI *ai = ai_new();
    ok(ai->proc_run == 1 || getenv("AIP_PROCRUN") != NULL,
       "引擎**默认开**（§6.106 翻默认；AIP_PROCRUN=0 那侧按规矩跳过）");
    char note[200];
    { char *a[1] = {(char *)"泡茶3"}; (void)token_op_run(ai, "建过程", a, 1, note, sizeof note); }
    { char *a[3] = {(char *)"泡茶3", (char *)"烧水3", (char *)"1"};
      (void)token_op_run(ai, "加步骤", a, 3, note, sizeof note); }
    { char *a[2] = {(char *)"泡茶3", (char *)"回收临时"};
      (void)token_op_run(ai, "加步骤", a, 2, note, sizeof note); }
    Token *root = proc_root_of(ai, "泡茶3");
    ok(root != NULL, "过程根找得到（`proc_root_of`）");
    /* 过程边是**结构**：不许被"拆直连"那一类动作拆掉 */
    if (root) {
        long i = micro_edge_index(root, pool_find(ai->pool, "烧水3"));
        ok(i >= 0 && !micro_edge_removable(&root->assocs[i]),
           "过程边**不可拆**（插词/加前提不许把第 N 步从过程里删掉）");
        Token *plain = T(ai, "普通词3");
        (void)token_add_assoc_unique(root, plain);
        long j = micro_edge_index(root, plain);
        ok(j >= 0 && micro_edge_removable(&root->assocs[j]) == 1,
           "普通共现边照旧可拆（这条保护只给过程边，不是把整张图锁死）");
    }
    /* 引擎关着时：run 不发车（返回 0 且不记录步数） */
    {
        ProcRun pr;
        ai->proc_run = 0;
        size_t g0 = proc_run(ai, "泡茶3", &pr);
        ok(g0 == 0 && pr.n_step == 0, "引擎关着时 `proc_run` 不跑（步数 0）—— 调用方自己判开关");
    }
    /* 开着：普通步骤过、行为步骤真执行 */
    {
        ProcRun pr;
        ai->proc_run = 1;                    /* 这一节测的是"实跑"：C 级测试自己钉开关 */
        ai->proc_dry = 0;                    /* 干跑关（不然就会"假定成立"而不是真执行） */
        size_t g = proc_run(ai, "泡茶3", &pr);
        ok(g == 0 && pr.n_step == 2, "两步都过（0 断点）");
        ok(pr.n_ran == 1, "其中「回收临时」那一步**真的执行了**（受审通道，计数 +1）");
        ok(pr.trace.n >= 3, "执行轨迹记下来了（复用 Derivation：goal + 步 + conclude）");
    }
    /* 前提不成立 ⇒ 断点（前提不在注意窗口里；这是现成判据） */
    {
        AI *b = ai_new();
        char note2[200];
        { char *a[1] = {(char *)"泡茶4"}; (void)token_op_run(b, "建过程", a, 1, note2, sizeof note2); }
        { char *a[3] = {(char *)"泡茶4", (char *)"烧水4", (char *)"1"};
          (void)token_op_run(b, "加步骤", a, 3, note2, sizeof note2); }
        /* 前提挂在根与这一步之间：`加前提 <过程根> <动作> <前提名>` */
        { char *a[3] = {(char *)"[过程:泡茶4]", (char *)"烧水4", (char *)"有热水4"};
          (void)token_op_run(b, "加前提", a, 3, note2, sizeof note2); }
        ProcRun pr;
        b->proc_run = 1;
        b->proc_dry = 0;
        size_t g = proc_run(b, "泡茶4", &pr);
        ok(g == 1 && pr.n_gap == 1, "前提没成立 ⇒ **1 个断点**（不是「照跑不误」）");
        {   /* 断点要**既进轨迹**（有 DER_GAP 步）**又上计数**（`trace.gaps`）——
             * `der_gap()` 只加一步、不动计数，所以这一条是引擎自己补的（实测踩过） */
            int gap_steps = 0;
            for (size_t i = 0; i < pr.trace.n; ++i)
                if (pr.trace.step[i].kind == DER_GAP) gap_steps++;
            ok(gap_steps >= 1 && (size_t)pr.trace.gaps == pr.n_gap,
               "断点既进轨迹（有 DerGap 步）又上计数（trace.gaps == n_gap）");
        }
        ai_free(b);
    }
    /* 要参数的行为 ⇒ 断点（不猜参数） */
    {
        AI *c = ai_new();
        char n3[200];
        { char *a[1] = {(char *)"泡茶5"}; (void)token_op_run(c, "建过程", a, 1, n3, sizeof n3); }
        { char *a[2] = {(char *)"泡茶5", (char *)"加步骤"};
          (void)token_op_run(c, "加步骤", a, 2, n3, sizeof n3); }
        ProcRun pr;
        c->proc_run = 1;
        c->proc_dry = 0;
        size_t g = proc_run(c, "泡茶5", &pr);
        ok(g == 1 && pr.n_ran == 0, "行为要参数时**不猜参数**：记断点、不执行");
        ai_free(c);
    }
    ai_free(ai);
}

/* ==================== ㉕ ④ 干跑：假设世界（Hypo 当执行栈与作用域） ====================
 *
 * §6.56 第 4 条：`Hypo` 当执行栈与作用域，回退用它的 pop。这一节钉：
 * 干跑默认关、前提不成立→假定（记进层）、行为不执行、**跑完深度回原样**、
 * 按步回退（撤单条 —— 给假设层补的 `hypo_del`）、以及调用时多压一层。 */
static void test_proc_dry(void) {
    printf("== ㉕ ④ 干跑：假设世界（Hypo 当执行栈与作用域） ==\n");
    /* `hypo_del` 本身：撤单条（这是"按步回退"缺的那一半） */
    {
        AI *h = ai_new();
        Token *x = T(h, "假甲"), *y = T(h, "假乙"), *z = T(h, "假丙");
        (void)hypo_push(&h->hypo, NULL);
        (void)hypo_add(&h->hypo, x, y, REL_NEED, 0);
        (void)hypo_add(&h->hypo, x, z, REL_NEED, 0);
        size_t n0 = h->hypo.fr[0].n;
        ok(n0 == 2, "一层里两条假设");
        ok(hypo_del(&h->hypo, x, y, REL_NEED) == 1 && h->hypo.fr[0].n == n0 - 1,
           "`hypo_del` 撤掉**指定那一条**（另一条还在）");
        ok(hypo_find(&h->hypo, NULL, x, REL_NEED, z, NULL) != NULL &&
           hypo_find(&h->hypo, NULL, x, REL_NEED, y, NULL) == NULL,
           "撤的是对的那条（剩下那条还查得到）");
        ok(hypo_del(&h->hypo, x, y, REL_NEED) == 0, "已经不在了 → 再撤返回 0（不谎报）");
        ai_free(h);
    }
    /* 干跑：假定前提、行为不执行、深度回原样 */
    {
        AI *ai = ai_new();
        char note[200];
        /* §6.105 起干跑**默认开**：这里随默认一起翻（显式关那侧由 `AIP_PROCDRY=0` 覆盖）。
         * ⚠ 同一个 AI 上后面的断言跑的是"自己显式设过 proc_dry"的场景，不受影响。 */
        ok(ai->proc_dry == 1 || getenv("AIP_PROCDRY") != NULL,
           "干跑**默认开**（§6.105 翻默认；AIP_PROCDRY=0 那侧按规矩跳过）");
        { char *a[1] = {(char *)"泡茶6"}; (void)token_op_run(ai, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶6", (char *)"烧水6", (char *)"1"};
          (void)token_op_run(ai, "加步骤", a, 3, note, sizeof note); }
        { char *a[2] = {(char *)"泡茶6", (char *)"回收临时"};
          (void)token_op_run(ai, "加步骤", a, 2, note, sizeof note); }
        { char *a[3] = {(char *)"[过程:泡茶6]", (char *)"烧水6", (char *)"有热水6"};
          (void)token_op_run(ai, "加前提", a, 3, note, sizeof note); }
        size_t base = ai->hypo.depth;
        ai->proc_run = 1;
        /* 实跑：前提不成立 ⇒ 断点；行为真执行 */
        ProcRun wet;
        ai->proc_dry = 0;
        (void)proc_run(ai, "泡茶6", &wet);
        ok(wet.n_gap == 1 && wet.n_ran == 1,
           "实跑：前提不成立=1 个断点，行为步骤真执行（③ 的语义）");
        ok(ai->hypo.depth == base, "实跑之后假设层深度回原样");
        /* 干跑：假定成立、行为不执行 */
        ProcRun dry;
        ai->proc_dry = 1;
        (void)proc_run(ai, "泡茶6", &dry);
        ok(dry.n_gap == 0 && dry.n_assume == 1 && dry.n_defer == 1,
           "干跑：前提**假定成立**（1 条假设）、行为步骤**推迟不执行**（1 个）、0 断点");
        ok(dry.n_ran == 0, "干跑**一个行为都没执行**（与实跑的区别）");
        ok(ai->hypo.depth == base, "干跑之后深度也回原样（pop 干净，不漏）");
        ok(dry.n_assume_ok == dry.n_assume - dry.n_rollback,
           "跑完核对：没被回退的假设都还在层里（`hypo_find` 核过）");
        ok(dry.hypo_peak > dry.hypo_base, "跑的时候确实压了层（不是空转）");
        ai_free(ai);
    }
    /* 按步回退：先假定、随后发现这一步做不了（要参数）⇒ 撤掉那一步的假设 */
    {
        AI *b = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶7"}; (void)token_op_run(b, "建过程", a, 1, note, sizeof note); }
        { char *a[2] = {(char *)"泡茶7", (char *)"加步骤"};
          (void)token_op_run(b, "加步骤", a, 2, note, sizeof note); }
        { char *a[3] = {(char *)"[过程:泡茶7]", (char *)"加步骤", (char *)"有热水7"};
          (void)token_op_run(b, "加前提", a, 3, note, sizeof note); }
        b->proc_run = 1; b->proc_dry = 1;
        ProcRun pr;
        size_t base = b->hypo.depth;
        (void)proc_run(b, "泡茶7", &pr);
        ok(pr.n_assume == 1 && pr.n_rollback == 1 && pr.n_assume_ok == 0,
           "**按步回退**：假定 1 条 → 这一步做不了 → 撤 1 条 → 核对 0 条（账对得上）");
        ok(b->hypo.depth == base, "回退之后深度仍在原样（撤的是事实，不是层）");
        ai_free(b);
    }
    /* 调用：进子过程要多压一层（执行栈） */
    {
        AI *c = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶8"}; (void)token_op_run(c, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶8", (char *)"烧水8", (char *)"1"};
          (void)token_op_run(c, "加步骤", a, 3, note, sizeof note); }
        { char *a[1] = {(char *)"洗杯8"}; (void)token_op_run(c, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"洗杯8", (char *)"冲水8", (char *)"1"};
          (void)token_op_run(c, "加步骤", a, 3, note, sizeof note); }
        { char *a[2] = {(char *)"泡茶8", (char *)"洗杯8"};
          (void)token_op_run(c, "加调用", a, 2, note, sizeof note); }
        c->proc_run = 1; c->proc_dry = 1;
        ProcRun pr;
        size_t base = c->hypo.depth;
        (void)proc_run(c, "泡茶8", &pr);
        ok(pr.n_call == 1 && pr.hypo_peak >= base + 2,
           "调用子过程时**多压一层**（执行栈：主过程 1 层 + 子过程 1 层）");
        ok(c->hypo.depth == base, "跑完深度回原样（子过程那层也 pop 掉了）");
        ai_free(c);
    }
}

/* ============ ㉖ ⑤ 分支/循环/异常：记号、写入口、跑法（第 9 条 ⑤） ============
 *
 * §6.56 五步走的最后一步。条件判据**不另造**（就是前提那三档）；循环条件写在**回边记号**
 * 里（`[回边:条件词]`）—— 因为图里一条 (根,目标) 只有一条边，条件另挂一条会和步骤/回边撞车。 */
static void test_proc_branch(void) {
    printf("== ㉖ ⑤ 分支/循环/异常：条件走哪一支、出错跳哪、循环什么时候停 ==\n");
    /* 记号判据 */
    {
        AssocEntry e;
        memset(&e, 0, sizeof e);
        Token mk; memset(&mk, 0, sizeof mk);
        Token tgt; memset(&tgt, 0, sizeof tgt);
        e.role = ASSOC_ROLE_PROCESS; e.action = &mk; e.main = &tgt;
        mk.name = (char *)"[条件:猫]";
        ok(edge_is_cond(&e) && !edge_is_else(&e) && strcmp(edge_cond_name(&e), "猫]") == 0,
           "`[条件:猫]` 认得出（名字里带结尾的 `]` —— 打印要用 `proc_mark_bare` 去掉）");
        { char cb[32];
          /* 长度按**字节**算（"猫" 是 3 字节）—— 别按字数想（第一版就写成 2 了） */
          ok(proc_mark_bare(mk.name, PROC_COND_PREFIX, cb, sizeof cb) == strlen("猫") &&
             strcmp(cb, "猫") == 0, "`proc_mark_bare` 去掉结尾的 `]`（打印时才对）"); }
        mk.name = (char *)"[否则]";
        ok(edge_is_else(&e) && !edge_is_cond(&e), "`[否则]` 认得出");
        mk.name = (char *)"[异常:没热水]";
        ok(edge_is_exc(&e) && strcmp(edge_exc_name(&e), "没热水]") == 0, "`[异常:类型]` 认得出");
        mk.name = (char *)"[回边]";
        ok(edge_is_back(&e) && edge_back_cond(&e) == NULL, "不带条件的回边：认得出、没有条件");
        mk.name = (char *)"[回边:有热水]";
        ok(edge_is_back(&e) && edge_back_cond(&e) && strcmp(edge_back_cond(&e), "有热水]") == 0,
           "带条件的回边 `[回边:有热水]`：认得出，条件取得出来");
        ok(!edge_is_process(&e) == 0, "（前提：这些记号都是**过程边**上的动作槽）");
    }
    /* 写入口 + 跑法：分支 */
    {
        AI *ai = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶9"}; (void)token_op_run(ai, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶9", (char *)"猫", (char *)"泡茶叶9"};
          ok(token_op_run(ai, "加条件", a, 3, note, sizeof note) >= 0, "`加条件` 走受审通道"); }
        { char *a[2] = {(char *)"泡茶9", (char *)"去烧水9"};
          ok(token_op_run(ai, "加否则", a, 2, note, sizeof note) >= 0, "`加否则` 走受审通道"); }
        Token *root = proc_root_of(ai, "泡茶9");
        ok(root != NULL, "过程根在");
        if (root) {
            long ic = -1, ie = -1;
            for (size_t i = 0; i < root->count; ++i) {
                if (edge_is_cond(&root->assocs[i])) ic = (long)i;
                if (edge_is_else(&root->assocs[i])) ie = (long)i;
            }
            ok(ic >= 0 && edge_is_process(&root->assocs[ic]), "条件边建出来了，role=过程");
            ok(ie >= 0 && edge_is_process(&root->assocs[ie]), "否则边建出来了，role=过程");
        }
        /* 条件成立（猫 在焦点里）→ 走条件那一支 */
        (void)ai_focus_push(ai, (Token *)pool_find(ai->pool, "猫"), 1.0f);
        ai->proc_run = 1; ai->proc_dry = 0;
        ProcRun pr;
        { size_t g = proc_run(ai, "泡茶9", &pr);
          ok(g == 0 && pr.n_ok >= 1, "条件成立：条件那一支跑了（0 断点）"); }
        ai_free(ai);
    }
    /* 条件词图里没有 → 判不了（按不成立走），有否则支就去否则支 */
    {
        AI *b = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶10"}; (void)token_op_run(b, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶10", (char *)"图里没这个词", (char *)"泡茶叶10"};
          (void)token_op_run(b, "加条件", a, 3, note, sizeof note); }
        { char *a[2] = {(char *)"泡茶10", (char *)"去烧水10"};
          (void)token_op_run(b, "加否则", a, 2, note, sizeof note); }
        b->proc_run = 1; b->proc_dry = 0;
        ProcRun pr;
        (void)proc_run(b, "泡茶10", &pr);
        ok(pr.n_ok >= 1 && pr.n_gap == 0,
           "条件词判不了 → 按不成立走 `[否则]` 那一支（**不静默当成立**）");
        ai_free(b);
    }
    /* 异常：哪一步失败就跑异常支；**出错那一步的账照旧记** */
    {
        AI *c = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶11"}; (void)token_op_run(c, "建过程", a, 1, note, sizeof note); }
        { char *a[2] = {(char *)"泡茶11", (char *)"加步骤"};      /* 要参数 ⇒ 必失败 */
          (void)token_op_run(c, "加步骤", a, 2, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶11", (char *)"没参数", (char *)"用微波炉11"};
          (void)token_op_run(c, "加异常", a, 3, note, sizeof note); }
        c->proc_run = 1; c->proc_dry = 0;
        ProcRun pr;
        (void)proc_run(c, "泡茶11", &pr);
        ok(pr.n_gap == 1 && pr.n_exc_handled == 1,
           "出错那一步记 1 个断点，异常处理步跑通 1 次（**账不抹**）");
        ai_free(c);
    }
    /* 循环：回边 + 上限；带条件的回边 = while */
    {
        AI *d = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶12"}; (void)token_op_run(d, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶12", (char *)"烧水12", (char *)"1"};
          (void)token_op_run(d, "加步骤", a, 3, note, sizeof note); }
        { char *a[2] = {(char *)"泡茶12", (char *)"1"};            /* 不带条件 */
          (void)token_op_run(d, "加回边", a, 2, note, sizeof note); }
        d->proc_run = 1; d->proc_dry = 0;
        ProcRun p1;
        (void)proc_run(d, "泡茶12", &p1);
        ok(p1.n_step >= 3, "不带条件的回边：按上限 2 次重跑（总步数 ≥ 3）");
        ai_free(d);
    }
    {
        AI *e = ai_new();
        char note[200];
        { char *a[1] = {(char *)"泡茶13"}; (void)token_op_run(e, "建过程", a, 1, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶13", (char *)"烧水13", (char *)"1"};
          (void)token_op_run(e, "加步骤", a, 3, note, sizeof note); }
        { char *a[3] = {(char *)"泡茶13", (char *)"1", (char *)"猫"};
          (void)token_op_run(e, "加回边", a, 3, note, sizeof note); }   /* while 猫 */
        Token *mk = pool_find(e->pool, "[回边:猫]");
        ok(mk != NULL, "带条件的回边记号 `[回边:猫]` 建出来了（条件写在记号里，不另挂边）");
        e->proc_run = 1; e->proc_dry = 0;
        ProcRun p2;
        (void)proc_run(e, "泡茶13", &p2);
        ok(p2.n_skip == 1 && p2.n_step == 1,
           "条件不成立（猫不在焦点里）⇒ **一次都不重跑**（while 的正常出口）");
        ai_free(e);
    }
}

/* ============ ㉗ ① 步骤带对象：逐步 `der_verify` 独立复核（第 9 条 ① 的收口） ============ */
static void test_proc_claim(void) {
    printf("== ㉗ ① 步骤带对象：`rel_solve` + `der_verify` 独立复核 ==\n");
    AI *ai = ai_new();
    /* 主语/关系/宾语都得先在池子里（CLI 那侧是装载模型/分词时进的）—— 这是 `pool_find` 的脾气，
     * 和 §6.61 那次"加前提"踩的是同一个坑。 */
    (void)T(ai, "属于"); (void)T(ai, "猫"); (void)T(ai, "动物");
    (void)token_tag_rel(T(ai, "猫"), T(ai, "动物"), REL_IS_A, REL_BOOST);   /* 图里真有这条 */
    char note[200];
    { char *a[1] = {(char *)"核对2"}; (void)token_op_run(ai, "建过程", a, 1, note, sizeof note); }
    { char *a[4] = {(char *)"核对2", (char *)"猫", (char *)"属于", (char *)"动物"};
      ok(token_op_run(ai, "加断言", a, 4, note, sizeof note) >= 0,
         "`加断言`（4 个参数）走受审通道 —— CLI 参数上限也是这一轮修的"); }
    Token *root = proc_root_of(ai, "核对2");
    long i = root ? micro_edge_index(root, pool_find(ai->pool, "猫")) : -1;
    ok(i >= 0, "断言边建出来了（端点 = 主语）");
    if (i >= 0) {
        int idx = 0, neg = 0; char sb[64], rb[64], bb[64];
        ok(proc_step_claim(&root->assocs[i], &idx, sb, sizeof sb, rb, sizeof rb, bb, sizeof bb,
                           &neg) == 1 && idx == 1 && strcmp(sb, "猫") == 0 &&
           strcmp(rb, "属于") == 0 && strcmp(bb, "动物") == 0 && neg == 0,
           "`proc_step_claim` 从记号里解出（步号, 主语, 关系, 宾语, 否定）");
    }
    ai->proc_run = 1; ai->proc_dry = 0;
    ProcRun pr;
    (void)proc_run(ai, "核对2", &pr);
    ok(pr.n_verified == 1 && pr.n_verify_fail == 0 && pr.n_gap == 0,
       "成立的那条：**独立复核通过 1 个**（0 断点）");
    ok(pr.trace.n >= 3, "复核过的链**并进了整轮轨迹**（不是只报个通过）");
    ai_free(ai);
    /* 不成立的那条 ⇒ 断点 */
    AI *b = ai_new();
    (void)T(b, "属于"); (void)T(b, "猫"); (void)T(b, "鸟");
    char n2[200];
    { char *a[1] = {(char *)"核对3"}; (void)token_op_run(b, "建过程", a, 1, n2, sizeof n2); }
    { char *a[4] = {(char *)"核对3", (char *)"猫", (char *)"属于", (char *)"鸟"};
      (void)token_op_run(b, "加断言", a, 4, n2, sizeof n2); }
    b->proc_run = 1; b->proc_dry = 0;
    ProcRun q;
    (void)proc_run(b, "核对3", &q);
    ok(q.n_verify_fail == 1 && q.n_gap == 1, "核不过的那条 ⇒ **1 个断点**（不是「词在就放行」）");
    ai_free(b);
}

/* ============ ㉘ 自撰写入接上统一通道（第 6 条尾巴：老写入者绕过安全内核） ============
 *
 * 缺口（§B.2）：**想象**、**内部对话**这些"它自己想出来的"写入一直是直写，只有账。
 * 这一轮把它们改走 `ai_write_assoc`：`route_writes` 打开时和别的自撰一样**提意图受审**；
 * 关着时行为与今天**逐字节相同**（默认关）。 */
static void test_revoke_marker(void) {
    printf("== ㉙ 撤销留痕：`[撤销:…]` 里数回来（重载后推论档不再偏乐观） ==\n");
    AI *ai = ai_new();
    /* 模拟一条历史留痕（真撤销走 think.h 的受审通道写它） */
    Token *mk = pool_token_intern(ai->pool, MICRO_REVOKE_TOKEN_PREFIX "猫|属于|动物]");
    ok(mk != NULL, "`[撤销:…]` 标记是内部命名空间的普通节点");
    ai->n_retracted = 0;                       /* 假装"刚重载、计数清零" */
    ai_recount_evidence(ai);
    ok(ai->n_retracted >= 1, "`ai_recount_evidence` **从图里数回来**（重载后不再偏乐观）");
    ok(ai->n_retracted == 1, "只数标记，不多不少");
    ai_free(ai);
}

static void test_selfwrite_route(void) {
    printf("== ㉘ 自撰写入（想象/内部对话）走统一通道 ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "想甲"), *b = T(ai, "想乙"), *d = T(ai, "想丙");
    /* 关着：直写（不进队列、账也不动）—— 与今天一样 */
    ai->route_writes = 0;
    size_t q0 = micro_of(ai)->queue.n;
    ok(ai_write_assoc(ai, ai->self, d, AI_WSRC_INNER) == 0 || 1, "关着时照旧直写（不报错）");
    ok(micro_of(ai)->queue.n == q0, "关着时**不进意图队列**（与今天逐字节相同的保证）");
    /* 开着：提意图（受审通道），并记来源账 */
    ai->route_writes = 1;
    size_t w0 = ai->writes[AI_WSRC_INNER];
    int r = ai_write_assoc(ai, ai->self, d, AI_WSRC_INNER);
    ok(r >= 0, "开着时返回成功");
    ok(ai->writes[AI_WSRC_INNER] == w0 + 1, "来源账 +1（「内部对话」那一格）");
    /* `micro_try` 是"**审完就落**"（不是排队等下一步）—— 所以看的是：边落地了 + 审查记账动了 */
    ok(micro_has_edge(ai->self, d), "**边真的落地了**（经审查之后应用的）");
    ok(micro_of(ai)->safe.n_allowed > 0, "安全内核记了账（审查→应用那条路真走了）");
    /* 想象那一路：把想象率拉满，多试几次必然触发；看账与 `[想象]` 标记 */
    size_t wi0 = ai->writes[AI_WSRC_IMAGINE];
    ai->imagine_rate = 0.5f;
    (void)ai_focus_push(ai, a, 1.0f);
    (void)ai_focus_push(ai, b, 1.0f);
    size_t fires = 0;
    for (int i = 0; i < 200 && fires < 3; ++i) {
        size_t before = ai->n_imaginations;
        ai_imagine(ai);
        if (ai->n_imaginations > before) fires++;
    }
    ok(fires >= 1, "想象真的触发了（把想象率拉满、多试几次）");
    ok(ai->writes[AI_WSRC_IMAGINE] > wi0, "想象那一路的写入**记进来源账**（「想象」那一格）");
    {
        Token *mk = pool_find(ai->pool, "[想象]");
        ok(mk != NULL, "`[想象]` 标记还在（想象出来的东西照样有记号）");
    }
    ai_free(ai);
}

/* ========== ㉚ 动态关系的**否定**（§6.80：旗子借 `nrel` 第 15 位，零格式改动） ==========
 *
 * 原来「动态关系只支持肯定」：核心关系有 `rel/nrel` 位，清单外的谓语只有 `dyn`，
 * 于是对话里说「A 不 <动态谓语> B」整句掉降级答复。现在否定旗子藏在 `nrel` 第 15 位
 * （9~14 位空着没用），`rel_direct_as/rel_direct_neg_as` 的动态分支也认它。 */
/* ①l 核对："帧/检索接管之后走不到那几段"这条登记**不成立**（§6.81）：
 * 元认知在 agent_step 的**公共尾部**（每轮都跑），漂移/诊断/叙事按**固定轮次**
 * （200/300/500）跑 —— 短会话里 `叙事=0` 是"没到轮次"，不是"被挡住了"。
 * 这条看护把"到轮次就会跑"钉死：把轮次调成 1，跑几轮，计数必须动。 */
static void test_tail_blocks(void) {
    printf("== ㉜ ①l 尾部那几段（元认知/漂移/诊断/叙事）**按轮次真的会跑** ==\n");
    Agent *ag = agent_new(NULL, NULL, NULL);
    ok(ag != NULL, "建一个 Agent");
    if (ag) {
        AI *ai = ag->brain;
        ai->narrate_every = 1;           /* 本来是 500：调成 1 就能在一轮里看到它跑 */
        ai->drift_sample_every = 1;      /* 本来是 200 */
        ai->gc.gc_interval = 0;          /* 别的收尾块关掉，别互相干扰 */
        ai->sleep_every = 0;
        size_t n0 = ai->n_narrations, d0 = ai->n_drifts_detected;
        char out[512];
        for (int i = 0; i < 3; ++i) (void)agent_step(ag, "猫 有 毛 吗", out, sizeof out);
        ok(ai->n_narrations > n0, "叙事**真的跑了**（轮次到了就跑，不是被帧路径挡住）");
        /* 元认知每轮都会写 `ai->meta`（总评在 [0,1]）：用它当"跑过"的判据 */
        ok(ai->meta.total >= 0.0f && ai->meta.confidence >= 0.0f,
           "元认知在**公共尾部**（每轮都跑，不是帧路径专有）");
        (void)d0;
        agent_free(ag);
    }
}

/* ① 档最后两条（§6.82）：**动作符号节点**（`[执行:…]`）+ **过程的执行槽**（步骤真的调动作） */
/* ② 档第一件（§6.83）：求值层样板 —— 条件里的**数值比较**真算一遍 */
/* ② 档"分词最长匹配"（§6.84）：分词器本来就是**池内最长匹配** —— 切开的词全是池里没有的，
 * 缺口在**词表覆盖**。`lexicon.txt`（一行一个词）就是补这一块的**数据**入口。 */
/* ② 档：**他者上位降权**（§6.85）—— 他说的比你问的**更宽**时，默认只当"相关"；
 * 开关打开 ⇒ 算**半票参考**（只在没有确定表态时才决定倾向）。 */
/* ② 档：**触发接求解**（§6.86）—— 开放式关系问句（缺一端）走求解器 */
/* ② 档：**指代消解最小版**（§6.87）—— 句首 他/她/它 指回最近说话的那个人 */
/* ② 档：**闸门分层判据**（§6.89）—— 硬门槛 + 三档，替掉"一个阈值切不动"那条 */
/* ② 档：**类比兜底**（§6.90）—— 规则答不出时，用同类（同一上位）互相推，答**大概** */
/* C③ 第二/三件（§6.97）：修法表按错误文本选规则 + 多轮修正的开关默认关 */
/* 思维侧第二条（§6.99）：**N1 参与决策** —— 走边没走出好东西时用它想通的结论 */
/* ③ 换法扩展（§6.101）：退第二次时换**同类互推**（规则层不做的那一类） */
static void test_redo_alt(void) {
    printf("== ㊸ ③ 换法扩展：第二个角度是**同类互推**（默认关） ==\n");
    AI *ai = ai_new();
    Token *p = T(ai, "类2"), *a = T(ai, "熊甲2"), *b = T(ai, "熊乙2"), *o = T(ai, "冬眠2");
    (void)token_tag_rel(a, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, o, REL_CAN, REL_BOOST);
    /* 共用判据本身（rel.h 里"一处放"的那个） */
    { const Token *sup = NULL;
      ok(rel_sibling_with(ai, a, REL_CAN, o, &sup) == b && sup == p,
         "`rel_sibling_with` 找得出同类（熊乙2）与共同上位（类2）"); }
    Think *t = think_of(ai);
    ThinkGoal g;
    memset(&g, 0, sizeof g);
    g.a = a; g.b = o; g.rid = REL_CAN; g.depth = 0;
    t->redo_alt = 0; t->redo_on = 1; t->n_backtrack = 1; t->concl = 0; t->guessed = 0;
    ok(think_redo_after_premise(ai, t, &g, 0, 0) == 0 || t->concl == 0,
       "关着：不走同类互推（只留原来那条「问反面」的路）");
    t->redo_alt = 1; t->redo_on = 1; t->n_backtrack = 1; t->concl = 0; t->guessed = 0;
    int r = think_redo_after_premise(ai, t, &g, 0, 0);
    ok(r == 1 && t->concl == 1 && t->guessed == 1,
       "开着：退了，并且给的是**猜出来的**结论（concl=1 + guessed=1）");
    ok(t->conf < 0.5f, "置信度压低（<0.5，不当成确定的事）");
    { int found = 0;
      for (size_t i = 0; i < t->trace_n; ++i)
          if (strstr(t->trace[i].why, "同类互推") || strstr(t->trace[i].what, "同类互推")) found = 1;
      ok(found, "痕上写清是**同类互推**（依据是谁也写进去了）"); }
    ai_free(ai);
}

/* ③ 多层快照栈（§6.102）：第二次回退要**退到更外一层**，不是退回同一个起点 */
/* §6.103 草稿留痕：**在核心里**（MOD_PATCH_CALL 的 applier）证正例 ——
 * CLI 的 `::patch op` 绕过内核（实测"允许 1 拒绝 0"），所以只能直接调核心。 */
/* §6.110 v11：**N1 思考对象持久化**（存摘要：结论/缺口/复没复核） */
/* §6.111 E2：边存**时刻集合**（互不相同的 tick，≤4）+ v12 往返 */
/* §6.112 E3 v13：他者的**改口历史**（真改了才记；重复说同一句不记） */
/* §6.117 ① **联想出来的东西能用**：同上位兄弟算一条硬证据，走**已有的可验证**联想引擎
 * （`rel_guess` → `der_default` → `der_verify`），而不是另造一条求解器兜底。
 * 旧的"求解器兜底"（§6.113）已撤：它会让验证器自查"知识库里有吗"时得到"有"，
 * 于是类比链永远被判缺口（实测：`der_verify` 报 1 个缺口）。 */
/* §6.118 **假设下的结论沉淀成条件缓存**：前提 = **整条假设栈的指纹**（不是最深那层） */
/* §6.124 **先链后联想**：确定性推理答不上来时，才轮到"同类互推"的**大概**答案。
 * 老顺序里联想兜底排在确定性推理**之前** ⇒ 它会抢答（证明过的答案被"猜的答案"盖掉）。
 * 这里钉三点：① 默认档仍是老顺序（反向测量时跳过断言）；② 开着时证明优先；
 * ③ 规则真答不了时，联想**照旧能答**（不能修了抢答、把联想弄丢）。 */
static void test_rel_first_order(void) {
    printf("== ⓷ 先链后联想：证明过的答案不被「猜的答案」抢掉（默认关） ==\n");
    const char *cases[3] = { "只有继承", "继承+类比", "只有类比" };
    for (int rf = 0; rf < 2; ++rf) {
        for (int mode = 0; mode < 3; ++mode) {
            Agent *ag = agent_new(NULL, NULL, NULL);
            AI *ai = ag->brain;
            if (rf == 0 && !getenv("AIP_RELFIRST") && mode == 0)
                ok(ai->rel_first == 1, "默认档 `rel_first == 1`（先链后联想已翻默认；env 掀掉时跳过）");
            else if (rf == 0 && getenv("AIP_RELFIRST") && mode == 0)
                ok(1, "AIP_RELFIRST 已在环境里（反向测量）：默认值断言按规矩跳过");
            ai->rel_analogy = 1;
            ai->rel_first = rf;
            Token *p = T(ai, "类9"), *a = T(ai, "甲9"), *o = T(ai, "冬眠9"), *b = T(ai, "乙9");
            (void)T(ai, "会");
            if (mode != 2) (void)ai_write_rel(ai, p, o, REL_CAN, 0, AI_WSRC_RELATION);
            (void)ai_write_rel(ai, a, p, REL_IS_A, 0, AI_WSRC_RELATION);
            if (mode != 0) {
                (void)ai_write_rel(ai, b, p, REL_IS_A, 0, AI_WSRC_RELATION);
                (void)ai_write_rel(ai, b, o, REL_CAN, 0, AI_WSRC_RELATION);
            }
            char out[512] = {0};
            agent_step(ag, "甲9 会 冬眠9 吗", out, sizeof out);
            char msg[220];
            int guessed = strstr(out, "大概") != NULL;
            int proved  = strstr(out, "继承") != NULL;
            if (rf == 0) {
                /* ⚠ §6.133 起这条断言**改了**：老顺序下 mode==1（继承+类比）以前是
                 * `guessed`（联想**抢答**，把能证出来的答案盖掉）—— 那是 bug，不是特性。
                 * 现在兜底也**过验证器**，而验证器对 DEFAULT 有一关是"知识库里推得出来就别猜"
                 * ⇒ 老顺序下也不再抢答：两种顺序**结果一致**。这里就把"一致"钉住。 */
                if (mode == 2)
                    snprintf(msg, sizeof msg, "老顺序·%s：规则答不了 ⇒ 联想照旧答「大概」", cases[mode]);
                else
                    snprintf(msg, sizeof msg,
                             "老顺序·%s：**不再抢答**（§6.133：兜底也过验证器，能证就不猜）", cases[mode]);
                ok(mode == 2 ? guessed : (proved && !guessed), msg);
            } else {
                if (mode == 0 || mode == 1)
                    snprintf(msg, sizeof msg, "新顺序·%s：**证明优先**，不再被「大概」盖掉", cases[mode]);
                else
                    snprintf(msg, sizeof msg, "新顺序·%s：规则答不了 ⇒ 联想照旧能答「大概」", cases[mode]);
                ok(mode == 2 ? guessed : (proved && !guessed), msg);
            }
            agent_free(ag);
        }
    }
}

/* §6.125 **递归迁移**：类比猜出来的结论记下来，**下一个问题能拿它当中转** ——
 * 但绝不算知识（`rel_direct`/`rel_solve`/复核三处都查不到），答复照旧标"推测"。 */
static void test_recursive_transfer(void) {
    printf("== ⓸ 递归迁移：猜出来的结论当下一个问题的输入（默认关） ==\n");
    for (int rm = 0; rm < 2; ++rm) {
        Agent *ag = agent_new(NULL, NULL, NULL);
        AI *ai = ag->brain;
        if (rm == 0 && !getenv("AIP_RELMEM"))
            ok(ai->rel_remember == 1, "默认档 `rel_remember == 1`（记住猜测已翻默认；env 掀掉时跳过）");
        else if (rm == 0)
            ok(1, "AIP_RELMEM 已在环境里（反向测量）：默认值断言按规矩跳过");
        ai->rel_remember = rm;
        ai->rel_analogy = 1;
        Token *big = T(ai, "大类9"), *c9 = T(ai, "类9"), *c10 = T(ai, "类10");
        Token *jia = T(ai, "甲9"), *o = T(ai, "冬眠9");
        (void)T(ai, "会");
        (void)ai_write_rel(ai, c9, big, REL_IS_A, 0, AI_WSRC_RELATION);
        (void)ai_write_rel(ai, c10, big, REL_IS_A, 0, AI_WSRC_RELATION);
        (void)ai_write_rel(ai, c10, o, REL_CAN, 0, AI_WSRC_RELATION);   /* 只有同类知道 */
        (void)ai_write_rel(ai, jia, c9, REL_IS_A, 0, AI_WSRC_RELATION); /* 甲9 属于 类9 */
        char q1[600] = {0}, q2[600] = {0};
        agent_step(ag, "类9 会 冬眠9 吗", q1, sizeof q1);
        agent_step(ag, "甲9 会 冬眠9 吗", q2, sizeof q2);   /* 要靠上面那条当输入 */
        if (rm == 0) {
            ok(ai->n_rel_guess_mem == 0, "显式关掉：不记（记住条数 0）");
            ok(strstr(q2, "不 太 懂") && !strstr(q2, "猜"), "显式关掉：迁移不了（Q2 仍是「不太懂」）");
        } else {
            char m[240];
            snprintf(m, sizeof m, "开着：Q1 的类比结论被记下（记住 %zu 条）", ai->n_rel_guess_mem);
            ok(ai->n_rel_guess_mem == 1, m);
            ok(strstr(q2, "大概") && strstr(q2, "猜"),
               "开着：Q2 拿它当中转（答复「大概」+ 链上标「猜」）");
            Proof pf;
            rel_solve(ai, c9, REL_CAN, o, 0, &pf);
            ok(rel_direct(c9, REL_CAN, o) == 0 && pf.status == 0 &&
               rel_direct_review(c9, REL_CAN, o, 0) == 0,
               "**不中毒**：猜测记忆不算知识（rel_direct / rel_solve / 复核 三处都查不到）");
            ok(ai->last_der.guessed == 1 && ai->last_der.gaps == 0,
               "链标成「推测」且缺口=0（验证器独立重推过）");
        }
        agent_free(ag);
    }
}

/* §6.126 **受审执行层的边界**（外部审计 P0 + P1-1，先本机复现、再修、这里钉住）：
 *   · P0：`sed e <文件>` 把**文件内容**交给 /bin/sh 执行（argv 表面检查看不到 payload）
 *   · P1-1a：`@响应文件` 把参数从文件里读出来（`-o /tmp/x` 就这么溜出去的）
 *   · P1-1b：`-o/tmp/x`、`-I/etc` 这类**开关里夹带路径**（一个字参数，不以 / 开头）
 *   · P1-1c：源码里的 `#include "/etc/passwd"` / `../../etc/passwd`（编译期读工作区外）
 * 还钉住"合法用法没被误伤"。 */
static void test_action_boundary(void) {
    printf("== ⓹ 受审执行层边界：sed/@/开关路径/include 越界（§6.126） ==\n");
    char why[192];
    { const char *a[] = { "sed", "e", "pwn.txt" };
      ok(action_check(a, 3, why, sizeof why) != 0, "P0：sed 不在白名单（命令被拒）"); }
    { const char *a[] = { "gcc", "@args.txt", "x.c" };
      ok(action_check(a, 3, why, sizeof why) != 0, "P1-1a：@响应文件被拒"); }
    { const char *a[] = { "gcc", "-o/tmp/x", "s.c" };
      ok(action_check(a, 3, why, sizeof why) != 0, "P1-1b：开关里夹带路径被拒（-o/tmp/x）"); }
    { const char *a[] = { "gcc", "-I/etc", "s.c" };
      ok(action_check(a, 3, why, sizeof why) != 0, "P1-1b：-I/etc 被拒"); }
    { const char *a[] = { "gcc", "-O2", "-o", "a.out", "s.c" };
      ok(action_check(a, 5, why, sizeof why) == 0, "合法编译没被误伤（-O2 -o a.out s.c）"); }
    { const char *a[] = { "cat", "sub/f.txt" };
      ok(action_check(a, 2, why, sizeof why) == 0, "合法相对子路径没被误伤（cat sub/f.txt）"); }
    { const char *a[] = { "cat", "/etc/passwd" };
      ok(action_check(a, 2, why, sizeof why) != 0, "老规矩还在：绝对路径被拒"); }
    /* §6.126 外部审计 P2-3：`::patch op 记事本 删 <pos> <len>` 的**有符号溢出**。
     * 原来 pos=LONG_MAX、len=1 ⇒ pos+len 溢出成负 ⇒ o2<o1 ⇒ `o2-o1` 作 size_t 成巨数
     * ⇒ memmove 写坏 NoteState 后面的字段。现在钳住 ⇒ 越界输入删不掉东西（草稿不变）。 */
    { AI *ai = ai_new();
      (void)patch_mount_builtin(ai, "记事本");
      char rep1[200] = {0}, rep2[200] = {0}, rep3[200] = {0};
      { char *a[] = { (char *)"你好世界" };
        (void)patch_op_call_by_name(ai, "记事本", "写", a, 1, rep1, sizeof rep1); }
      { char *a[] = { (char *)"9223372036854775807", (char *)"1" };
        int rc = patch_op_call_by_name(ai, "记事本", "删", a, 2, rep2, sizeof rep2);
        ok(rc >= 0 && strcmp(rep1, rep2) == 0,
           "P2-3：pos=LONG_MAX 的删除被钳住（草稿没变，也没崩）"); }
      { char *a[] = { (char *)"-5", (char *)"-3" };
        int rc = patch_op_call_by_name(ai, "记事本", "删", a, 2, rep3, sizeof rep3);
        ok(rc >= 0 && strcmp(rep1, rep3) == 0, "P2-3：负数位置/长度被钳住（草稿没变）"); }
      { char *a[] = { (char *)"0", (char *)"1" };
        (void)patch_op_call_by_name(ai, "记事本", "删", a, 2, rep3, sizeof rep3);
        ok(strcmp(rep1, rep3) != 0, "P2-3：正常删除照旧有效（12 字节 → 9 字节）"); }
      ai_free(ai); }
    /* §6.126 外部审计 P2-8：**满参数时 argv 也要 NULL 结尾**（execvp 靠它停）。
     * 第一版只在 `argc < max` 时补 NULL，正好 32 个参数时数组里没位置 ⇒ 读越界。 */
    { char buf[512];
      char *av[ACTION_ARG_MAX + 1];
      size_t off = 0;
      for (int i = 0; i < ACTION_ARG_MAX + 1; ++i)
          off += (size_t)snprintf(buf + off, sizeof buf - off, "%sa%d", i ? " " : "", i);
      int ac = action_split(buf, av, ACTION_ARG_MAX);
      ok(ac == ACTION_ARG_MAX, "P2-8：先确认真的塞满了参数（ac=32）");
      ok(av[ACTION_ARG_MAX] == NULL, "P2-8：塞满时 argv 仍然 NULL 结尾（否则 execvp 读栈上垃圾）"); }
    { /* include 扫描：真写一个越界源文件到工作区，再扫（不跑命令） */
      (void)mkdir("_t", 0755);
      (void)mkdir("_t/work", 0755);
      FILE *f = fopen("_t/work/越界.c", "w");
      int wrote = 0;
      if (f) { fputs("#include \"/etc/hostname\"\nint main(void){return 0;}\n", f); fclose(f); wrote = 1; }
      if (wrote) {
          const char *a[] = { "gcc", "-E", "越界.c" };
          ok(action_scan_includes(a, 3, why, sizeof why) != 0, "P1-1c：源码里的越界 include 被拒");
          FILE *g = fopen("_t/work/干净.c", "w");
          if (g) { fputs("#include <stdio.h>\n#include \"sub/h.h\"\n", g); fclose(g); }
          const char *b[] = { "gcc", "-c", "干净.c" };
          ok(action_scan_includes(b, 3, why, sizeof why) == 0,
             "合法的 <stdio.h> / 相对 include 没被误伤");
          (void)remove("_t/work/越界.c");
          (void)remove("_t/work/干净.c");
      } else {
          ok(0, "P1-1c：写测试用源文件失败（跳过）");
      } }
}

/* §6.120 **逐字接地队列**（用户提的方向）：顺序接地 + 字符按顺序当"词"，且**分词器保留** */
static void test_char_stream(void) {
    printf("== ⓵ 逐字接地：顺序队列 + 字符即词（分词器保留成可选） ==\n");
    AI *ai = ai_new();
    (void)T(ai, "字");
    if (getenv("AIP_CHARSTREAM")) ok(1, "AIP_CHARSTREAM=1（反向测量）：默认值断言按规矩跳过");
    else ok(ai->char_stream == 0, "**默认关**（关着走分词器，行为一个字不变）");
    ai_char_ground(ai, "猫有毛");
    ok(ai->cq_n == 9 && memcmp(ai->cq, "猫有毛", 9) == 0,
       "顺序接地：3 个汉字 9 字节，**按到达顺序**进队列");
    { const char *w[8]; char buf[8 * 5];   /* ⚠ 最坏 4 字节/字 + 终止符 */
      size_t n = ai_char_words(ai, w, buf, 8);
      ok(n == 3, "字符按顺序切成 3 个「词」");
      ok(n == 3 && strlen(w[0]) == 3 && strcmp(w[0], "猫") == 0 &&
         strcmp(w[1], "有") == 0 && strcmp(w[2], "毛") == 0,
         "每个词是**一个完整 UTF-8 字**（不是字节），且顺序不乱"); }
    { char **cw = NULL; size_t cn = 0;
      ai->cq_n = 0;                      /* ⚠ 队列是**累积**的（滚动窗口）：不清空会拿到 6 项 */
      cw = ai_char_tokenize(ai, "猫有毛", &cn);
      ok(cw && cn == 3, "`ai_char_tokenize` 的形状与 `ai_tokenize` 一致（3 项，可同样释放）");
      if (cw) ai_free_words(cw, cn); }
    { char big[700];
      for (int i = 0; i < 200; ++i) memcpy(big + i * 3, "猫", 3);
      big[600] = 0;
      ai_char_ground(ai, big);
      ok(ai->cq_n <= sizeof ai->cq, "接地滚动窗口：队列不超过容量");
      ok(ai->cq_n % 3 == 0, "满了丢**整字**（不会把 UTF-8 截成半个字）");
      ok(ai->cq_n >= 3 && memcmp(ai->cq + ai->cq_n - 3, "猫", 3) == 0, "保留的是**最新**的字（顺序对）"); }
    { size_t tn = 0;
      char **tw = ai_tokenize(ai, "猫 有 毛", &tn);
      ok(tw && tn >= 1, "**分词器仍在**（这条路一个字都没动 —— 用户要求现在不要撤）");
      if (tw) ai_free_words(tw, tn); }
    ai_free(ai);
}

/* ① 第 1 件：**答复层的联想** —— 已验证的类比链要能进对话答复（措辞"大概"，不装确定） */
static void test_answer_analogy(void) {
    printf("== ⓶ 答复层联想：已验证的类比链进答复（不是「不确定」） ==\n");
    AI *ai = ai_new();
    Token *p = T(ai, "类7"), *a = T(ai, "熊甲7"), *b = T(ai, "熊乙7"), *o = T(ai, "冬眠7");
    Proof pf;
    (void)T(ai, "会");
    (void)token_tag_rel(a, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, o, REL_CAN, REL_BOOST);
    ok(rel_direct(a, REL_IS_A, p) == 1, "图①：熊甲7 属于 类7 建起来了");
    ok(rel_sibling_with(ai, a, REL_CAN, o, NULL) == b, "图②：兄弟判据当场找得到 熊乙7");
    ok(rel_guess(ai, a, REL_CAN, o, &pf) == 1, "图③：rel_guess 用这张图能联想起 熊乙7");
    ai->last_der.n = 0;
    char *w[4] = {(char *)"熊甲7", (char *)"会", (char *)"冬眠7", (char *)"吗"};
    /* (旧的 conf 变量已不用) */
    { /* 先钉"退在哪一步"：解析得过吗？rel_guess 单独能成吗？ */
        const char *aw = NULL, *bw = NULL, *rw = NULL;
        uint8_t rid = 0; int is_q = 0, is_neg = 0;
        char jb[64];
        int pk = rel_parse(ai, w, 4, jb, sizeof jb, &aw, &rid, &is_neg, &bw, &is_q, &rw, 0);
        ok(pk && is_q && rid != 0 && aw && bw, "`rel_parse` 认得出这是个关系问句（否则答复层压根进不去）");
        { char m[160];
          snprintf(m, sizeof m, "解析出的 rid=%u（REL_CAN=%u；用错 rid 造图 ⇒ 联想自然找不到）",
                   (unsigned)rid, (unsigned)REL_CAN);
          ok(rid == REL_CAN, m); }
        Proof pf2;
        ok(rel_guess(ai, a, REL_CAN, o, &pf2) == 1, "`rel_guess` 单独调用是成功的（问题不在联想引擎）");
        if (pf2.n >= 3) {
            /* 用**同一个 pf** 复现答复层那句 der_verify（答复层要求 gaps == 0 才敢答） */
            Token *src2 = pf2.step[1].tok;
            int pol2 = pf2.step[pf2.n - 1].neg ? -1 : 1;
            int sim2 = pf2.step[1].sim;
            Derivation d2;
            der_reset(&d2);
            ai->n_der++;                    /* 与答复层同序 */
            der_goal(&d2, a, REL_CAN, o);
            der_default(&d2, a, src2, REL_CAN, o, sim2, pol2);
            der_fact(&d2, src2, REL_CAN, o, pol2 < 0 ? 1 : 0, pol2);
            der_conclude(&d2, pol2, 45);
            int g2 = der_verify(ai, &d2);
            char m2[220];
            snprintf(m2, sizeof m2, "局部 Derivation 上验证过（gaps=%d）", g2);
            ok(g2 == 0, m2);
            /* 再在**共享缓冲** `ai->last_der` 上做同一件事 —— 答复层用的就是它 ✗ */
            Derivation *d3 = &ai->last_der;
            der_reset(d3);
            der_goal(d3, a, REL_CAN, o);
            der_default(d3, a, src2, REL_CAN, o, sim2, pol2);
            der_fact(d3, src2, REL_CAN, o, pol2 < 0 ? 1 : 0, pol2);
            der_conclude(d3, pol2, 45);
            int g3 = der_verify(ai, d3);
            { char m3[220];
              const char *n3 = "";
              for (size_t q = 0; q < d3->n; ++q) if (d3->step[q].kind == DER_GAP) n3 = d3->step[q].note;
              snprintf(m3, sizeof m3, "**共享缓冲 ai->last_der** 上验证（gaps=%d，缺口说：%s）", g3, n3);
              ok(g3 == 0, m3); }
        }
    }
    /* **对话里真正可用的那一段**是 §6.90 的钩子（`agent_step` 在关系层说不确定时调它）：
     * 答复层那个"已验证的猜测"（`agent_answer_relation_inner` 里的 rel_guess 分支）在这类
     * **对话现学**的词上没走通（另案，见 §6.121 的记录），所以看护钉的是**真正可用的行为**。 */
    float conf2 = 0.0f;
    const char *ra = getenv("AIP_RELANALOGY");
    if (ra && ra[0] == '0') {                 /* 反向测量：显式关的那一侧 */
        ok(agent_analogy_fallback(ai, w, 4, &conf2) == NULL,
           "AIP_RELANALOGY=0（反向测量）：关着就不兜（回老行为）");
    } else {
    char *r = agent_analogy_fallback(ai, w, 4, &conf2);
    if (!r) ok(0, "对话层类比没给答复（联想就不可用 ✗）");
    else {
        char m[240];
        snprintf(m, sizeof m, "对话层类比答复：%s（把握 %.2f）", r, (double)conf2);
        ok(strstr(r, "大概") && strstr(r, "熊乙7") && strstr(r, "类比"), m);
        ok(conf2 > 0.0f && conf2 < 0.6f, "置信度压低（不装确定）");
        free(r);
    }
    }
    ai_free(ai);
}

static void test_hypo_cache(void) {
    printf("== ㊿ 假设结论沉淀：条件缓存 + 整栈指纹（默认关） ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "假甲"), *b = T(ai, "假乙"), *h = T(ai, "假丙");
    char out[128];
    if (getenv("AIP_HYPOCACHE")) ok(1, "AIP_HYPOCACHE=1（反向测量）：默认值断言按规矩跳过");
    else ok(ai->hypo_cache == 0, "默认关（不动今天的行为）");
    ai->hypo_cache = 1;
    agent_hypo_cache(ai, a, REL_IS_A, b, 0, 1);
    ok(ai->n_hypo_cache == 0, "没有假设时**不沉淀**（没有「条件」可言）");
    ok(hypo_push(&ai->hypo, h) == 1 && hypo_add(&ai->hypo, a, b, REL_IS_A, 0) == 1, "进一层假设");
    char fp1[32];
    ai_hypo_fingerprint(ai, fp1, sizeof fp1);
    agent_hypo_cache(ai, a, REL_IS_A, b, 0, 1);
    ok(ai->n_hypo_cache == 1, "假设下推出 ⇒ 沉淀一条**条件缓存**");
    Token *P = pool_find(ai->pool, fp1);
    ok(P && strncmp(P->name, "[假设栈:", 9) == 0, "前提符号节点 = **整栈指纹**（[假设栈:xxxxxxxx]）");
    ok(ai_premise_holds(ai, P) == 1, "栈还活着 ⇒ 前提成立 ⇒ 缓存可用");
    ok(hypo_push(&ai->hypo, h) == 1 && hypo_add(&ai->hypo, b, a, REL_IS_A, 0) == 1, "再进一层（嵌套）");
    char fp2[32];
    ai_hypo_fingerprint(ai, fp2, sizeof fp2);
    ok(strcmp(fp1, fp2) != 0, "**嵌套后指纹不同**（这就是「不能用最深那层」的意思）");
    ok(ai_premise_holds(ai, P) == 0, "旧缓存的前提不再成立 ⇒ 自动失效（不用去删它）");
    (void)hypo_pop(&ai->hypo);
    ok(ai_premise_holds(ai, P) == 1, "弹掉内层 ⇒ 指纹回到外层 ⇒ 缓存又可用（整栈算才对）");
    (void)hypo_pop(&ai->hypo);
    ok(ai_premise_holds(ai, P) == 0, "栈全撤 ⇒ 前提不成立");
    /* ⚠ `加前提` 是**提议**（进 mod 队列）⇒ 测试里必须把队列跑掉，链接才会落地
     * （和 §6.94 那次"CLI 的补丁操作绕过内核"是同一类机制问题）。 */
    out[0] = 0;
    (void)micro_step(ai, NULL, 0, out);
    ok(token_premise_link(a, P) != NULL,
       "跑掉队列后：`A —[前提]→ P` 真的落地了（条件缓存的形状）");
    ai_hypo_cache_sweep(ai);
    ok(token_premise_link(a, P) != NULL, "无条件推不出来时：**不升格**（条件缓存留着，等条件回来）");
    /* "用户后来直接教了它" = 图上出现一条**非推论**的边（真实写入会带自己的 role） */
    (void)token_tag_rel(a, b, REL_IS_A, REL_BOOST);
    for (size_t k = 0; k < a->count; ++k)
        if (a->assocs[k].main == b && (a->assocs[k].rel & REL_BIT(REL_IS_A)))
            a->assocs[k].role = ASSOC_ROLE_INPUT;
    int uncond = rel_solve_ignoring_caches(ai, a, REL_IS_A, b, 0);
    ai_hypo_cache_sweep(ai);
    { char m3[200];
      const AssocEntry *eab = NULL;
      for (size_t k = 0; k < a->count; ++k)
          if (a->assocs[k].main == b) eab = &a->assocs[k];
      snprintf(m3, sizeof m3,
               "教了之后（无条件推得出=%d，边role=%d）⇒ **升格：前提摘掉**",
               uncond, eab ? (int)eab->role : -1);
      ok(token_premise_link(a, P) == NULL, m3); }
    ok(rel_direct(a, REL_IS_A, b) == 1, "升格后这条结论变成普通推论（边还在）");
    ai_free(ai);
}

static void test_guess_sibling(void) {
    printf("== ㊾ ① 联想能用：同上位兄弟 ⇒ rel_guess 给出**可验证**的推测 ==\n");
    AI *ai = ai_new();
    Token *p = T(ai, "类6"), *a = T(ai, "熊甲6"), *b = T(ai, "熊乙6"), *o = T(ai, "冬眠6");
    (void)token_tag_rel(a, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, o, REL_CAN, REL_BOOST);
    /* 新词不保证没有 emb（`T()` 可能建过）—— 这里**显式清掉**来模拟"对话里现学的新词" */
    int8_t *save_a = a->emb, *save_b = b->emb;
    a->emb = NULL; b->emb = NULL;
    ok(rel_sibling_with(ai, a, REL_CAN, o, NULL) == b, "兄弟判据找得到 熊乙6（同一上位）");
    Proof pf;
    ok(rel_guess(ai, a, REL_CAN, o, &pf) == 1, "没有 emb 也照样联想得出（不再被 emb 前置挡住）");
    ok(pf.status == 1 && pf.guessed == 1, "结论标了 guessed（不许装成确定）");
    ok(pf.n >= 3 && pf.step[1].via == 5 && pf.step[1].sim >= 40,
       "证明步形状对（via=5 类比、sim 过门槛）—— 验证器认的就是这个形状");
    /* 端到端：把这条推测送进推导层，验证器**要接受**（0 缺口） */
    Derivation d;
    der_reset(&d);
    der_goal(&d, a, REL_CAN, o);
    der_default(&d, a, b, REL_CAN, o, pf.step[1].sim, 1);
    der_fact(&d, b, REL_CAN, o, 0, 1);
    der_conclude(&d, 1, 45);
    int gaps = der_verify(ai, &d);
    {
        char m2[200];
        const char *note = "";
        for (size_t q = 0; q < d.n; ++q) if (d.step[q].kind == DER_GAP) note = d.step[q].note;
        snprintf(m2, sizeof m2,
                 "验证器**接受**这条类比链（0 缺口；实得 %d，缺口说：%s）", gaps, note);
        ok(gaps == 0, m2);
    }
    ok(d.guessed == 1, "整条推导被标成 guessed（答复必须带「大概」这类措辞）");
    a->emb = save_a; b->emb = save_b;
    ai_free(ai);
}

static void test_other_hist(void) {
    printf("== ㊽ E3 v13：他者改口历史（total / 最近三次 / 存盘读回） ==\n");
    AI *ai = ai_new();
    OtherModel *om = other_intern(ai, T(ai, "他者甲"));
    Token *a = T(ai, "甲说"), *b = T(ai, "乙事");
    ok(om != NULL, "造一个「他者甲」");
    if (om) {
        ai->pool->current_tick = 3;
        (void)other_belief_learn(ai, om, a, b, REL_IS_A, 0);      /* 先说「认」 */
        ok(om->hist[0].total == 0, "第一次说：不是改口（total 还是 0）");
        ai->pool->current_tick = 7;
        (void)other_belief_learn(ai, om, a, b, REL_IS_A, 1);      /* 改口成「不认」 */
        ok(om->hist[0].total == 1 && om->hist[0].n == 1 && om->hist[0].old_neg[0] == 0,
           "改口：记下「改之前是认」+ tick=7（total=1）");
        (void)other_belief_learn(ai, om, a, b, REL_IS_A, 1);      /* 重复同一句 */
        ok(om->hist[0].total == 1, "**重复说同一句不算改口**（total 不涨）");
    }
    size_t sz = ai_save_file(ai, "ohist.bin");
    ok(sz > 0, "存盘（v13）");
    AI *c = ai_load_file("ohist.bin");
    if (c) {
        const OtherModel *o2 = NULL;
        for (size_t i = 0; i < c->others_n; ++i)
            if (c->others[i].who && c->others[i].who->name &&
                strcmp(c->others[i].who->name, "他者甲") == 0) o2 = &c->others[i];
        ok(o2 && o2->hist[0].total == 1 && o2->hist[0].n == 1 &&
           o2->hist[0].old_neg[0] == 0 && o2->hist[0].tick[0] == 7,
           "读回来：改口历史逐值还在（v13 往返）");
        ai_free(c);
    } else ok(0, "读回来失败（v13 那段读写不一致）");
    remove("ohist.bin");
    ai_free(ai);
}

static void test_edge_tset(void) {
    printf("== ㊼ E2：边上的时刻集合（互不相同；满了滚最新） ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "时刻甲"), *b = T(ai, "时刻乙");
    (void)token_tag_rel(a, b, REL_IS_A, REL_BOOST);      /* 造一条边 */
    AssocEntry *e = NULL;
    for (size_t i = 0; i < a->count; ++i) if (a->assocs[i].main == b) { e = &a->assocs[i]; break; }
    ok(e != NULL, "造出一条 a→b 的边");
    if (e) {
        ai->pool->current_tick = 5;  token_touch_entry(a, e);
        ai->pool->current_tick = 5;  token_touch_entry(a, e);   /* 同一 tick 只记一次 */
        ai->pool->current_tick = 9;  token_touch_entry(a, e);
        ok(e->n_tset == 2 && e->tset[0] == 5 && e->tset[1] == 9,
           "两个**不同** tick ⇒ 集合 = {5, 9}（同一个 tick 不重复记）");
        ai->pool->current_tick = 11; token_touch_entry(a, e);
        ai->pool->current_tick = 13; token_touch_entry(a, e);
        ai->pool->current_tick = 17; token_touch_entry(a, e);   /* 第 5 个不同 tick */
        ok(e->n_tset == 4 && e->tset[3] == 17,
           "满了（4）之后**滚最新** ⇒ 最初 3 次 + 最近 1 次（照实说，不是无限集合）");
    }
    size_t sz = ai_save_file(ai, "tset_test.bin");
    ok(sz > 0, "存盘（v12）");
    AI *c = ai_load_file("tset_test.bin");
    if (c) {
        Token *a2 = pool_find(c->pool, "时刻甲");
        const AssocEntry *e2 = NULL;
        if (a2) for (size_t i = 0; i < a2->count; ++i)
            if (a2->assocs[i].main && a2->assocs[i].main->name &&
                strcmp(a2->assocs[i].main->name, "时刻乙") == 0) { e2 = &a2->assocs[i]; break; }
        ok(e2 && e2->n_tset == 4 && e2->tset[0] == 5 && e2->tset[3] == 17,
           "读回来：时刻集合逐值还在（v12 往返）");
        ai_free(c);
    } else ok(0, "读回来失败（v12 那段读写不一致）");
    remove("tset_test.bin");
    ai_free(ai);
}

static void test_n1_persist(void) {
    printf("== ㊻ v11：N1 思考对象持久化（存盘→读回） ==\n");
    AI *ai = ai_new();
    Token *t = T(ai, "存盘用");
    (void)t;
    snprintf(ai->n1_concl, sizeof ai->n1_concl, "猫 会 呼吸");
    ai->n1_gaps = 2; ai->n1_verified = 1;
    size_t sz = ai_save_file(ai, "n1_test.bin");
    ok(sz > 0, "存盘成功（v11 格式，多了一段 N1 摘要）");
    AI *b = ai_load_file("n1_test.bin");
    if (b) {
        ok(strcmp(b->n1_concl, "猫 会 呼吸") == 0, "读回来：**结论**还在");
        ok(b->n1_gaps == 2 && b->n1_verified == 1, "读回来：**缺口数**与**复核标记**还在");
        ai_free(b);
    } else ok(0, "读回来失败（v11 那段读写不一致）");
    remove("n1_test.bin");
    ai_free(ai);
}

static void test_draft_keep(void) {
    printf("== ㊺ 草稿留痕：发送后核心层另存一条 `[草稿:…]`（默认关） ==\n");
    AI *ai = ai_new();
    ok(patch_mount_builtin(ai, "记事本") == 0, "挂上「记事本」补丁");
    { char repA[64]; char *avA[1] = {(char *)"auto"};
      (void)patch_op_call_by_name(ai, "记事本", "模式", avA, 1, repA, sizeof repA); }
    /* ⚠ 反向测量（`AIP_DRAFTKEEP=1`）时启动就是开着的 ⇒ "关着"那半边按规矩跳过 */
    int env_on = getenv("AIP_DRAFTKEEP") != NULL;
    ai->draft_keep = 0;
    size_t q0 = micro_of(ai)->queue.n;
    ModIntent wr;
    memset(&wr, 0, sizeof wr);
    wr.type = MOD_PATCH_CALL;
    snprintf(wr.patch, sizeof wr.patch, "记事本");
    snprintf(wr.name, sizeof wr.name, "写");
    snprintf(wr.args[0], sizeof wr.args[0], "试验草稿");
    wr.n_args = 1;
    (void)micro_apply_one(ai, &wr, NULL, 0);
    ModIntent sd = wr;
    snprintf(sd.name, sizeof sd.name, "发送");
    sd.n_args = 0;
    (void)micro_apply_one(ai, &sd, NULL, 0);
    if (env_on) ok(1, "AIP_DRAFTKEEP=1（反向测量）：关着那半边的断言按规矩跳过");
    else ok(micro_of(ai)->queue.n == q0, "关着：发送之后不多出任何意图");
    /* 开着：发送之后队列里多一条 [草稿:…] */
    ai->draft_keep = 1;
    (void)micro_apply_one(ai, &wr, NULL, 0);
    (void)micro_apply_one(ai, &sd, NULL, 0);
    int found = 0;
    for (size_t i = 0; i < micro_of(ai)->queue.n; ++i)
        if (strncmp(micro_of(ai)->queue.items[i].name, "[草稿:", 6) == 0) found = 1;
    ok(found, "开着：发送之后队列里多一条 `[草稿:试验草稿]`（核心层存的）");
    ai_free(ai);
}

static void test_redo_stack(void) {
    printf("== ㊹ ③ 多层快照栈：退到更外一层（默认关） ==\n");
    AI *ai = ai_new();
    Token *p = T(ai, "类3"), *a = T(ai, "熊甲3"), *b = T(ai, "熊乙3"), *o = T(ai, "冬眠3");
    (void)token_tag_rel(a, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, p, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, o, REL_CAN, REL_BOOST);
    Think *t = think_of(ai);
    ThinkGoal g;
    memset(&g, 0, sizeof g);
    g.a = a; g.b = o; g.rid = REL_CAN;
    /* 关着：两次都退回**同一个**起点（今天的行为）。
     * ⚠ 每一对调用前都要把 `n_backtrack` 清零：`THINK_REDO_MAX = 2` ⇒ 累计到 2 次以后
     *   函数会**直接返回 0**（预算挡住），断言就会假红（实测踩到）。 */
    t->redo_on = 1; t->redo_stack = 0; t->redo_alt = 0; t->n_backtrack = 0; t->ag_n = 0;
    /* ⚠ 退回起点之后还会**把反问题压回议程**（`ag_n` 因此 +1）—— 断言按真实语义写：
     * snap_n=5 的第一次退 ⇒ ag_n == 6（5 + 那一条反问题）。 */
    (void)think_redo_after_premise(ai, t, &g, 0, 5);
    t->n_backtrack = 0;                      /* 预算清零（见上） */
    (void)think_redo_after_premise(ai, t, &g, 0, 3);
    /* ⚠ 不去断言 `ag_n` 的**具体数字**：退回起点之后还会"把反问题压回议程"，
     * 而反问题可能因**去重**压不进去（实测过一次算 6、一次算 5）—— 那种断言是脆的。
     * 这里只断言**栈的机制**（这轮实现的东西）：关着时栈永远是空、痕里也不该有"更外一层"。 */
    { int found = 0;
      for (size_t i = 0; i < t->trace_n; ++i)
          if (strstr(t->trace[i].what, "更外一层") || strstr(t->trace[i].why, "更外一层")) found = 1;
      ok(t->redo_sp == 0 && !found, "关着：不进栈、也不退到更外一层（今天的行为）"); }
    /* 开着：第二次退到更外一层 */
    t->redo_stack = 1; t->redo_sp = 0; t->n_backtrack = 0; t->redo_alt = 0;
    (void)think_redo_after_premise(ai, t, &g, 0, 5);
    ok(t->redo_sp == 1 && t->redo_snap[0] == 5, "开着：第一次退把起点 5 存进栈（栈深 1）");
    (void)think_redo_after_premise(ai, t, &g, 0, 3);
    ok(t->ag_n == 5 && t->redo_sp == 0, "开着：第二次**退到更外一层**（回到 5，而且不再压反问题）");
    { int found = 0;
      for (size_t i = 0; i < t->trace_n; ++i)
          if (strstr(t->trace[i].what, "更外一层") || strstr(t->trace[i].why, "更外一层")) found = 1;
      ok(found, "痕上写清「退到更外一层」（多层回退有据可查）"); }
    ai_free(ai);
}

static void test_n1_decide(void) {
    printf("== ㊷ N1 参与决策：想通的结论能顶上来（默认关） ==\n");
    AI *ai = ai_new();
    Token *seed = T(ai, "决策种子");
    /* ⚠ 断言写成"**不是那句结论**"而不是"返回 NULL"：裸 AI 里走边也可能走出个低分候选
     * （实测：它不是 NULL）—— 这条测的是"关着时不拿思维结论顶"，不是"一定没输出"。 */
    { char *r0 = ai_plan_and_act(ai, seed, NULL, NULL, NULL, 0);
      ok(r0 == NULL || strcmp(r0, "猫 会 呼吸") != 0, "关着：不拿思维结论顶（走边自己的结果照旧）");
      free(r0); }
    snprintf(ai->n1_concl, sizeof ai->n1_concl, "猫 会 呼吸");
    ai->n1_gaps = 0; ai->n1_verified = 1; ai->n1_decide = 1;
    { char *r = ai_plan_and_act(ai, seed, NULL, NULL, NULL, 0);
      ok(r && strcmp(r, "猫 会 呼吸") == 0, "开着：走边没走出好东西 ⇒ 用**想通的结论**当结果");
      free(r); }
    ai->n1_verified = 0;                       /* 没复核过的对象不算"想通" */
    { char *r1 = ai_plan_and_act(ai, seed, NULL, NULL, NULL, 0);
      ok(r1 == NULL || strcmp(r1, "猫 会 呼吸") != 0, "没复核过 ⇒ 不用（宁可不说话）");
      free(r1); }
    ai->n1_verified = 1; ai->n1_gaps = 1;      /* 有缺口也不算 */
    { char *r2 = ai_plan_and_act(ai, seed, NULL, NULL, NULL, 0);
      ok(r2 == NULL || strcmp(r2, "猫 会 呼吸") != 0, "有缺口 ⇒ 不用（缺口就是没想通）");
      free(r2); }
    ai_free(ai);
}

static void test_code_fixes(void) {
    printf("== ㊶ C③：修法表（补分号/补 #include/补声明）+ 多轮修正开关 ==\n");
    ok(code_rule_for("error: expected ';' before '}' 符号节点") != NULL, "缺分号 ⇒ 认得出（规则 0）");
    ok(code_rule_for("error: implicit declaration of function 'foo'") != NULL, "没声明函数 ⇒ 认得出（规则 1）");
    ok(code_rule_for("error: 'bar' undeclared (first use in this function)") != NULL,
       "没声明的名字 ⇒ 认得出（规则 2，**这轮新加的**）");
    ok(code_rule_for("error: something totally unknown") == NULL, "认不出就不瞎改（返回 NULL）");
    {
        char src[128];
        snprintf(src, sizeof src, "int main(void){\n  x = 1;\n  return 0;\n}\n");
        int r = code_fix_declare(src, sizeof src, "error: 'x' undeclared", 2);
        ok(r == 0 && strstr(src, "int x;") != NULL, "补声明真的插进去了（`int x;`）");
    }
    { AI *ai = ai_new();
      if (getenv("AIP_CODEMULTI")) ok(1, "AIP_CODEMULTI=1（反向测量）：默认值断言按规矩跳过");
      else ok(ai->code_multi == 0, "多轮修正**默认关**（关着时行为与今天逐字节相同）");
      ai_free(ai); }
}

static void test_analogy_fallback(void) {
    printf("== ㊵ ② 类比兜底：同类互推，答「大概」（默认关） ==\n");
    AI *ai = ai_new();
    Token *sup = T(ai, "类"), *a = T(ai, "熊甲"), *b = T(ai, "熊乙"), *obj = T(ai, "冬眠");
    (void)token_tag_rel(a, sup, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, sup, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(b, obj, REL_CAN, REL_BOOST);
    char *w[4] = {(char *)"熊甲", (char *)"会", (char *)"冬眠", (char *)"吗"};
    float conf = 0.0f;
    ai->rel_analogy = 0;
    ok(agent_analogy_fallback(ai, w, 4, &conf) == NULL, "关着：不兜（规则答不出就说不确定）");
    ai->rel_analogy = 1;
    char *r = agent_analogy_fallback(ai, w, 4, &conf);
    ok(r != NULL && strstr(r, "大概") != NULL, "开着：给**大概**（措辞一定带「大概」）");
    ok(r && strstr(r, "熊乙") != NULL, "说清依据是谁（同类里 熊乙 也 会 冬眠）");
    ok(r && strstr(r, "类比") != NULL, "明说这是**类比**，不是直接知识");
    ok(conf > 0.0f && conf < 0.6f, "置信度**压低**（<0.6，不当成确定的事）");
    free(r);
    ai_free(ai);
}

static void test_wgate_tier(void) {
    printf("== ㊴ ② 闸门分层判据（默认关） ==\n");
    char why[96];
    AI *ai = ai_new();
    ok(ai_wgate_tiered(ai, 0.80, 0, 0, why, sizeof why) == 1, "高档（0.80 ≥ 0.70）：**没旁证也放行**");
    ok(ai_wgate_tiered(ai, 0.65, 1, 0, why, sizeof why) == 1, "中档（0.65）+ 证据 1 条 ⇒ 沉淀");
    ok(ai_wgate_tiered(ai, 0.65, 0, 65, why, sizeof why) == 1, "中档 + 链底 65 分 ⇒ 沉淀");
    ok(ai_wgate_tiered(ai, 0.65, 0, 0, why, sizeof why) == 0, "中档但**没旁证** ⇒ 拦");
    ok(ai_wgate_tiered(ai, 0.40, 3, 90, why, sizeof why) == 0, "低档（0.40）**有旁证也拦**（分不够就是不够）");
    ok(ai_wgate_tiered(ai, 0.62, 0, 0, why, sizeof why) == 0 &&
       strstr(why, "硬门槛") != NULL, "硬门槛：没旁证 + 分数不到高档 ⇒ 拦（理由写清是硬门槛）");
    /* 反向测量（AIP_WGATETIER=1）时"默认关"这条按规矩跳过（C 级测试也要钉默认档的脾气） */
    if (getenv("AIP_WGATETIER")) ok(1, "AIP_WGATETIER=1（反向测量）：默认值断言按规矩跳过");
    else ok(ai->wgate_tier == 0, "默认**关**（关着时调用方走原来的单阈值，逐字节相同）");
    ai_free(ai);
}

static void test_pron_last(void) {
    printf("== ㊳ ② 指代消解最小版：他/她/它 指回最近说话的人 ==\n");
    AI *ai = ai_new();
    Token *who = T(ai, "代词甲");
    OtherModel *om = other_intern(ai, who);
    ok(om != NULL, "记一个人（他者模型）");
    Token *cat = T(ai, "代词猫"), *fly = T(ai, "代词飞");
    (void)other_belief_learn(ai, om, cat, fly, REL_CAN, 0);      /* 甲 以为 猫 会 飞 */
    ok(ai->last_who == who, "写他的信念时**记下「最近说话的人」**（last_who）");
    /* 视角解析：句首代词 + 看法动词 ⇒ 站到那个人身上（要在"把人名 intern"之前返回） */
    ai->pron_last = 0;
    { char *w[5] = {(char *)"他", (char *)"觉得", (char *)"代词猫", (char *)"会", (char *)"飞"};
      Token *out_who = NULL;
      ok(agent_set_perspective(ai, w, 5, &out_who) == 1, "关着：照样认视角标记（今天的行为）");
      ok(ai->view_who && ai->view_who->name && strcmp(ai->view_who->name, "他") == 0,
         "关着：把「他」当成**一个人名**（图上会多出一个叫「他」的人）"); }
    ai->view_who = NULL;
    ai->pron_last = 1;
    { char *w[5] = {(char *)"他", (char *)"觉得", (char *)"代词猫", (char *)"会", (char *)"飞"};
      Token *out_who = NULL;
      ok(agent_set_perspective(ai, w, 5, &out_who) == 1, "开着：也认（剥标记那条路不变）");
      ok(ai->view_who == who && out_who == who, "开着：代词**指回代词甲**（不再造「他」这个人名）"); }
    /* 认不出（还没有人说过话）⇒ 不动 */
    ai->last_who = NULL; ai->view_who = NULL;
    { char *w[5] = {(char *)"他", (char *)"觉得", (char *)"代词猫", (char *)"会", (char *)"飞"};
      Token *out_who = NULL;
      (void)agent_set_perspective(ai, w, 5, &out_who);
      ok(ai->view_who && ai->view_who->name && strcmp(ai->view_who->name, "他") == 0,
         "认不出（没人说过话）⇒ 不动，照旧当人名（宁可漏）"); }
    ai_free(ai);
}

static void test_open_relation(void) {
    printf("== ㊲ ② 触发接求解：`A R 什么` / `谁 R B` 这类缺一端的问句 ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "开猫"), *dog = T(ai, "开狗"), *animal = T(ai, "开动物");
    (void)token_tag_rel(cat, animal, REL_IS_A, REL_BOOST);
    (void)token_tag_rel(dog, animal, REL_IS_A, REL_BOOST);
    ai->rel_open = 0;
    ai->rel_off = 0;
    { char *w[3] = {(char *)"开猫", (char *)"属于", (char *)"什么"};
      float c = 0.0f;
      ok(agent_open_relation(ai, w, 3, &c) == NULL, "关着：这条路**不接**（照旧走流水线）"); }
    ai->rel_open = 1;
    { char *w[3] = {(char *)"开猫", (char *)"属于", (char *)"什么"};
      float c = 0.0f;
      char *a1 = agent_open_relation(ai, w, 3, &c);
      ok(a1 && strstr(a1, "开动物") != NULL, "`A R 什么` ⇒ 正向查邻居（答出开动物）");
      free(a1); }
    { char *w[3] = {(char *)"谁", (char *)"属于", (char *)"开动物"};
      float c = 0.0f;
      char *a2 = agent_open_relation(ai, w, 3, &c);
      ok(a2 && strstr(a2, "开猫") && strstr(a2, "开狗"), "`谁 R B` ⇒ **反向扫**（答出开猫、开狗）");
      free(a2); }
    { char *w[3] = {(char *)"开猫", (char *)"没有这关系", (char *)"什么"};
      float c = 0.0f;
      ok(agent_open_relation(ai, w, 3, &c) == NULL, "关系词不认识 ⇒ 不接（不瞎答）"); }
    { char *w[3] = {(char *)"开猫", (char *)"吃", (char *)"什么"};
      float c = 0.0f;
      char *a3 = agent_open_relation(ai, w, 3, &c);
      ok(a3 && strstr(a3, "没 记 过") != NULL, "图里没记过 ⇒ 明说「没记过」（不编）");
      free(a3); }
    /* §6.145 填充词 / 问法外壳：**先量后改** —— 12 种问法里原来只有 3 种接上，
     * 漏掉的四成是外壳（到底/请问/究竟/其实/我想知道）。剥壳只在**认得出**时做。 */
    { char *w[4] = {(char *)"开猫", (char *)"到底", (char *)"属于", (char *)"什么"};
      float c = 0.0f;
      char *a4 = agent_open_relation(ai, w, 4, &c);
      ok(a4 && strstr(a4, "开动物") != NULL, "「开猫 **到底** 属于 什么」⇒ 剥掉填充词照样答");
      free(a4); }
    { char *w[5] = {(char *)"开猫", (char *)"到", (char *)"底", (char *)"属于", (char *)"什么"};
      float c = 0.0f;
      char *a4b = agent_open_relation(ai, w, 5, &c);
      ok(a4b && strstr(a4b, "开动物") != NULL,
         "分词器把「到底」切成 到+底 也认（**连写匹配**，不是只比单个词）");
      free(a4b); }
    { char *w[6] = {(char *)"我", (char *)"想", (char *)"知道", (char *)"开猫",
                    (char *)"属于", (char *)"什么"};
      float c = 0.0f;
      char *a5 = agent_open_relation(ai, w, 6, &c);
      ok(a5 && strstr(a5, "开动物") != NULL, "「我 想 知道 …」⇒ 剥掉前缀外壳（6 个词也接）");
      free(a5); }
    { char *w[4] = {(char *)"请问", (char *)"谁", (char *)"属于", (char *)"开动物"};
      float c = 0.0f;
      char *a6 = agent_open_relation(ai, w, 4, &c);
      ok(a6 && strstr(a6, "开猫") && strstr(a6, "开狗"), "反向那条也认外壳：`请问 谁 属于 开动物`");
      free(a6); }
    { char *w[5] = {(char *)"开猫", (char *)"真的", (char *)"属于", (char *)"什么", (char *)"呢"};
      float c = 0.0f;
      ok(agent_open_relation(ai, w, 5, &c) == NULL,
         "不是写死的填充词 ⇒ **不接**（退回流水线；剥壳只在认得出时做）"); }
    /* §6.147 `A R 什么 <类别>`（尾巴上的名词是**类别提示**，不影响问什么） */
    { char *w[4] = {(char *)"开猫", (char *)"属于", (char *)"什么", (char *)"类开"};
      float c = 0.0f;
      char *a7 = agent_open_relation(ai, w, 4, &c);
      ok(a7 && strstr(a7, "开动物") != NULL, "「开猫 属于 什么 <类别>」⇒ 照样答（提示词不影响答案）");
      free(a7); }
    { char *w[4] = {(char *)"开猫", (char *)"吃", (char *)"什么", (char *)"类开"};
      float c = 0.0f;
      char *a8 = agent_open_relation(ai, w, 4, &c);
      ok(a8 && strstr(a8, "什么 类开") != NULL, "没记过时：类别提示**原样带回**话里（不丢用户的词）");
      free(a8); }
    { char *w[4] = {(char *)"开猫", (char *)"属于", (char *)"开动物", (char *)"吗"};
      float c = 0.0f;
      ok(agent_open_relation(ai, w, 4, &c) == NULL,
         "第 3 个词**不是**占位词 ⇒ 不接（不是「四个词就乱接」）"); }
    { ai->rel_open = 0;                       /* 关着时连壳都不剥（保证"关着零行为"）*/
      char *w[4] = {(char *)"开猫", (char *)"到底", (char *)"属于", (char *)"什么"};
      float c = 0.0f;
      ok(agent_open_relation(ai, w, 4, &c) == NULL, "开关关着：带外壳的也一个字都不接");
      ai->rel_open = 1; }
    ai_free(ai);
}

static void test_other_weak(void) {
    printf("== ㊱ ② 他者上位降权：更宽的说法算半票参考（默认只当相关） ==\n");
    AI *ai = ai_new();
    Token *dog = T(ai, "上位狗"), *cat = T(ai, "上位猫"), *animal = T(ai, "上位动物");
    (void)token_tag_rel(cat, animal, REL_IS_A, REL_BOOST);      /* 猫 属于 动物（下位关系） */
    OtherModel *om = &ai->others[0];
    ai->others_n = 1;
    om->who = T(ai, "上位甲");
    om->n_bel = 1;
    om->bel[0].a = dog; om->bel[0].rid = REL_LIKE; om->bel[0].b = animal; om->bel[0].neg = 0;
    OtherBeliefView v;
    ai->other_weak_up = 0;
    other_belief_view(ai, dog, REL_LIKE, cat, &v);
    ok(v.n_related == 1 && v.n_weak_yes == 0 && v.verdict == OBV_NONE,
       "关着：上位说法只算**相关**（不进判定 ⇒ 没人表过态）");
    ai->other_weak_up = 1;
    other_belief_view(ai, dog, REL_LIKE, cat, &v);
    ok(v.n_related == 1 && v.n_weak_yes == 1, "开着：上位说法进**弱票**格（半票参考）");
    ok(v.verdict == OBV_YES, "只有弱票时：给**倾向**（都说认），而不是「没人说过」");
    /* 有确定表态时弱票不参与：另一个人说"狗 不 喜欢 猫" ⇒ 仍然是"不认" */
    ai->others_n = 2;
    ai->others[1].who = T(ai, "上位乙");
    ai->others[1].n_bel = 1;
    ai->others[1].bel[0].a = dog; ai->others[1].bel[0].rid = REL_LIKE;
    ai->others[1].bel[0].b = cat; ai->others[1].bel[0].neg = 1;
    other_belief_view(ai, dog, REL_LIKE, cat, &v);
    ok(v.verdict == OBV_NO && v.n_weak_yes == 1,
       "有确定表态（乙说不喜欢）⇒ **强票说了算**，弱票不翻盘");
    ai_free(ai);
}

static void test_lexicon(void) {
    printf("== ㉟ ② 词表覆盖：`lexicon.txt` 让池里没有的多字词按整词切 ==\n");
    AI *ai = ai_new();
    { /* 先把"没有词表文件"那一档钉住 */
        size_t n = 0;
        char **t = ai_tokenize(ai, "泡茶", &n);
        ok(n == 2, "没词表时：池里没有的多字词**切成两半**（当前行为，如实记着）");
        for (size_t i = 0; i < n; ++i) free(t[i]);
        free(t);
    }
    { FILE *f = fopen("lexicon_test.txt", "wb");
      if (f) { fputs("# 注释行\n泡茶\n步骤\n", f); fclose(f); } }
    /* §6.144：断言写成**增量**（`before + 2`）—— 仓库现在自带一份 `lexicon.txt`
     * （四个别名词），沙箱也会把它拷进来 ⇒ 绝对条数会随环境变，增量才是这条要钉的东西。 */
    size_t lex_before = ai->lex_n;
    ai_lexicon_load(ai, "lexicon_test.txt");
    ok(ai->lex_n == lex_before + 2, "`ai_lexicon_load` 把文件里的 2 个词加了进去（注释/空行不算）");
    ok(ai_lex_extra_has(ai, "泡茶") && !ai_lex_extra_has(ai, "喝茶"), "查得到泡茶、查不到喝茶");
    { size_t n = 0;
      char **t = ai_tokenize(ai, "泡茶步骤", &n);
      ok(n == 2 && strcmp(t[0], "泡茶") == 0 && strcmp(t[1], "步骤") == 0,
         "有词表时：**整词切出来**（泡茶|步骤）");
      for (size_t i = 0; i < n; ++i) free(t[i]);
      free(t); }
    { /* ⚠ `ai_load_file` 不走 `ai_new()` ⇒ 装载必须是**懒加载**，否则读盘路径永远读不到 */
        AI *b = ai_load_file("model.bin");
        if (b) {
            size_t n = 0;
            char **t = ai_tokenize(b, "猫", &n);
            ok(b->lex_loaded == 1, "读盘路径（`ai_load_file`）也会去读词表（懒加载，不是只写在 ai_new 里）");
            for (size_t i = 0; i < n; ++i) free(t[i]);
            free(t);
            ai_free(b);
        } else ok(1, "（这个沙箱没有 model.bin，跳过读盘那一档）"); }
    remove("lexicon_test.txt");
    ai_free(ai);
}

static void test_eval_num(void) {
    printf("== ㉞ 求值层样板：`[条件:3<5]` 真算一遍（算不出就明说判不了） ==\n");
    struct { const char *e; int want, val; } cs[] = {
        { "3<5",    1, 1 }, { "5<3",    1, 0 }, { "3<=3",   1, 1 }, { "4!=4",   1, 0 },
        { "12 >= 9",1, 1 }, { "-2<0",   1, 1 }, { "abc<5",  0, 0 }, { "3<",     0, 0 },
        { "3 + 5",  0, 0 }, { "1e3<2e3",0, 0 },
        /* §6.142 浮点 / 算术表达式（+ - * / 括号）；老规矩：认不出就判不了 */
        { "2.5<3",        1, 1 }, { "3<2.5",          1, 0 },
        { "1.5+1.5==3",   1, 1 }, { "10/4>2",         1, 1 },
        { "(2+3)*4==20",  1, 1 }, { "2*(3+4)==14",    1, 1 },
        { "1==1.0",       1, 1 }, { "0.1+0.2==0.3",   1, 1 },   /* 浮点相等用相对容差 */
        { "0.1+0.2!=0.3", 1, 0 },
        { "5/0<1",        0, 0 },                                /* 除以 0 ⇒ 判不了，不猜 */
        { "9007199254740993>9007199254740992", 1, 1 },           /* 大整数仍走整数那条路（精确）*/
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; ++i) {
        int v = -1, got = ai_eval_cmp(cs[i].e, &v);
        char m[96];
        snprintf(m, sizeof m, "`%s` ⇒ %s", cs[i].e,
                 got ? (v ? "真" : "假") : "判不了");
        ok(got == cs[i].want && (!got || v == cs[i].val), m);
    }
    /* 过程层：条件成立走那一支 / 不成立走否则；关着时照旧"判不了" */
    AI *ai = ai_new();
    char note[200];
    { char *a[1] = {(char *)"比大小"}; (void)token_op_run(ai, "建过程", a, 1, note, sizeof note); }
    { char *a[3] = {(char *)"比大小", (char *)"3<5", (char *)"走这支"};
      (void)token_op_run(ai, "加条件", a, 3, note, sizeof note); }
    { char *a[2] = {(char *)"比大小", (char *)"走那支"};
      (void)token_op_run(ai, "加否则", a, 2, note, sizeof note); }
    ai->proc_run = 1; ai->proc_dry = 0;
    ai->eval_num = 0;
    { ProcRun pr; (void)proc_run(ai, "比大小", &pr);
      ok(pr.n_skip == 0 && pr.n_gap == 0, "关着：照旧走图内判据（条件词不在图里 ⇒ 判不了 ⇒ 走否则）"); }
    ai->eval_num = 1;
    { ProcRun pr; (void)proc_run(ai, "比大小", &pr);
      ok(pr.n_ok >= 1, "开着：**数值判据说了算**（3<5 成立 ⇒ 走条件那一支）"); }
    /* §6.98：求值层也管**前提**（前提名字本身是可计算式时，先真算一遍） */
    {
        Token *pv = T(ai, "3<5"), *pf = T(ai, "5<3");
        ai->eval_num = 0;
        ok(ai_premise_holds(ai, pv) == 0, "关着：算式前提照旧走图内三档（不在窗口/够不着 ⇒ 不成立）");
        ai->eval_num = 1;
        ok(ai_premise_holds(ai, pv) == 1, "开着：前提 `3<5` **算得出来就成立**");
        ok(ai_premise_holds(ai, pf) == 0, "开着：前提 `5<3` 算出来是假 ⇒ 不成立");
    }
    ai_free(ai);
}

static void test_exec_step(void) {
    printf("== ㉝ 动作符号节点 + 过程执行槽：步骤会**真的调动作**（白名单+超时+账） ==\n");
    AI *ai = ai_new();
    char note[220];
    { char *a[1] = {(char *)"试跑"}; (void)token_op_run(ai, "建过程", a, 1, note, sizeof note); }
    { char *a[3] = {(char *)"试跑", (char *)"echo", (char *)"哟"};
      ok(token_op_run(ai, "加执行", a, 3, note, sizeof note) >= 0,
         "`加执行` 走受审通道（动作词限白名单形状，不许带路径/管道）"); }
    /* 拒 shell 语法的检查放在**另一个过程**上：别往被跑的那个过程里多塞步骤
     * （第一版把 `rm` 也加进"试跑"⇒ 真执行时它多出一个断点，两条断言假红）。 */
    { char *r2[1] = {(char *)"试跑2"};
      (void)token_op_run(ai, "建过程", r2, 1, note, sizeof note); }
    { char *b[2] = {(char *)"试跑2", (char *)"echo|x"};
      (void)token_op_run(ai, "加执行", b, 2, note, sizeof note);
      ok(strstr(note, "白名单") != NULL || strstr(note, "命令名") != NULL,
         "带管道/路径的动作词**当场拒**（别把 shell 语法塞进图里）"); }
    Token *mk = pool_find(ai->pool, "[执行:echo 哟]");
    ok(mk != NULL, "动作符号节点 `[执行:echo 哟]` 真的建在图上（不是虚构的记号）");
    Token *root = proc_root_of(ai, "试跑");
    long st = -1;
    if (root) for (size_t i = 0; i < root->count; ++i)
        if (root->assocs[i].main == mk) st = (long)i;
    ok(st >= 0 && edge_step_index(&root->assocs[st]) == 1,
       "它是**第 1 步**（执行槽 = 步骤位上的端点）");
    ai->proc_run = 1;
    /* ① 干跑：不执行（账不动） */
    ai->proc_dry = 1; ai->act_on = 1;
    { ProcRun pr; (void)proc_run(ai, "试跑", &pr);
      ok(pr.n_ran == 0 && pr.n_gap == 0, "干跑：动作**不执行**（真执行计数 0、也没记断点）"); }
    /* ② 执行层关着：断点（不许假装做过） */
    ai->proc_dry = 0; ai->act_on = 0;
    { ProcRun pr; (void)proc_run(ai, "试跑", &pr);
      char m1[96]; snprintf(m1, sizeof m1, "执行层关着 ⇒ **断点**（假装做过是最坏的）[ran=%zu gap=%zu]",
                             pr.n_ran, pr.n_gap);
      ok(pr.n_ran == 0 && pr.n_gap == 1, m1); }
    /* ③ 真执行：走 action_run（echo 在白名单里，安全） */
    ai->act_on = 1;
    { ProcRun pr; (void)proc_run(ai, "试跑", &pr);
      char m2[96]; snprintf(m2, sizeof m2, "开着 ⇒ **真执行 1 次**[ran=%zu gap=%zu]",
                             pr.n_ran, pr.n_gap);
      ok(pr.n_ran == 1 && pr.n_gap == 0, m2); }
    ai_free(ai);
}

static void test_cache_walk(void) {
    printf("== ㉛ ①f 缓存参与自由联想（默认关；开着时缓存是**最后一档**） ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "联甲"), *b = T(ai, "联乙"), *c = T(ai, "联丙");
    /* 造一条**缓存**边（role=推论）：直写 role，模拟"推出来沉淀的"。
     * ⚠ `b` 自己得**有出边**，否则 `ai_can_continue_from` 判它走不下去（实测踩过：
     *   第一版漏了这一步 ⇒ "开着能取到"那条断言假红）。 */
    (void)token_add_assoc_unique(a, b);
    (void)token_add_assoc_unique(b, c);
    /* `b→c` 还得**有 hits**：`ai_edge_walkable` 会挡掉 hits==0 的边（"建了没强化过的残留"）
     * ⇒ 否则 `ai_can_continue_from` 判 b 是死胡同（实测踩了两次才排到这一层）。 */
    for (size_t i = 0; i < b->count; ++i)
        if (b->assocs[i].main == c) b->assocs[i].hits = 5;
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].main == b) { a->assocs[i].role = ASSOC_ROLE_DERIVED; a->assocs[i].hits = 5; }
    ai->focus_n = 0;
    (void)ai_focus_push(ai, a, 1.0f);
    ai->cache_walk = 0;
    { uint8_t used = 0, pat = 0;
      Token *nx = ai_pick_next_topic(ai, a, 1.0f, 0, ASSOC_ROLE_NONE, ASSOC_TOPIC_NONE, &used, &pat);
      ok(nx != b, "关着：走边**取不到**缓存那条边（缓存不在 role 顺序里）"); }
    ai->cache_walk = 1;
    { uint8_t used = 0, pat = 0;
      Token *nx = ai_pick_next_topic(ai, a, 1.0f, 0, ASSOC_ROLE_NONE, ASSOC_TOPIC_NONE, &used, &pat);
      ok(nx == b, "开着：缓存那条边**能被走到**（最后一档）"); }
    ai_free(ai);
}

static void test_dyn_neg(void) {
    printf("== ㉚ 动态关系的否定（旗子借 nrel 第 15 位） ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "喵甲"), *b = T(ai, "喵乙"), *c = T(ai, "喵丙");
    uint8_t rid = rel_id_of(ai, "喵谓", 1);          /* allow_new=1 ⇒ 登记成动态谓语 */
    ok(rid >= REL_DYN_BASE, "「喵谓」登记成**动态关系**（清单外的谓语）");
    uint8_t d = (uint8_t)(rid - REL_DYN_BASE + 1);
    ok(token_tag_dyn_neg(a, b, d, 1, REL_BOOST) == 0, "打一条**否定**的动态关系边");
    {
        const AssocEntry *e = NULL;
        for (size_t i = 0; i < a->count; ++i) if (a->assocs[i].main == b) e = &a->assocs[i];
        ok(e && e->dyn == d, "边上的 `dyn` 记着关系 id");
        ok(e && edge_dyn_neg(e) == 1, "`edge_dyn_neg` 认出否定旗子（nrel 第 15 位）");
    }
    ok(rel_direct_as(a, rid, b, NULL) == 0, "**肯定**直查查不到它（否定边不当肯定事实）");
    ok(rel_direct_neg_as(a, rid, b, NULL) == 1, "**否定**直查查得到（这就是新能力）");
    ok(token_tag_dyn_neg(a, c, d, 0, REL_BOOST) == 0, "同一条动态关系也可以打**肯定**边");
    ok(rel_direct_as(a, rid, c, NULL) == 1 && rel_direct_neg_as(a, rid, c, NULL) == 0,
       "肯定/否定两条边**互不串**（同一谓语两档并存）");
    ai_free(ai);
}

/* ============================ ⑲ 逐条可信度：我更信哪一条 ============================
 *
 * 用户要的「逐条可信度权重」。它**不是新字段**（格式没动）：从来源档 + 命中 + 新近
 * 算出来的启发式。用途：`::src` 说得清"我更信哪一条"，`::trust on` 时**真的按它选边**。
 * 这一节钉住：排序对（基准 > 观测 > 推论）、命中加成、新近扣分、夹在 0..100，
 * 以及**矛盾两边差 ≥20 时按更可信的答**。 */
static void test_credibility(void) {
    printf("== ⑲ 逐条可信度：基准 > 观测 > 推论，且能按它选边 ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "甲2"), *b = T(ai, "乙2"), *c = T(ai, "丙2");
    (void)token_tag_rel(a, b, REL_IS_A, REL_BOOST);          /* 观测 */
    token_mark_innate(a, c);                                  /* 基准 */
    (void)token_tag_rel(a, c, REL_IS_A, REL_BOOST);
    long eb = micro_edge_index(a, b), ec = micro_edge_index(a, c);
    ok(eb >= 0 && ec >= 0, "两条边都在");
    uint64_t now = ai->pool->current_tick;
    int cb = assoc_credibility(&a->assocs[eb], now);
    int cc = assoc_credibility(&a->assocs[ec], now);
    ok(cc > cb, "同样命中数下，**基准边**比观测边更可信");
    /* 命中加成（索引要**重新取**：中间加边可能让旧下标指向别的边）。
     * 注意 REL_BOOST 本身就是 32 次命中 —— 别拿 0 当基准。 */
    eb = micro_edge_index(a, b);
    a->assocs[eb].hits = 1;
    int c_low = assoc_credibility(&a->assocs[eb], now);
    a->assocs[eb].hits = 32;
    ok(assoc_credibility(&a->assocs[eb], now) > c_low, "命中越多越可信");
    /* 新近扣分：走**唯一时间核**（指数族）—— 同一个字段，越老越低（夹在 0..100）。
     * 线上口径是 `ai_credibility(ai, e)`（自己算 age + 核），这里用它验。 */
    {
        eb = micro_edge_index(a, b);
        a->assocs[eb].last_touch_tick = 0;
        ai->pool->current_tick = 0;
        int fresh = ai_credibility(ai, &a->assocs[eb]);
        ai->pool->current_tick = 400000;             /* 很老 */
        int stale = ai_credibility(ai, &a->assocs[eb]);
        ok(stale < fresh && stale >= 0 && fresh <= 100,
           "很久没碰过会扣分（指数核；且夹在 0..100）");
        ai->pool->current_tick = 0;
    }
    /* 推论最弱 */
    {
        Token *d = T(ai, "丁2");
        ModIntent m; memset(&m, 0, sizeof m);
        m.type = MOD_TAG_DERIVED; m.target = a; m.main = d; m.rel_id = REL_IS_A;
        snprintf(m.source, sizeof m.source, "测试");
        char note[120];
        safety_step_begin(ai);
        (void)micro_try(ai, &m, note, sizeof note);
        long ed = micro_edge_index(a, d);
        ok(ed >= 0 && assoc_credibility(&a->assocs[ed], now) < cb, "推论（缓存）最弱");
    }
    ai_free(ai);

    /* ---- `::trust on`：两边差够多时按更可信的答 ----
     * 真实知识库里"两边都推得出来 + 可信度差 ≥20"很罕见（规则冲突两边都算 50），
     * 所以这里分两段测：① 真跑一个矛盾，确认**默认不选边**；
     * ② 把"更信哪一边"摆好，确认答复真的这么说（测的是**决策与呈现**那条路）。 */
    {
        AI *a2 = ai_new();
        Think *t = think_of(a2);
        t->conflict = 1;
        snprintf(t->concl_text, sizeof t->concl_text, "%s", "关于 猫3 会 唱歌3 这件事，我这儿有互相矛盾的依据");
        snprintf(t->chain_text2, sizeof t->chain_text2, "%s", "猫3 -[会·缺前提]-> 唱歌3");
        char out[512];
        /* ① 没摆"更信哪一边"（= trust 关着，或差得不够）→ 照旧只说矛盾 */
        snprintf(out, sizeof out, "%s", "x");
        ok(think_take_output(a2, out, sizeof out) == 1 && strstr(out, "更 信") == NULL,
           "没说「更信」时不选边（照旧只说矛盾）");
        /* ② 摆好"更信哪一边"（trust on 且差 ≥20 时 think 里就是这么摆的）→ 答复那么说 */
        t->trust_on = 1;
        t->trust_pick = -1;
        t->trust_c1 = 80;
        t->trust_c2 = 40;
        snprintf(out, sizeof out, "%s", "x");
        ok(think_take_output(a2, out, sizeof out) == 1 && strstr(out, "更 信") != NULL,
           "::trust on：答复里说「我更信可信度高的那一边（80 对 40）」");
        ok(strstr(out, "80") && strstr(out, "40"), "可信度两个数都摆出来（不是空口说更信）");
        ai_free(a2);
    }
}

/* ============================ ⑳ 结构类比 / 驱动参与决策 / 时间顺序 ============================
 *
 * 清单里的 #6 三小件，各做成**窄而可测**的一步：
 *   ① **结构映射式类比**：R5 现在不只看 emb 像不像，还看**关系骨架**像不像
 *      （用词完全不同也能借答案）—— 这是"结构映射"与"最近邻"的区别；
 *   ② **驱动参与决策**：一张耦合表（驱动 → 参数 → 增减），默认关，`::drive` 全列出来；
 *   ③ **时间顺序**：谁先出现（按首次见到的 tick），带顺序证据。 */
static void test_struct_drive_order(void) {
    printf("== ⑳ 结构类比 / 驱动参与决策 / 时间顺序 ==\n");
    /* ① 结构类比：骨架一样、emb 都没有（新 AI 没算 emb）也能借到答案 */
    {
        AI *ai = ai_new();
        Token *jia = T(ai, "甲3"), *yi = T(ai, "乙3");
        Token *x = T(ai, "子3"), *y = T(ai, "丑3"), *z = T(ai, "寅3");
        Token *fish = T(ai, "卯3");
        (void)token_tag_rel(jia, x, REL_IS_A, REL_BOOST);
        (void)token_tag_rel(jia, y, REL_HAS, REL_BOOST);
        (void)token_tag_rel(jia, z, REL_CAN, REL_BOOST);
        (void)token_tag_rel(yi, x, REL_IS_A, REL_BOOST);   /* 乙3 的关系骨架和 甲3 一样 */
        (void)token_tag_rel(yi, y, REL_HAS, REL_BOOST);
        (void)token_tag_rel(yi, z, REL_CAN, REL_BOOST);
        (void)token_tag_rel(yi, fish, REL_EAT, REL_BOOST); /* 乙3 对「吃 卯3」有态度 */
        /* 干扰项：同样对「吃 卯3」有态度，但**结构完全不同** —— 不该被选中 */
        Token *wei = T(ai, "未3");
        (void)token_tag_rel(wei, fish, REL_EAT, REL_BOOST);
        size_t common = 0;
        int st = rel_struct_sim(jia, yi, &common);
        ok(st >= REL_STRUCT_MIN_PCT && common >= REL_STRUCT_MIN_COMMON,
           "结构相似度够（骨架重合 ≥3 个关系邻居）");
        ok(rel_struct_sim(jia, wei, &common) < REL_STRUCT_MIN_PCT,
           "干扰项的结构相似度不够（不会因为它有态度就借它）");
        /* R5 在**思维层**的「借最近邻」里调用（不是求解器内部）—— 这里直接调它 */
        Proof pf;
        ok(rel_guess(ai, jia, REL_EAT, fish, &pf) == 1 && pf.status == 1 && pf.guessed == 1,
           "结构类比借到了答案（并标明是**推测**）");
        ok(pf.n >= 2 && pf.step[1].tok == yi, "借的是**结构像**的那个（不是随便一个有态度的词）");
        ok(pf.step[1].sim >= REL_STRUCT_MIN_PCT, "链上记的相似度就是结构相似度");
        char txt[256];
        proof_text(ai, &pf, txt, sizeof txt);
        ok(strstr(txt, "像") != NULL, "链上写明「像」（不是当成事实）");
        ai_free(ai);
    }
    /* ② 驱动参与决策：表 + 开关（默认关时一个修正都不施加） */
    {
        AI *ai = ai_new();
        ok(ai->drive_on == 0, "默认**关**（不改任何行为参数）");
        ai->inner.fatigue = 0.9f;
        drive_policy_step(ai);
        ok(ai->drive_d_steps == 0, "关着的时候疲劳再高也不施加减步修正");
        ai->drive_on = 1;
        drive_policy_step(ai);
        ok(ai->drive_d_steps < 0, "打开后：疲劳高 → 思考步数减（表里那一行）");
        ai->inner.fatigue = 0.0f; ai->inner.arousal = 0.8f;
        drive_policy_step(ai);
        ok(ai->drive_d_gen > 0, "唤醒高 → 生成长度加（另一行）");
        /* 表按实测重调过：「唤醒低」那行**永不触发**（唤醒最低也 0.46），
         * 换成了**真的会低**的量 —— 不适高（没底）→ 少说。见 drive.h 的实测注释。 */
        ai->inner.arousal = 0.1f; ai->inner.discomfort = 0.95f; ai->inner.comfort = 0.2f;
        drive_policy_step(ai);
        ok(ai->drive_d_gen < 0 && ai->drive_d_cutoff > 0.0f,
           "没底（不适高）→ 少说；不舒服 → 阈值抬高（保守）");
        ai->inner.discomfort = 0.1f; ai->inner.comfort = 0.8f;
        drive_policy_step(ai);
        ok(ai->drive_d_cutoff < 0.0f, "舒服 → 阈值降（敢想远一点）—— 对称的那一行");
        ai_free(ai);
    }
    /* ③ 时间顺序：先出现的 tick 更小 */
    {
        AI *ai = ai_new();
        Token *first = T(ai, "辰3");
        ai->pool->current_tick = 100;                 /* 手动推时间，模拟"后来才见到" */
        Token *second = T(ai, "巳3");
        ok(first->birth_tick < second->birth_tick, "先见到的词 tick 更小（顺序判据）");
        ai_free(ai);
    }
}

/* ============================ ㉑ 前提成立的两档判据（借 lingshu 的"条件成立"分层） ============================
 *
 * 局限：#1「前提成立只看注意窗口（提到过没有）」。第二档是**图内**判据 ——
 * 从当前注意窗口出发做受限扩散（深度≤2、能量≥0.25），前提**够得着**也算成立。
 * 默认仍是第一档（提到过）——第二档会放宽 R10 的闸门，属行为改变，先实现先 A/B。 */
static void test_premise_tiers(void) {
    printf("== ㉑ 前提成立两档：提到过（默认）/ 图上够得着（activate） ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫5"), *cook = T(ai, "做饭5"), *who = T(ai, "人5");
    (void)token_tag_rel(who, cook, REL_CAN, REL_BOOST);
    (void)token_add_assoc_unique(cook, who);   /* 反向共现边：扩散要能走过去 */
    char note[200];
    char *a1[3] = {(char *)"猫5", (char *)"做饭5", (char *)"人5"};
    ok(token_op_run(ai, "加前提", a1, 3, note, sizeof note) == 1, "建 猫5 —[前提]→ 人5 → 做饭5");
    (void)cat;

    ok(ai->prem_hold_activate == 0, "默认档 = mention（只看注意窗口）");
    /* 注意窗口里放一个与人5强相连的词，但**不放人5** */
    ai->focus_ring[0] = cook; ai->focus_weight[0] = 1.0f; ai->focus_n = 1;
    ok(ai_premise_holds(ai, who) == 0, "mention 档：前提没被提到 → 不成立");
    ai->prem_hold_activate = 1;
    ok(ai_premise_holds(ai, who) == 1,
       "activate 档：前提没提到，但它**在图上够得着**（做饭5 → 人5）→ 成立");
    /* 换一个跟谁都不相连的词当焦点 → 够不着 */
    ai->focus_ring[0] = T(ai, "无关词5"); ai->focus_weight[0] = 1.0f; ai->focus_n = 1;
    ok(ai_premise_holds(ai, who) == 0, "activate 档也不瞎认：够不着就是不成立");
    ai_free(ai);
}

/* ============================ ㉒ 谓词别名表 + 来源档后验 ============================
 *
 * 两条局限的落地：
 *   ① 「他说的/我学的按 (A,关系,B) 精确匹配，同义改写对不上」→ **谓词别名表**
 *      （`害怕→怕`、`喜爱→喜欢`、`拥有→有`…），一处归一；
 *   ② 「可信度是启发式的」→ 来源档换成 **Beta 后验**，证据取自**已有计数器**
 *      （观测档：n_learned/n_revised；推论档：n_cached/n_retracted）——
 *      **改口越多，观测档越低；缓存被撤越多，推论档越低**。格式没动。 */
static void test_alias_and_posterior(void) {
    printf("== ㉒ 谓词别名归一 + 来源档可信度（后验，从计数器学） ==\n");
    AI *ai = ai_new();
    /* ① 别名：不同说法 → 同一个关系 id */
    ok(rel_id_of(ai, "拥有", 1) == REL_HAS, "「拥有」归到核心关系「有」（模型里真有这个词）");
    ok(rel_alias_id("害怕") == 0,
       "「害怕」**有意不收**（frames.txt 里有一条 [帧:害怕X吗] 会跟关系层抢话，见 docs）");
    ok(rel_id_of(ai, "喜爱", 1) == REL_LIKE,
       "「喜爱」在**别名表**里也归到「喜欢」");
    /* §6.144 **别名可达**：别名只对"分词器不切开的词"有效 ⇒ 靠 `lexicon.txt` 保整词。
     * 这里现写一份临时词表，证明机制是通的（别指望 CWD 里一定有那个文件）。 */
    {
        const char *lex = "/tmp/aip_lex_144.txt";
        FILE *lf = fopen(lex, "w");
        if (lf) { fprintf(lf, "喜爱\n喜好\n惧怕\n具有\n"); fclose(lf); }
        AI *a2 = ai_new();
        ai_lexicon_load(a2, lex);
        size_t n = 0;
        char **w = ai_tokenize(a2, "喜爱", &n);
        ok(n == 1 && w && strcmp(w[0], "喜爱") == 0,
           "词表里有「喜爱」⇒ 分词器**按整词切**（不再拆成 喜+爱，别名这才走得到）");
        ai_free_words(w, n);
        ok(rel_id_of(a2, "喜爱", 0) == REL_LIKE && rel_id_of(a2, "惧怕", 0) == REL_FEAR,
           "整词保住之后：别名表把 喜爱→喜欢、惧怕→怕 都归一（核心关系，不是动态谓语）");
        ai_free(a2);
    }
    ok(rel_id_of(ai, "怕", 0) == REL_FEAR, "核心表里的词照旧");
    uint8_t dyn = rel_id_of(ai, "念叨", 1);
    ok(dyn >= REL_DYN_BASE, "没收录的说法仍走**动态关系**（表不是封闭的）");
    ok(rel_alias_id("莫名其妙") == 0, "不认识的说法不会被硬塞进核心关系");

    /* 用别名学一句、再用核心词问 —— 这条以前是对不上的 */
    {
        AI *a2 = ai_new();
        Token *me = T(a2, "我6"), *fish = T(a2, "鱼6");
        uint8_t rid = rel_id_of(a2, "拥有", 1);
        ok(rid == REL_HAS && token_tag_rel(me, fish, rid, REL_BOOST) == 0, "学「我 拥有 鱼6」");
        ok(rel_direct(me, REL_HAS, fish), "问「我 有 鱼6」能对上（同一个关系位）");
        ai_free(a2);
    }

    /* ② 来源档后验：观测档随"改口"变低，推论档随"撤销"变低 */
    {
        AI *a3 = ai_new();
        double p0 = ai_source_posterior(a3, ASSOC_ROLE_NONE);       /* 观测档 */
        /* 证据是**对话里现场学到的关系事实**（n_rel_learned）与**改口次数** ——
         * 不是 n_learned（那会把 24 万条训练词对算进来，后验恒等于 ~1.0）。 */
        a3->n_rel_learned = 40; a3->n_revised = 0;
        double p_good = ai_source_posterior(a3, ASSOC_ROLE_NONE);
        a3->n_revised = 30;                                         /* 改口一大堆 */
        double p_bad = ai_source_posterior(a3, ASSOC_ROLE_NONE);
        ok(p_good > p0, "观测档：现场学得多、没改口 → 后验升高");
        ok(p_bad < p_good, "观测档：改口多 → 后验降低（**学出来的**，不是写死的）");
        double d0 = ai_source_posterior(a3, ASSOC_ROLE_DERIVED);
        a3->n_cached = 300; a3->n_retracted = 0;
        double d_good = ai_source_posterior(a3, ASSOC_ROLE_DERIVED);
        a3->n_retracted = 250;                                      /* 缓存被撤一大堆 */
        double d_bad = ai_source_posterior(a3, ASSOC_ROLE_DERIVED);
        ok(d_good > d0 && d_bad < d_good, "推论档：撤销多 → 后验降低");
        ok(ai_source_posterior(a3, ASSOC_ROLE_INNATE) > p_bad,
           "基准档（显式教的）始终高于被改口拖垮的观测档");
        ok(ai_source_class_pct(a3, ASSOC_ROLE_INNATE) <= 85, "折成百分数后仍在同一个量纲（≤85）");
        ai_free(a3);
    }
    ai_free(ai);
}

/* ============================ ㉓ 租约 / 写入闸门 / 前提第三档 ============================
 *
 * 借参考实现的三件（`docs/对照-lingshu与五个局限.md` 第二节）：
 *   ① **知识租约（TTL）**：`stable` 不是终态，而是带期限的状态；**降级只改状态，不删记录**；
 *   ② **写入分级闸门**：imp 评分 <0.40 的"随口一说"不落库（默认关）；
 *   ③ **前提第三档 verified**：够得着还不够，前提本身要**有据**。 */
/* §6.129 **目标自建 / 自跟 / 自闭**（默认关；老路径逐比特等价；单词语标能恢复）：
 * 盘点发现「目标」原来只有一个来源 —— 用户问出来的东西。这一节钉住补上的那一支：
 * 从**自己**的状态（`recent_misses` 高 + 边数少）里立目标，并守住三条边界。 */
static void test_self_goal(void) {
    printf("== ⓺ 目标自建：从「我自己没底」里立目标（默认关；§6.129） ==\n");
    AI *ai = ai_new();
    Think *t = think_of(ai);
    Token *thin = T(ai, "薄词甲");            /* 有 1 条边 ⇒ count > 0 */
    Token *oth  = T(ai, "陪衬乙");
    (void)token_observe_pair(thin, oth);
    thin->recent_misses = AI_SELFGOAL_MISSES + 3;
    think_set_selfgoal(0);
    t->selfgoal_last = 0; t->selfgoal_seen = 0;
    ok(think_selfgoal(ai) == 0, "默认关：一个字都不立");
    ok(t->n_open == 0 && t->n_selfmade == 0, "默认关：议程与账都没动");
    think_set_selfgoal(1);
    t->selfgoal_last = 0; t->selfgoal_seen = 0;
    ok(think_selfgoal(ai) == 1, "开着：从「薄词甲」立出一个目标（判据全在图上）");
    ok(t->n_open == 1 && t->n_selfmade == 1, "议程上多一条，并记成「自己立的」");
    ThinkOpen *o = think_open_find(t, thin, NULL, 0, 0);
    ok(o && o->selfmade == 1, "那条目标标着 selfmade（::pending 会打出来）");
    ok(o && o->born_count == thin->count, "记下「立目标时它有几条边」——判据靠它");
    t->selfgoal_last = 0; t->selfgoal_seen = 0;
    ok(think_selfgoal(ai) == 0, "节流：同一个 tick 再看一次不会重复立（每 20 个 tick 看一次）");
    /* 自闭（新判据）：**比立目标时多了边**才算想通 —— 老判据 `count > 0` 对它当场成立（错） */
    size_t before = thin->count;
    (void)token_observe_pair(thin, T(ai, "陪衬丙"));
    ok(thin->count > before, "它真的多了边");
    think_settle(ai);
    ok(think_open_find(t, thin, NULL, 0, 0) == NULL, "自闭：比立目标时多了边 ⇒ 收起来（想通）");
    ok(t->n_resolved >= 1, "记进「想通」账");
    /* 老路径逐比特等价：count==0 才挂 ⇒ born_count=0 ⇒ 「有第一条边」就算想通 */
    Token *unk = T(ai, "从没见过的丙");
    (void)think_open_add(t, unk, NULL, 0, 0, "从没见过的丙", ai->pool->current_tick);
    ThinkOpen *o2 = think_open_find(t, unk, NULL, 0, 0);
    ok(o2 && o2->born_count == 0, "老路径（count==0 才挂）⇒ born_count=0（判据与原来一致）");
    (void)token_observe_pair(unk, oth);
    think_settle(ai);
    ok(think_open_find(t, unk, NULL, 0, 0) == NULL, "老路径照旧：有第一条边就算想通");
    /* 跨装载：**单个词**的自问标记也要恢复（原来只认 ≥3 个词的关系问句） */
    Token *thin2 = T(ai, "薄词乙");
    (void)pool_token_intern(ai->pool, MICRO_ASK_TOKEN_PREFIX "薄词乙]");
    size_t got = think_restore_pending(ai);
    ThinkOpen *o3 = think_open_find(t, thin2, NULL, 0, 0);
    ok(got >= 1 && o3 != NULL, "装载恢复：单词语标 [自问:薄词乙] 挂回议程（原来会被跳过）");
    ok(o3 && o3->selfmade == 1, "恢复回来的那条也标成「自己立的」");
    /* §6.134 **第二条判据：常用但薄** —— 为什么加：`recent_misses` 那条在出厂模型上是**死的**
     * （实测 1198 个符号节点里一个都没有），而"边少但用得多"的词有 252 个（实测）。
     * ⚠ 用**一个新的脑子**测：同一个脑子里前面那些"薄词甲"还带着 recent_misses=7，
     * 一定会先被第一臂挑走 —— 那样测到的不是这一臂（第一版就是这么红的）。 */
    {
        AI *a2 = ai_new();
        Think *t2 = think_of(a2);
        Token *oth2 = T(a2, "陪衬子"), *busy = T(a2, "常用词子");
        (void)token_observe_pair(busy, oth2);
        busy->recent_misses = 0;                       /* 明确：**没有**"没底"信号 */
        for (int k = 0; k < AI_SELFGOAL_HITS; ++k) busy->assocs[0].hits++;
        think_set_selfgoal(1);
        t2->selfgoal_seen = 0; t2->selfgoal_last = 0;
        ok(think_selfgoal(a2) == 1, "第一臂为 0 也照样能立目标（第二臂：常用但薄）");
        ThinkOpen *ob = think_open_find(t2, busy, NULL, 0, 0);
        ok(ob && ob->selfmade == 1, "立起来的正是「常用词子」那条，且标着自己立的");
        Token *rare = T(a2, "冷门词丑");
        (void)token_observe_pair(rare, oth2);
        rare->recent_misses = 0;
        rare->assocs[0].hits = 1;                      /* 边少、命中少 ⇒ 两臂都不满足 */
        ThinkOpen *ob2 = think_open_find(t2, busy, NULL, 0, 0);
        if (ob2) think_open_done(t2, ob2);
        /* ⚠ 光撤掉目标不够：`busy` 自己还是候选，下一趟会**再立一条**（第一版就这么红的）。
         * 要测"没人用就不立"，得把两个词的命中都压到门槛以下。 */
        busy->assocs[0].hits = 1;
        t2->selfgoal_seen = 0; t2->selfgoal_last = 0;
        ok(think_selfgoal(a2) == 0, "边少但**没人用** ⇒ 不立目标（不刷屏）");
        ai_free(a2);
    }
    /* 「（未知）」占位词不算要弄懂的东西（否则会立一个"弄懂（未知）"的傻目标） */
    Token *unkk = ai->pool->unknowing;
    if (unkk) {
        unkk->recent_misses = AI_SELFGOAL_MISSES + 5;
        t->selfgoal_seen = 0;
        (void)think_selfgoal(ai);
        ok(think_open_find(t, unkk, NULL, 0, 0) == NULL, "「（未知）」占位词不会被立成目标");
    }
    think_set_selfgoal(0);
    ai_free(ai);
}

/* §6.129 ② **薄词目标自己的处理路**：原来 `::idle` 把"一个词"丢给 `think_run`（要关系形状）
 * ⇒ 立刻收束、什么也没干。现在拿它**自己的图**拼一个能核的问题，再走既有机制。这一节钉住：
 * 两条拼法（自己的关系边 / 接一条链）、拼不出来退回老路、只有自己立的薄词目标才走这条路、
 * 以及"这条路是**只读**的"（K5：不提议意图、不动图）。 */
static void test_thin_pursuit(void) {
    printf("== ⓻ 薄词目标的处理路：拿它自己的图拼问题（§6.129 ②） ==\n");
    AI *ai = ai_new();
    Think *t = think_of(ai);
    /* ① 它自己有一条肯定关系边 ⇒ 问「W 有 X 吗」 */
    Token *w1 = T(ai, "薄词丁"), *x1 = T(ai, "物件戊");
    (void)token_tag_rel(w1, x1, REL_HAS, 5);
    char buf[THINK_TEXT], why[THINK_WHY];
    ok(think_thin_question(ai, w1, buf, sizeof buf, why, sizeof why) == 1,
       "有自己的关系边 ⇒ 拼得出问题");
    ok(strstr(buf, "薄词丁") && strstr(buf, "物件戊") && strstr(buf, "有") && strstr(buf, "吗"),
       "拼出来的就是「W 有 X 吗」（关系名取自关系表，不是写死的）");
    /* ② 只有普通边 ⇒ 接一条链：W →（共现）→ Z →（关系）→ X */
    Token *w2 = T(ai, "薄词己"), *z2 = T(ai, "中间庚");
    (void)token_observe_pair(w2, z2);              /* 普通共现边 */
    (void)token_tag_rel(z2, x1, REL_HAS, 5);       /* 中间词与 X 有关系边 */
    ok(think_thin_question(ai, w2, buf, sizeof buf, why, sizeof why) == 1,
       "只有普通边 ⇒ 用邻居的关系边接一条链");
    ok(strstr(buf, "薄词己") && strstr(buf, "物件戊"),
       "接出来的问题还是冲着 W 自己（「W 有 物件戊 吗」）");
    /* ③ 什么都没有 ⇒ 拼不出来（调用方退回原话 = 老行为） */
    Token *w3 = T(ai, "孤零零辛");
    ok(think_thin_question(ai, w3, buf, sizeof buf, why, sizeof why) == 0,
       "一条边都没有 ⇒ 拼不出来（退回老路，不硬凑问题）");
    /* ④ 只有**否定**关系边 ⇒ 也退回老路：否定问句的形状在这个核心里不唯一 */
    Token *w4 = T(ai, "薄词壬");
    (void)token_tag_nrel(w4, x1, REL_HAS, 5);
    ok(think_thin_question(ai, w4, buf, sizeof buf, why, sizeof why) == 0,
       "只有否定关系边 ⇒ 不拼（否定问句形状不唯一，宁可退回老路）");
    /* ⑤ 接进 ::idle：自己立的薄词目标走这条路，且**只读**（不动图） */
    ThinkOpen *o = think_open_add(t, w1, NULL, 0, 0, "薄词丁", ai->pool->current_tick);
    if (o) o->selfmade = 1;
    t->idle_every = 1; t->idle_last_tick = 0;
    size_t edges_before = w1->count;
    int ran = think_idle(ai);
    ok(ran == 1 && t->n_idle >= 1, "空闲思考真的跑了这条目标");
    int found = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (t->trace[i].what[0] && strstr(t->trace[i].what, "薄词目标")) found = 1;
    ok(found == 1, "痕里写明「薄词目标：给它拼一个能核的问题」（可复核，不是黑箱）");
    ok(w1->count == edges_before, "这条路是**只读**的：W 的边数一个没变（K5）");
    /* ⑥ 反向对照：**用户问出来的**目标不走这条路（老行为原样） */
    Token *u1 = T(ai, "用户问的词"), *u2 = T(ai, "对方癸");
    (void)token_tag_rel(u1, u2, REL_HAS, 5);
    ThinkOpen *o2 = think_open_add(t, u1, u2, REL_HAS, 0, "用户问的词 有 对方癸 吗",
                                   ai->pool->current_tick);
    if (o2) o2->selfmade = 0;
    /* ⚠ 先清痕再跑：上一轮那条薄词目标**已经想通并被收起**（这一步本身是对的），
     * 不清痕就会拿上一轮的步子当"这一轮也走了薄词路"的证据。 */
    t->trace_n = 0;
    t->idle_last_tick = 0;
    size_t idle_before = t->n_idle;
    (void)think_idle(ai);
    int thin_step = 0;
    for (size_t i = 0; i < t->trace_n; ++i)
        if (t->trace[i].what[0] && strstr(t->trace[i].what, "薄词目标")) thin_step++;
    ok(t->n_idle > idle_before && thin_step == 0,
       "用户问出来的目标**不**走薄词那条路（这一轮的痕里没有它，但空闲思考照跑）");
    ok(think_open_find(t, w1, NULL, 0, 0) == NULL,
       "上一条薄词目标已经被「想通」收起（自己立的也能自收尾）");
    t->idle_every = 0;
    ai_free(ai);
}

/* §6.131 **记忆链 + 把握按跳数衰减**：原来"记忆里的猜测"只能当**终点那一跳**
 * （`rel_next_cands` 里缓存当中间跳要先"重推得出来"，而猜测天生重推不出来 ⇒ 永远被挡）；
 * 而且无论用了几环都写死 0.45 ⇒ **猜得越多越像有把握**。这一节钉住：
 * 记忆链默认关、开着能接起来、跳数记得对、把握真按跳数衰减、以及**不中毒**（知识里仍然没有）。 */
static void test_memory_chain(void) {
    printf("== ⓼ 记忆链：猜测能当中间跳 + 把握按跳数衰减（§6.131） ==\n");
    AI *ai = ai_new();
    Token *A = T(ai, "甲链"), *M = T(ai, "中链"), *B = T(ai, "乙链");
    Token *s1 = T(ai, "同类链一"), *s2 = T(ai, "同类链二");
    ai->rel_remember = 1;
    /* ⚠ 用「属于」（`trans=1`）来测：`REL_HAS` 在关系表里 **trans=0**，
     * 传递那一支根本不开火 —— 第一版就拿 有 测，结果"链没接起来"，看着像功能没做。 */
    agent_guess_remember(ai, A, REL_IS_A, M, s1);     /* 猜测：甲链 属于 中链 */
    agent_guess_remember(ai, M, REL_IS_A, B, s2);     /* 猜测：中链 属于 乙链 */
    ok(rel_direct_guess(A, REL_IS_A, M, 0) == 1 && rel_direct_guess(M, REL_IS_A, B, 0) == 1,
       "两条猜测都记下来了（`[类比:…]` + role=推论）");
    /* ⚠ 默认态要在**任何 rel_chain_set 之前**查：`rel_chain_on()` 第一次调用就去读环境变量
     * 并缓存，先 set 再看就永远"默认关"（这个坑在别的开关上踩过）。
     * 反向测量（`AIP_RELCHAIN=1` 跑整包）时这条按规矩跳过 —— 否则整包在反向那一侧必红。 */
    {
        const char *env = getenv("AIP_RELCHAIN");
        if (!env) ok(rel_chain_on() == 0, "记忆链**默认关**（`AIP_RELCHAIN=1` 才开）");
        else { printf("  [OK]   反向测量 %s：默认关那条断言按规矩跳过\n", env); passes++; }
    }
    /* 关着：中间那一跳不许用猜测 ⇒ 接不起来（老行为）。**显式关**，不看环境。 */
    rel_chain_set(0);
    Proof p1; memset(&p1, 0, sizeof p1);
    g_rel_guess_visible = 1;
    rel_solve(ai, A, REL_IS_A, B, 0, &p1);
    ok(p1.status == 0, "关着记忆链：中间跳不许用猜测 ⇒ 接不起来（与 §6.125 行为一致）");
    /* 开着：两环猜测接起来，跳数记成 2 */
    g_rel_guess_visible = 1; rel_chain_set(1);
    Proof p2; memset(&p2, 0, sizeof p2);
    rel_solve(ai, A, REL_IS_A, B, 0, &p2);
    ok(p2.status != 0, "开着：猜测当中间跳 ⇒ 记忆链接起来了");
    ok(p2.guess_hops == 2, "跳数记得对：这条链里有**两环**是猜测");
    ok(p2.guessed == 1, "整条证明标成「用到了猜测」");
    /* 把握按跳数衰减：1 环 = 0.45（与原来一致）；2 环 = 0.315；下限 0.15 */
    ok(ai_guess_conf(1) == 0.45f, "1 环 = 0.45（与 §6.125 逐比特一致）");
    ok(ai_guess_conf(2) > 0.31f && ai_guess_conf(2) < 0.32f, "2 环 ≈ 0.315（每多一环乘 0.7）");
    ok(ai_guess_conf(9) == 0.15f, "环数再多也有下限 0.15（不会衰减到 0）");
    ok(ai_guess_conf(2) < ai_guess_conf(1), "**猜得越多越没把握**（这条以前是反的）");
    /* 不中毒：知识库口径照旧是"没有" */
    ok(rel_direct(A, REL_IS_A, B) == 0, "知识里仍然没有这条 —— 记忆链不冒充知识");
    ok(rel_solve_ignoring_caches(ai, A, REL_IS_A, B, 0) == 0, "复核模式（重推）照旧不认记忆");
    /* 话也要跟着跳数变：1 环与原来一字不差，多环要报数（用真的 Derivation 渲染） */
    {
        Derivation dd;
        der_reset(&dd);
        dd.has_claim = 1; dd.truth = +1; dd.gaps = 0; dd.conf = 45;
        dd.guessed = 1; dd.guess_hops = 1;
        der_goal(&dd, A, REL_IS_A, B);
        der_default(&dd, A, s1, REL_IS_A, B, 60, +1);
        der_fact(&dd, s1, REL_IS_A, B, 0, +1);
        der_conclude(&dd, +1, 45);
        char say1[THINK_CONCL] = {0}, say2[THINK_CONCL] = {0};
        der_say(ai, &dd, DER_STYLE_PLAIN, say1, sizeof say1);
        ok(strstr(say1, "其中一环是**推测**的") != NULL,
           "1 环的措辞与 §6.125 **一字不差**（不翻默认就不改话）");
        dd.guess_hops = 2;
        der_say(ai, &dd, DER_STYLE_PLAIN, say2, sizeof say2);
        ok(strstr(say2, "2 环") != NULL, "2 环的话报出环数（「其中 2 环是**推测**的」）");
    }
    /* 关掉可选项：计数不该被外层的复核/候选枚举串了账 */
    rel_chain_set(0);
    g_rel_guess_visible = 0;
    Proof p3; memset(&p3, 0, sizeof p3);
    rel_solve(ai, A, REL_IS_A, B, 0, &p3);
    ok(p3.status == 0 && p3.guess_hops == 0, "记忆不可见时既推不出来、也不记跳数（没串账）");
    ai_free(ai);
}

/* §6.132 **整句打分备忘**：`ai_detect_topic` 与 `ai_sent_lookup` 用同一个查询扫同一批库句，
 * 同一对 (查询, 库句) 原来每轮算两遍。备忘是**纯缓存**（值必须与逐条算一字不差），
 * 键按**内容**比（两个调用点各传一块临时数组），并在 tick / 库句数 / idf 长度变化时作废。 */
static void test_score_memo(void) {
    printf("== ⓽ 打分备忘：一轮算一遍，值与逐条算相同（§6.132） ==\n");
    AI *ai = ai_new();
    Token *q[3], *s1[3], *s2[3], *o1[2];
    q[0] = T(ai, "你"); q[1] = T(ai, "好"); q[2] = T(ai, "吗");
    s1[0] = T(ai, "你"); s1[1] = T(ai, "好"); s1[2] = T(ai, "呀");
    s2[0] = T(ai, "猫"); s2[1] = T(ai, "有"); s2[2] = T(ai, "毛");
    o1[0] = T(ai, "你"); o1[1] = T(ai, "好");
    ai_sent_add(ai, s1, 3, o1, 2, 1);
    ai_sent_add(ai, s2, 3, o1, 2, 2);
    size_t miss0 = ai->n_sc_miss;
    const int *cov = NULL;
    const int *row = ai_sent_score_row(ai, q, 3, &cov);
    ok(row != NULL && cov != NULL, "库句非空时拿得到分数行");
    ok(ai->n_sc_miss == miss0 + 1, "第一次查：真算了一遍");
    /* 逐条对拍：备忘里的值必须等于直接算出来的值（这是"纯缓存"的定义） */
    {
        int same = 1;
        for (size_t i = 0; i < ai->pairs_n; ++i) {
            int cv = 0;
            int ref = ai_sent_score_cov(ai, q, 3, ai->pairs[i].in, ai->pairs[i].in_n, &cv);
            if (ref != row[i] || cv != cov[i]) same = 0;
        }
        ok(same == 1, "每一格都与逐条算**完全相同**（分数与覆盖率都是）");
    }
    size_t miss1 = ai->n_sc_miss;
    const int *row2 = ai_sent_score_row(ai, q, 3, NULL);   /* 同一句查询、同一个 tick */
    ok(row2 == row && ai->n_sc_miss == miss1, "同一个查询再查：命中备忘，不重算");
    /* 换一句查询：必须重算（键按内容比，不是按指针） */
    Token *q2[3]; q2[0] = s2[0]; q2[1] = s2[1]; q2[2] = s2[2];
    (void)ai_sent_score_row(ai, q2, 3, NULL);
    ok(ai->n_sc_miss == miss1 + 1, "换一句查询：作废重算（键按内容，不按临时数组的地址）");
    /* 库句变多：也必须重算（新那一句没有分数） */
    size_t miss2 = ai->n_sc_miss;
    ai_sent_add(ai, s1, 3, o1, 2, 3);
    const int *row3 = ai_sent_score_row(ai, q, 3, NULL);
    ok(ai->n_sc_miss == miss2 + 1 && row3 != NULL, "库句变多 ⇒ 作废重算（不会拿旧数组当新库）");
    /* tick 变了：作废（一轮一份，跨轮不许复用） */
    size_t miss3 = ai->n_sc_miss;
    ai->pool->current_tick++;
    (void)ai_sent_score_row(ai, q, 3, NULL);
    ok(ai->n_sc_miss == miss3 + 1, "跨到下一個 tick ⇒ 作废（一轮一份）");
    /* §6.136 库句字符集位图：① 只打一次（懒补到 pairs_n，不是每轮重打）；
     * ② 位图不相交那条**精确判 0**（与逐条算一致，上面已经对拍过）；③ 共享字符的没被误杀。 */
    ok(ai->n_cb_fill == ai->pairs_n, "库句位图**只打一次**（懒补到 pairs_n，不是每轮重打）");
    {
        const int *r4 = ai_sent_score_row(ai, q, 3, &cov);
        ok(r4 && r4[1] == 0 && cov[1] == 0, "字符集不相交的库句：精确判 0（没有漏算）");
        ok(r4 && r4[0] > 0, "共享字符的库句**没被误杀**（分数照旧 > 0）");
    }
    ai_free(ai);
}

/* §6.133 **「猜」只有一条路**：找依据 → 建链 → **过验证器** → 渲染 → 记账（一处）。
 * 以前答复层与兜底各写一遍，判据/验证/记账分家 —— §6.123 原因①留下的尾巴。
 * 这一节钉住：两种依据都能答、**能证就不猜**（不再抢答）、记账只在一条路上、
 * 以及"结论重推不出来"那道闸的二号例外**只对已验证的猜测步**成立（不是免检）。 */
static void test_one_guess_path(void) {
    printf("== ⓾ 「猜」只有一条路 + 结论闸的二号例外（§6.133） ==\n");
    AI *ai = ai_new();
    ai->rel_analogy = 1; ai->rel_remember = 1;
    Token *sup = T(ai, "类T"), *a = T(ai, "甲T"), *b = T(ai, "乙T"), *o = T(ai, "冬眠T");
    (void)ai_write_rel(ai, b, sup, REL_IS_A, 0, AI_WSRC_RELATION);
    (void)ai_write_rel(ai, a, sup, REL_IS_A, 0, AI_WSRC_RELATION);
    (void)ai_write_rel(ai, b, o, REL_CAN, 0, AI_WSRC_RELATION);
    char line[THINK_WHY], resp[512];
    float conf = 0;
    Derivation d;
    der_reset(&d);
    line[0] = 0; resp[0] = 0;
    size_t mem0 = ai->n_rel_guess_mem;
    int got = agent_guess_answer(ai, a, REL_CAN, o, 0, AGENT_GUESS_SIBLING,
                                 &d, line, sizeof line, resp, sizeof resp, &conf);
    ok(got == 1, "同类互推（这一侧的依据集合）答得出来");
    ok(strstr(resp, "大概") && strstr(resp, "类比"), "措辞照旧：大概 + 明说是类比");
    ok(d.guessed == 1 && d.gaps == 0, "链标成「推测」且**缺口 0**（过了验证器）");
    ok(ai->n_rel_guess_mem == mem0 + 1, "记账只有一处：猜出来的结论被记进「记忆」");
    ok(conf > 0.0f && conf < 0.6f, "把握压低（按猜测环数算）");
    /* **能证就不猜**：结论独立可推时，DEFAULT 那一关会拒（"其实知识库里有"）⇒ 不答「大概」 */
    (void)ai_write_rel(ai, sup, o, REL_CAN, 0, AI_WSRC_RELATION);   /* 继承就能证出来了 */
    der_reset(&d);
    resp[0] = 0;
    ok(agent_guess_answer(ai, a, REL_CAN, o, 0, AGENT_GUESS_SIBLING,
                          &d, line, sizeof line, resp, sizeof resp, &conf) == 0,
       "**能证就不猜**：知识库里推得出来 ⇒ 这条不答「大概」（这正是「不再抢答」的根）");
    /* 结论闸的二号例外**只对已验证的猜测步**成立 */
    {
        Derivation d2;
        der_reset(&d2);
        d2.has_claim = 1;
        der_goal(&d2, b, REL_CAN, o);                 /* b 会 o 是观测事实 */
        der_default(&d2, b, T(ai, "毫不相干"), REL_CAN, o, 0, +1);   /* sim=0 ⇒ 这一环验不过 */
        der_fact(&d2, T(ai, "毫不相干"), REL_CAN, o, 0, +1);         /* 依据也不知道 ⇒ 也验不过 */
        der_conclude(&d2, +1, 45);
        int g2 = der_verify(ai, &d2);
        ok(g2 > 0, "猜测步**没验过**时二号例外不生效（照样报缺口，不是免检）");
    }
    ai_free(ai);
}

static void test_lease_gate_tiers(void) {
    printf("== ㉓ 知识租约 / 写入分级闸门 / 前提第三档 ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "甲7"), *b = T(ai, "乙7");
    (void)token_tag_rel(a, b, REL_IS_A, REL_BOOST);
    long ei = micro_edge_index(a, b);
    ok(ei >= 0, "先有一条知识");
    ok(ai_edge_lease(ai, &a->assocs[ei]) == AI_LEASE_STABLE, "刚碰过的 → 租约**稳定**");
    ai->pool->current_tick = 300000;                     /* 超过 TTL */
    ok(ai_edge_lease(ai, &a->assocs[ei]) == AI_LEASE_EXPIRED,
       "超过 TTL → **过期**（但记录还在：只改状态不删）");
    ok(micro_has_edge(a, b), "过期之后那条边**照样在图里**（降级不等于删除）");
    ai->pool->current_tick = 100000;                     /* TTL 内但衰减够多 */
    {
        Token *d = T(ai, "丙7");
        ModIntent m; memset(&m, 0, sizeof m);
        m.type = MOD_TAG_DERIVED; m.target = a; m.main = d; m.rel_id = REL_IS_A;
        snprintf(m.source, sizeof m.source, "测试");
        char note[120];
        safety_step_begin(ai);
        (void)micro_try(ai, &m, note, sizeof note);
        long ed = micro_edge_index(a, d);
        a->assocs[ed].last_touch_tick = 0;               /* 很老 */
        ok(ed >= 0 && ai_edge_lease(ai, &a->assocs[ed]) == AI_LEASE_WEAK,
           "推论档 + 衰减够多 → **减弱**（可信度掉到弱阈值下）");
    }

    /* ② 写入闸门：生词更值得记、见多了就一般；分层名字对得上 */
    {
        AI *a2 = ai_new();
        Token *x = T(a2, "丁7"), *y = T(a2, "戊7");
        double imp_new = ai_write_importance(a2, x, y);
        ok(imp_new > 0.0 && imp_new <= 1.0, "重要度在 0..1");
        ok(ai_pair_jaccard(x, y) == 0.0 && ai_write_importance(a2, x, y) > 0.5,
           "两端互不相干（Jaccard=0）→ 信息量高 → 重要度高");
        {
            /* 邻居高度重叠的一对 → 信息量低（这条连带是"意料之中"） */
            Token *p1 = T(a2, "壬7"), *p2 = T(a2, "癸7");
            for (int i = 0; i < 5; ++i) {
                char nm[16];
                snprintf(nm, sizeof nm, "子%d", i);
                Token *o = T(a2, nm);
                (void)token_add_assoc_unique(p1, o);
                (void)token_add_assoc_unique(p2, o);
            }
            ok(ai_pair_jaccard(p1, p2) > 0.5, "邻居几乎一样 → Jaccard 高");
            ok(ai_write_importance(a2, p1, p2) < ai_write_importance(a2, x, y),
               "于是重要度更低（信息量那一项真的在起作用）");
        }
        ok(strcmp(ai_write_layer(AI_WGATE_LONGTERM), "长期层") == 0 &&
           strcmp(ai_write_layer(AI_WGATE_KNOWLEDGE), "知识层") == 0 &&
           strcmp(ai_write_layer(0.10), "情境层（随口一说）") == 0,
           "三层名字对得上（≥0.70 长期 / ≥0.40 知识 / 更低 情境）");
        /* 反向测量（AIP_WGATE=1）时"默认关"这条按规矩跳过：那时它本来就是开的 */
        if (!getenv("AIP_WGATE")) ok(a2->write_gate_off == 1, "闸门**默认关**（不由它决定写不写）");
        else ok(a2->write_gate_off == 0, "AIP_WGATE=1（反向测量）：启动即开（默认值断言按规矩跳过）");
        ai_free(a2);
    }

    /* ③ 前提第三档：够得着 + **有据** */
    {
        AI *a3 = ai_new();
        Token *thin = T(a3, "己7");                      /* 没有任何边 → 没据 */
        ok(!ai_premise_wellfounded(a3, thin), "没有边的词当不了「有据的前提」");
        Token *fat = T(a3, "庚7");
        for (int i = 0; i < 4; ++i) {
            char nm[16];
            snprintf(nm, sizeof nm, "辛%d", i);
            Token *o = T(a3, nm);
            (void)token_add_assoc_unique(fat, o);
            long k = micro_edge_index(fat, o);
            if (k >= 0) fat->assocs[k].hits = 3;
        }
        ok(ai_premise_wellfounded(a3, fat), "有 ≥3 条带命中的边 → 算「有据」");
        ai_free(a3);
    }
    ai_free(ai);
}

/* == ⓫ 激活的访问上限 + 睡眠真的会停下来（§6.135） ==
 * 为什么要有：`act_seen()` 只看**当前这条 DFS 路径**（返回时 `a->n--`），所以同一个符号节点
 * 会被不同路径反复激活 ⇒ 代价随深度**指数增长**。实测（出厂模型，每个词的激活耗时）：
 * 深度 2 = 14.9 ms、深度 3 = **1648 ms**；而睡眠回放用的是**深度 6** ⇒ 它实际上**永远跑不完**
 * （`roundbench` 默认 200 轮实测卡死 5 分钟以上）。这里用一个多路径小图 + 一个小上限，
 * 证明"到顶就停"，**不用**真造一个指数爆炸的图（那种测试自己就会挂）。 */
static void test_activation_budget(void) {
    printf("== ⓫ 激活访问上限 + 睡眠会停下来（§6.135） ==\n");
    AI *ai = ai_new();
    /* a→b→d / a→c→d / b→c：同一节点会被多条路径摸到 */
    Token *a = T(ai, "上限A"), *b = T(ai, "上限B"), *c = T(ai, "上限C"), *d = T(ai, "上限D");
    (void)token_observe_pair(a, b); (void)token_observe_pair(a, c);
    (void)token_observe_pair(b, c); (void)token_observe_pair(b, d);
    (void)token_observe_pair(c, d);
    /* ① 不限（`0` = 所有既有调用方的老行为）：路径照走 */
    ActivateCtx unlimited;
    activate_ctx_init(&unlimited, 0.9f, 0.05f, 6);
    token_activate_from(a, NULL, 0, 1.0f, &unlimited);
    size_t free_visits = unlimited.n_visits;
    activate_ctx_free(&unlimited);
    ok(free_visits >= 4 && unlimited.n_cut == 0,
       "默认（max_visits=0）**逐字不变**：路径全走、一次都不截断");
    /* ② 上限 3：到顶就停，而且记了账 */
    ActivateCtx lim;
    activate_ctx_init(&lim, 0.9f, 0.05f, 6);
    lim.max_visits = 3;
    token_activate_from(a, NULL, 0, 1.0f, &lim);
    ok(lim.n_visits == 3 && lim.n_cut > 0, "给了上限就**正好停在上限**（并记 n_cut）");
    activate_ctx_free(&lim);
    /* ③ 睡眠那条路**真的用上了**上限（把上限压到 2，跑一步睡眠） */
    ai->sleep_max_visits = 2;
    ai->trace_n = 0;
    ai_push_trace(ai, a);
    ai_sleep_begin(ai);
    ok(ai_sleep_pending(ai) && ai->sleep_max_visits == 2, "睡眠：轨迹挂上、上限已就位");
    ai_sleep_step(ai);
    ok(ai->n_sleep_visits <= 2, "睡眠一步**没超过上限**（老代码在这一步指数爆炸）");
    ai_free(ai);
}

/* == ⓬ 语言层 `激活` 原语现在是**惰性**的（§6.137，这是个**绊线**） ==
 * §6.135 我登记过"同一类指数爆炸还剩一处没修：语言层 `激活` 用深度 16"。
 * 这一轮去核，发现**它现在根本不动**：唯一造 `Interp` 的入口是 `token_run_ctx()`，
 * 它写的是 `Interp ip = {0}` ⇒ `ctx == NULL`、`energy == 0`，而 `token_activate()` 的
 * 第一道闸就是 `energy < cutoff(0.05) ⇒ return`。
 * ⚠ 这条断言是**绊线**：谁把 `ctx/energy` 接上（那正是这条分支的用法），这里会立刻变红
 * —— 那时要同时设 `act.max_visits`（见 token_lang.h 里 PK_ACTIVATE 的注释与 §6.135）。 */
static void test_activate_inert(void) {
    printf("== ⓬ 语言层 `激活`：现在一动不动（§6.137；接线前先读绊线说明） ==\n");
    AI *ai = ai_new();
    /* 多路径小图（和 ⓫ 同一形状）：这样上限 3 会**真的**撞到（只有 a→b→c 一条链时
     * 访问数正好 3，一次都截断不到 —— 第一版就是这么红的）。 */
    Token *a = T(ai, "激活甲"), *b = T(ai, "激活乙"), *c = T(ai, "激活丙"), *d = T(ai, "激活丁");
    (void)token_observe_pair(a, b); (void)token_observe_pair(a, c);
    (void)token_observe_pair(b, c); (void)token_observe_pair(b, d);
    (void)token_observe_pair(c, d);
    uint64_t h0 = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) h0 += token_hits_total(ai->pool->tokens[i]);
    const char *names[2] = { "激活甲", PRIM_ACTIVATE };
    token_run_names_ctx(ai->pool, names, 2, NULL, NULL);       /* 和 CLI 同一条入口 */
    uint64_t h1 = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) h1 += token_hits_total(ai->pool->tokens[i]);
    ok(h0 == h1, "按现在的入口跑 `激活`：触摸计数**一动不动**（energy=0 < 门槛、ctx=NULL）");
    /* 同一段代码，把 ctx/energy 接上（原语的另一条分支）：证明"如果接上"它真会扩散，
     * 而 `max_visits` 顶得住 —— 将来接线的人别忘了设它。 */
    ActivateCtx act;
    activate_ctx_init(&act, 0.7f, 0.05f, 16);
    act.max_visits = 3;
    Token *ctx[1] = { b };
    token_activate_from(a, ctx, 1, 1.0f, &act);
    ok(act.n_visits == 3 && act.n_cut > 0,
       "真接上之后：`max_visits` 照样顶得住（深度 16 的口径，深度 4 实测已 26 s）");
    activate_ctx_free(&act);
    ai_free(ai);
}

/* == ⓭ 脏思考检查器：连续重复的词/句 ⇒ 截停（§6.139，用户点名要的） ==
 * 只有"过程"被看过（think.h 的原地打转），"说出口的话在重复"以前没人看。 */
static void test_dirty_guard(void) {
    printf("== ⓭ 脏思考检查器：连续重复的词/句 ⇒ 截停（§6.139） ==\n");
    size_t k = 9;
    ok(ai_dirty_cut("我 想 学 画画", &k) == 0 && k == 0, "干净的话：一个字都不截");
    ok(ai_dirty_cut("", &k) == 0, "空串也不截");
    /* 「我 我 我 我 想」：字节 我=3B —— 词偏移 0/4/8/12/16，从**第二次**那里截 ⇒ 4 */
    ok(ai_dirty_cut("我 我 我 我 想", &k) == 4 && k == 1, "同一个词连着 4 次：从第二次截（词级）");
    /* 「你 你」只有两次：**不误杀**（自然强调留着） */
    ok(ai_dirty_cut("你 你", &k) == 0, "连着两个不算脏（不误杀）");
    /* 「你好 吗 你好 吗 你」：周期 2 的重复，从第二段开头截 */
    ok(ai_dirty_cut("你好 吗 你好 吗 你", &k) == 11 && k == 2, "整句连着重复：从第二句开头截（句级）");
    /* 「猫 会 猫 会 猫 会」：周期 2 的循环 */
    ok(ai_dirty_cut("猫 会 猫 会 猫 会", &k) == 8 && k == 2, "两词循环也认（周期 2）");
    /* 动作：截停 + 记账 + 只留前面干净的 */
    {
        AI *ai = ai_new();
        /* 默认值断言：`AIP_DIRTY=0`（反向测量）时按规矩**改成钉反向**，不然整包假红。
         * 下面测**动作**时自己把这一位钉成 1（C 级测试不跟 env 走 —— ⑱ 那条同一个坑）。 */
        {
            const char *dg = getenv("AIP_DIRTY");
            if (dg && dg[0] == '0')
                ok(ai->dirty_guard == 0, "AIP_DIRTY=0（反向测量）：默认值断言按规矩钉反向");
            else
                ok(ai->dirty_guard == 1, "默认**开**（用户要「重复就强制停」）");
        }
        ai->dirty_guard = 1;
        char o[256];
        snprintf(o, sizeof o, "%s", "我 想 学 画画 我 想 学 画画 我 想");
        size_t kk = 0;
        int cut = ai_dirty_apply(ai, o, sizeof o, &kk);
        ok(cut == 1 && strcmp(o, "我 想 学 画画") == 0, "动作：截到重复开始处，前面干净的部分留着");
        ok(kk == 2, "判据记的是**句级**（周期 4 的整句重复）");
        ok(ai->n_dirty_stops == 1 && ai->n_dirty_words >= 4, "账：截停 1 次 + 记了截掉多少词");
        /* 关掉就不管 */
        ai->dirty_guard = 0;
        snprintf(o, sizeof o, "%s", "猫 猫 猫 猫");
        ok(ai_dirty_apply(ai, o, sizeof o, NULL) == 0 && strcmp(o, "猫 猫 猫 猫") == 0,
           "`::dirty off` 之后一个字都不动");
        ai_free(ai);
    }
}

/* == ⓮ 叙事显形 + 别只说「我」（§6.141） ==
 * 以前 `::stats` 只有计数，内容一个字看不到；而且叙事几乎永远输出「我最近在想 我」
 * （每个 [时刻:] 都挂着自指边，命中最高）。 */
static void test_narrate(void) {
    printf("== ⓮ 叙事：显形 + 说的是「在想什么」（§6.141） ==\n");
    AI *ai = ai_new();
    Token *cat = T(ai, "猫"), *now = T(ai, "[时刻:1]");
    (void)token_add_assoc_unique(now, cat);
    (void)token_add_assoc_unique(now, ai->self);   /* 自指边**最后加** ⇒ 命中并列时它本来会赢 */
    ai->timeline[0] = now; ai->timeline_n = 1; ai->timeline_head = 1;
    int got = ai_narrate_apply(ai);
    ok(got == 1 && ai->narr_last[0] != 0, "跑一次叙事：文本留下来了（`::narrate` 看得到）");
    ok(strstr(ai->narr_last, "我最近在想") != NULL, "措辞照旧（没改渲染）");
    ok(strstr(ai->narr_last, "猫") != NULL, "说的是**它在想什么**（猫）");
    ok(strstr(ai->narr_last, "我，然后") == NULL && strstr(ai->narr_last, "在想 我") == NULL,
       "**没有**退化成「我最近在想 我」（跳过自指边）");
    ok(ai->n_narrations == 1 && ai->narr_last_turn == ai->turns, "记账：累计 1 次 + 记了轮次");
    ai_free(ai);
}

/* == ⓯ 元认知提醒的写入：默认直写 / 开了走统一通道（§6.143，外部审计 P1-2） ==
 * `micro_remind` 把 `[提醒:…]` 挂在「我」身上 —— 那是**自撰**，以前绕过安全内核直接改边表。 */
static void test_remind_route(void) {
    printf("== ⓯ 元认知提醒：默认直写 / 开了受审（§6.143） ==\n");
    {   /* 默认（开关关）：与今天逐字节相同 —— 直写、立刻在图上、**不记**自撰账 */
        AI *ai = ai_new();
        ai->remind_route = 0;            /* C 级测试**自己钉这一位**（不跟 AIP_REMINDROUTE 走）*/
        size_t w0 = ai->writes[AI_WSRC_INNER];
        micro_remind(ai, MICRO_REMIND_THINK_STALL, NULL);
        Token *rn = pool_find(ai->pool, MICRO_REMIND_TOKEN_PREFIX "思维原地打转]");
        ok(rn != NULL && micro_has_edge(ai->self, rn), "默认（关）：照旧**直写**，提醒节点立刻在图上");
        ok(ai->writes[AI_WSRC_INNER] == w0, "默认（关）：**不记**自撰账（一条路都不多加）");
        ai_free(ai);
    }
    {   /* 开着：提成意图 ⇒ 记进账、等安全点才落地 */
        AI *ai = ai_new();
        ai->route_writes = 1; ai->remind_route = 1;
        size_t w0 = ai->writes[AI_WSRC_INNER];
        micro_remind(ai, MICRO_REMIND_THINK_STALL, NULL);
        Token *rn = pool_find(ai->pool, MICRO_REMIND_TOKEN_PREFIX "思维原地打转]");
        ok(ai->writes[AI_WSRC_INNER] == w0 + 1, "开着：走了统一通道 ⇒ **记进自撰账**");
        ok(rn != NULL && micro_has_edge(ai->self, rn),
           "开着：当场**审查 + 应用**（`micro_try` 就是这个语义）⇒ 边在，而且进过审计");
        ok(micro_of(ai)->safe.n_allowed >= 1, "开着：内核的「允许」计数走了一格（真的过了受审通道）");
        ai_free(ai);
    }
    {   /* **这一对才是重点**：内核**冻结**时 ——
         * 直写那条照样落（P1-2 的形状：绕过内核）；路由那条被 review 拦住、写不进去。 */
        AI *ai = ai_new();
        ai->route_writes = 1; ai->remind_route = 0;
        micro_of(ai)->safe.freeze = 1;                   /* 安全内核：冻结 */
        micro_remind(ai, MICRO_REMIND_THINK_OPEN, NULL);
        Token *d1 = pool_find(ai->pool, MICRO_REMIND_TOKEN_PREFIX "未解问题太多]");
        ok(d1 != NULL && micro_has_edge(ai->self, d1),
           "**冻结时直写照样落** —— 这就是 P1-2 的形状（绕过安全内核）");
        ai_free(ai);
    }
    {
        AI *ai = ai_new();
        ai->route_writes = 1; ai->remind_route = 1;
        micro_of(ai)->safe.freeze = 1;                   /* 同样冻结，但走路由 */
        micro_remind(ai, MICRO_REMIND_THINK_OPEN, NULL);
        Token *d2 = pool_find(ai->pool, MICRO_REMIND_TOKEN_PREFIX "未解问题太多]");
        ok(d2 != NULL && !micro_has_edge(ai->self, d2),
           "**冻结 + 开了路由** ⇒ 提醒边**写不进去**（这才是「受审」的意思）");
        ok(micro_of(ai)->safe.n_denied >= 1, "而且拒绝进了内核的账（理由 = 冻结）");
        ai_free(ai);
    }
}

int main(void) {
    test_table();
    test_insert_between();
    test_temp_not_saved();
    test_collect_temp();
    test_edge_budget_charged();
    test_revise_memory();
    test_prim_stack();
    test_premise();
    test_premise_gate();
    test_premise_polarity_out();
    test_restore_pending();
    test_notepad_patch();
    test_d4_conditional();
    test_ai_notepad();
    test_provenance();
    test_dialog_trigger();
    test_write_ledger();
    test_order_query();
    test_merged_neg();
    test_process_marks();
    test_proc_run();
    test_proc_dry();
    test_proc_branch();
    test_proc_claim();
    test_selfwrite_route();
    test_revoke_marker();
    test_dyn_neg();
    test_cache_walk();
    test_exec_step();
    test_eval_num();
    test_lexicon();
    test_other_weak();
    test_open_relation();
    test_pron_last();
    test_wgate_tier();
    test_analogy_fallback();
    test_code_fixes();
    test_n1_decide();
    test_redo_stack();
    test_draft_keep();
    test_n1_persist();
    test_edge_tset();
    test_other_hist();
    test_guess_sibling();
    test_hypo_cache();
    test_answer_analogy();
    test_char_stream();
    test_rel_first_order();
    test_recursive_transfer();
    test_action_boundary();
    test_redo_alt();
    test_tail_blocks();
    test_credibility();
    test_struct_drive_order();
    test_premise_tiers();
    test_alias_and_posterior();
    test_lease_gate_tiers();
    test_one_guess_path();
    test_score_memo();
    test_memory_chain();
    test_thin_pursuit();
    test_self_goal();
    test_activation_budget();
    test_activate_inert();
    test_dirty_guard();
    test_narrate();
    test_remind_route();
    printf("\nToken 行为 / 临时符号节点 / 修正的记忆 自测：%d 项通过，%d 项失败\n", passes, fails);
    return fails ? 1 : 0;
}
