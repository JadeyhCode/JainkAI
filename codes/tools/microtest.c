/* tools/microtest.c — 微内核自己的测试（C 级，不经过 CLI）
 *
 * 看护六件事，每件都配了反例：
 *   ① 意图队列：去重、容量封顶、溢出记账
 *   ② 安全内核：每一种拒绝理由都真的会拒绝（自环/保留名/预算/关系边/成环/开关/冻结），
 *                以及「被拒的意图不吃预算」
 *   ③ 应用：加边幂等、打关系、设类别/父类、加固、删边、造 Token 必须接回网络
 *   ④ 检测器：共现固化（图内有证据才接线，邻居不重叠的不许接）、缺口命名
 *   ⑤ 补丁边界：热插拔、越界输出按容量截断、连续出错自动停用、冲突/沉默提醒、
 *                补丁提议照样过安全内核、坏补丁不影响好补丁
 *   ⑥ 元认知：速率长期顶满预算 → 冻结；提案多被否 → 关小（并且会恢复）
 *
 * 用法：gcc -std=c11 -O2 -o build_microtest tools/microtest.c -lm && ./build_microtest
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

/* intern（去重）而不是 pool_token（创建）：测试里要的是「同一个节点」 */
static Token *T(AI *ai, const char *name) { return pool_token_intern(ai->pool, name); }
static void link2(Token *a, Token *b) { (void)token_add_assoc_unique(a, b); }

/* 共享邻居池：让两个 Token 的邻居高度重叠，制造「图里已有证据」的局面 */
static const char *const SHARED[] = {"s1","s2","s3","s4","s5","s6","s7","s8","s9","s10"};
static void fanout(AI *ai, const char *name, int n) {
    Token *t = T(ai, name);
    for (int i = 0; i < n && i < 10; ++i) link2(t, T(ai, SHARED[i]));
}

static ModIntent mkintent(ModType type, Token *target, Token *main) {
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.target = target;
    m.main = main;
    return m;
}
static int has_token(AI *ai, const char *name) { return pool_find(ai->pool, name) != NULL; }

/* ============================ ① 意图队列 ============================ */
static void test_queue(void) {
    printf("== ① 修改意图队列 ==\n");
    AI *ai = ai_new();
    Micro *mk = micro_of(ai);
    Token *a = T(ai, "甲"), *b = T(ai, "乙");
    ModIntent m = mkintent(MOD_ADD_ASSOC, a, b);
    ok(mod_propose(&mk->queue, &m) == 1, "收下第一条意图");
    ok(mod_propose(&mk->queue, &m) == 0 && mk->queue.n_dedup == 1,
       "同一条意图被去重（不会建两条边）");
    for (int i = 0; i < MICRO_QUEUE_MAX + 8; ++i) {
        char na[24], nb[24];
        snprintf(na, sizeof na, "q%d", i);
        snprintf(nb, sizeof nb, "r%d", i);
        ModIntent x = mkintent(MOD_ADD_ASSOC, T(ai, na), T(ai, nb));
        (void)mod_propose(&mk->queue, &x);
    }
    ok(mk->queue.n == MICRO_QUEUE_MAX, "队列容量封顶");
    ok(mk->queue.n_dropped == 9, "溢出的 9 条被记账（不是静默丢掉）");
    mod_queue_clear(&mk->queue);
    ok(mk->queue.n == 0, "清空后队列为空（意图不跨轮）");
    ai_free(ai);
}

/* ============================ ② 安全内核的每一条拒绝理由 ============================ */
static void test_safety(void) {
    printf("== ② 安全内核：拒绝理由逐条 ==\n");
    AI *ai = ai_new();
    Micro *mk = micro_of(ai);
    SafetyKernel *sk = &mk->safe;
    Token *a = T(ai, "甲"), *b = T(ai, "乙");
    Token *prim = T(ai, "[原语:对象]");

    ModIntent m = mkintent(MOD_ADD_ASSOC, a, a);
    ok(safety_review(sk, ai->pool, &m) == SAFE_DENY_SELF, "自环被拒");

    m = mkintent(MOD_ADD_ASSOC, prim, b);
    ok(safety_review(sk, ai->pool, &m) == SAFE_DENY_RESERVED, "拿保留 Token（[原语:…]）当目标被拒");

    m = mkintent(MOD_ADD_ASSOC, a, b);
    ok(safety_review(sk, ai->pool, &m) == SAFE_OK, "普通加边放行");

    m = mkintent(MOD_ADD_ASSOC, a, NULL);
    ok(safety_review(sk, ai->pool, &m) == SAFE_DENY_BADREF, "引用空 main 被拒");

    /* 造新 Token：预算 2 条，第 3 条必须被拒 */
    for (int i = 0; i < 2; ++i) {
        ModIntent t = mkintent(MOD_ADD_TOKEN, a, NULL);
        snprintf(t.name, sizeof t.name, "新%d", i);
        ok(safety_review(sk, ai->pool, &t) == SAFE_OK, "造新 Token 在预算内放行");
        safety_charge(sk, ai->pool, &t);
    }
    ModIntent t3 = mkintent(MOD_ADD_TOKEN, a, NULL);
    snprintf(t3.name, sizeof t3.name, "新3");
    ok(safety_review(sk, ai->pool, &t3) == SAFE_DENY_BUDGET_TOKEN, "第 3 个新 Token 超预算被拒");
    sk->step_new_tokens = 0;
    ModIntent t4 = mkintent(MOD_ADD_TOKEN, a, NULL);
    snprintf(t4.name, sizeof t4.name, "[原语:偷偷改]");
    ok(safety_review(sk, ai->pool, &t4) == SAFE_DENY_RESERVED, "名字用保留前缀被拒");

    /* 知识不是共现：关系边不许删 */
    Token *r1 = T(ai, "猫"), *r2 = T(ai, "动物");
    (void)token_tag_rel(r1, r2, REL_IS_A, REL_BOOST);
    ModIntent rm = mkintent(MOD_REMOVE_ASSOC, r1, r2);
    ok(safety_review(sk, ai->pool, &rm) == SAFE_DENY_RELATION, "删关系边被拒");
    link2(a, b);
    ModIntent rm2 = mkintent(MOD_REMOVE_ASSOC, a, b);
    ok(safety_review(sk, ai->pool, &rm2) == SAFE_OK, "删普通共现边放行");

    /* 类别继承不许成环 */
    Token *p = T(ai, "父"), *c = T(ai, "子");
    token_set_parent(c, p);
    ModIntent cyc = mkintent(MOD_SET_PARENT, p, c);
    ok(safety_review(sk, ai->pool, &cyc) == SAFE_DENY_CYCLE, "父节点接成环被拒");

    /* 两个总闸 */
    sk->mod_off = 1;
    ok(safety_review(sk, ai->pool, &m) == SAFE_DENY_SWITCH, "总开关关着时一律拒绝");
    sk->mod_off = 0;
    sk->freeze = 1;
    ok(safety_review(sk, ai->pool, &m) == SAFE_DENY_FROZEN, "冻结时一律拒绝");
    sk->freeze = 0;

    /* 被拒的意图不占预算（否则垃圾提案会把预算吃光） */
    sk->step_new_edges = 0;
    sk->max_new_edges_step = 0;
    ModIntent over = mkintent(MOD_ADD_ASSOC, a, T(ai, "另一个"));
    ok(safety_review(sk, ai->pool, &over) == SAFE_DENY_BUDGET_EDGE, "建边预算用满被拒");
    ok(safety_review(sk, ai->pool, &over) == SAFE_DENY_BUDGET_EDGE, "连拒两次理由相同（预算没被吃掉）");
    ai_free(ai);
}

/* ============================ ③ 应用：幂等 + 各类修改 ============================ */
static void test_apply(void) {
    printf("== ③ 安全点应用 ==\n");
    AI *ai = ai_new();
    Token *a = T(ai, "甲"), *b = T(ai, "乙");
    char note[96];

    ModIntent m = mkintent(MOD_ADD_ASSOC, a, b);
    ok(micro_apply_one(ai, &m, note, sizeof note) == 1, "加边：产生新结构");
    ok(micro_has_edge(a, b), "边真的在图里");
    ok(micro_apply_one(ai, &m, note, sizeof note) == 0, "同一条再加：幂等（不重复建边）");

    m = mkintent(MOD_TAG_REL, a, b);
    m.rel_id = REL_LIKE;
    ok(micro_apply_one(ai, &m, note, sizeof note) == 1, "打关系");
    long idx = micro_edge_index(a, b);
    ok(idx >= 0 && (a->assocs[idx].rel & REL_BIT(REL_LIKE)) != 0, "关系位真的置上了");

    m = mkintent(MOD_SET_CATEGORY, a, NULL);
    ok(micro_apply_one(ai, &m, note, sizeof note) == 1 && a->is_category, "设类别");
    m = mkintent(MOD_SET_PARENT, a, b);
    ok(micro_apply_one(ai, &m, note, sizeof note) == 1 && a->parent == b, "设父类");

    m = mkintent(MOD_BUMP_HITS, a, b);
    m.bump = 7;
    idx = micro_edge_index(a, b);
    uint32_t before = a->assocs[idx].hits;
    ok(micro_apply_one(ai, &m, note, sizeof note) == 1 && a->assocs[idx].hits == before + 7, "加固（hits+7）");

    m = mkintent(MOD_REMOVE_ASSOC, a, b);
    ok(micro_apply_one(ai, &m, note, sizeof note) == 1 && !micro_has_edge(a, b), "删边");

    ModIntent nt = mkintent(MOD_ADD_TOKEN, b, NULL);
    snprintf(nt.name, sizeof nt.name, "新概念");
    ok(micro_apply_one(ai, &nt, note, sizeof note) == 1, "造新 Token");
    Token *created = pool_find(ai->pool, "新概念");
    ok(created && micro_has_edge(created, b), "新 Token 有边接回触发它的上下文（不是孤岛）");
    ai_free(ai);
}

/* ============================ ④ 检测器 ============================ */
static void test_detectors(void) {
    printf("== ④ 检测器（共现固化 / 缺口命名） ==\n");
    AI *ai = ai_new();
    Micro *mk = micro_of(ai);
    fanout(ai, "甲", 10);
    fanout(ai, "乙", 10);
    ok(!micro_has_edge(T(ai, "甲"), T(ai, "乙")), "前置条件：甲→乙 还没有边");
    size_t edges_before = T(ai, "甲")->count;
    char *w[2] = {(char *)"甲", (char *)"乙"};
    micro_step(ai, w, 2, "测试");
    ok(mk->meta.n_new_edges == 1, "共现固化：把图里已有证据的那条边写了下来");
    ok(micro_has_edge(T(ai, "甲"), T(ai, "乙")), "甲→乙 真的在图里了");
    ok(T(ai, "甲")->count == edges_before + 1, "只加了一条边，没有乱动别的");
    micro_step(ai, w, 2, "测试");
    ok(mk->meta.n_new_edges == 1, "同一对再来一轮：幂等，不会重复固化");

    /* 邻居毫不重叠的一对：不许接线（判据是证据，不是「一起出现过」） */
    Token *c = T(ai, "丙"), *d = T(ai, "丁");
    for (int i = 1; i <= 6; ++i) { char nm[16]; snprintf(nm, sizeof nm, "独%d", i); link2(c, T(ai, nm)); }
    for (int i = 1; i <= 6; ++i) { char nm[16]; snprintf(nm, sizeof nm, "另%d", i); link2(d, T(ai, nm)); }
    size_t before_new = mk->meta.n_new_edges;
    char *w2[2] = {(char *)"丙", (char *)"丁"};
    micro_step(ai, w2, 2, "测试");
    ok(mk->meta.n_new_edges == before_new, "邻居不重叠的一对**不接线**");

    /* 缺口命名：第 100 轮 + 空缺感高 + 末尾词边够多 */
    Token *seed = T(ai, "戊");
    for (int i = 1; i <= 8; ++i) { char nm[16]; snprintf(nm, sizeof nm, "邻戊%d", i); link2(seed, T(ai, nm)); }
    for (size_t i = 0; i < seed->count; ++i) seed->assocs[i].hits = 0;
    seed->recent_misses = 40;                    /* gap = 40/(40+0+1) ≈ 0.976 */
    ok(token_gap(seed) > 0.65f, "前置条件：空缺感够高（token_gap>0.65）");
    mk->meta.n_steps = MICRO_D2_EVERY - 1;       /* 下一步正好是第 100 轮 */
    char *w3[3] = {(char *)"甲", (char *)"乙", (char *)"戊"};
    micro_step(ai, w3, 3, "测试");
    Token *nc = pool_find(ai->pool, "[新概念:甲+乙]");
    ok(nc != NULL, "缺口命名：造出了新概念 Token");
    ok(nc && micro_has_edge(nc, seed), "新概念接回了触发它的上下文");
    ok(mk->meta.n_new_tokens == 1, "新 Token 计数在走");
    ai_free(ai);
}

/* ============================ ⑤ 补丁边界 ============================ */
typedef struct { AI *ai; int destroyed; int propose_calls; } TP;

static size_t tp_sense_good(Patch *p, TokenPool *pool, PatchOut *out, size_t cap) {
    (void)p;
    if (cap == 0) return 0;
    out[0].tok = pool_token_intern(pool, "补丁_信号");
    out[0].neg = 0; out[0].w = 1.0f;
    return 1;
}
static size_t tp_sense_bad(Patch *p, TokenPool *pool, PatchOut *out, size_t cap) {
    (void)p; (void)pool; (void)out; (void)cap;
    return (size_t)-1;                 /* 坏补丁：报错 */
}
static size_t tp_sense_silent(Patch *p, TokenPool *pool, PatchOut *out, size_t cap) {
    (void)p; (void)pool; (void)out; (void)cap;
    return 0;                          /* 沉默补丁：一直没输出 */
}
static size_t tp_sense_conflict(Patch *p, TokenPool *pool, PatchOut *out, size_t cap) {
    (void)p;
    if (cap == 0) return 0;
    out[0].tok = pool_token_intern(pool, "补丁_信号");
    out[0].neg = 1;                    /* 和好补丁对同一个 Token 唱反调 */
    out[0].w = 1.0f;
    return 1;
}
static int tp_propose(Patch *p, void *mq) {
    TP *tp = (TP *)p->priv;
    tp->propose_calls++;
    return mod_propose_simple((ModQueue *)mq, MOD_ADD_ASSOC,
                              T(tp->ai, "提议甲"), T(tp->ai, "提议乙"), "测试补丁");
}
static void tp_destroy(Patch *p) { ((TP *)p->priv)->destroyed = 1; }

static void test_patches(void) {
    printf("== ⑤ 补丁边界 ==\n");
    AI *ai = ai_new();
    Micro *mk = micro_of(ai);
    PatchRegistry *r = &mk->patches;
    static TP tp;
    tp.ai = ai; tp.destroyed = 0; tp.propose_calls = 0;

    Patch good;  memset(&good, 0, sizeof good);
    good.name = "好补丁"; good.kind = PATCH_SENSE; good.priv = &tp; good.sense = tp_sense_good;
    ok(patch_register(r, &good, ai->pool, NULL) == 0, "挂载感知补丁");
    ok(patch_register(r, &good, ai->pool, NULL) != 0, "同名补丁不许重挂");

    PatchOut out[8];
    size_t n = patch_sense_all(ai, out, 8);
    ok(n == 1 && out[0].tok && has_token(ai, "补丁_信号"), "感知：补丁输出被收集");
    ok(micro_patch_sense(ai) == 1, "注入：补丁 Token 进了这一轮的上下文");
    {
        char tname[64];
        snprintf(tname, sizeof tname, "[时刻:%llu]",
                 (unsigned long long)ai->pool->current_tick);
        Token *now = pool_find(ai->pool, tname);
        ok(now && micro_has_edge(now, out[0].tok), "注入的 Token 挂到了当前时刻（参与关联学习）");
    }
    /* 容量为 1 时只可能收 1 条 —— 补丁不能越界写 */
    ok(patch_sense_all(ai, out, 1) == 1, "容量为 1 时最多收 1 条（越界输出被截断）");

    /* 坏补丁：连续 3 次报错 → 自动停用 + 提醒，且不影响好补丁 */
    Patch bad; memset(&bad, 0, sizeof bad);
    bad.name = "坏补丁"; bad.kind = PATCH_SENSE; bad.sense = tp_sense_bad;
    (void)patch_register(r, &bad, ai->pool, NULL);
    for (int i = 0; i < 3; ++i) (void)patch_sense_all(ai, out, 8);
    ok(bad.enabled == 0 && bad.n_err == 3, "坏补丁连续 3 次出错 → 自动停用");
    ok(has_token(ai, "[提醒:补丁异常已停用:坏补丁]"), "停用时留下提醒（元认知管外设）");
    ok(good.enabled == 1, "坏补丁停用**不影响**好补丁");

    /* 冲突：两个补丁对同一个 Token 一个说有、一个说没有 */
    Patch conf; memset(&conf, 0, sizeof conf);
    conf.name = "冲突补丁"; conf.kind = PATCH_SENSE; conf.sense = tp_sense_conflict;
    (void)patch_register(r, &conf, ai->pool, NULL);
    (void)patch_sense_all(ai, out, 8);
    ok(has_token(ai, "[提醒:补丁信号冲突:冲突补丁]"), "信号冲突被元认知提出来了");

    /* 提议：补丁不能改结构，只能提议 —— 提议照样过安全内核 */
    Patch prop; memset(&prop, 0, sizeof prop);
    prop.name = "提议补丁"; prop.kind = PATCH_SENSE; prop.priv = &tp; prop.propose = tp_propose;
    (void)patch_register(r, &prop, ai->pool, NULL);
    size_t before = mk->meta.n_new_edges;
    micro_step(ai, NULL, 0, "");
    ok(tp.propose_calls == 1, "补丁的 propose 被调用");
    ok(mk->meta.n_new_edges == before + 1 &&
       micro_has_edge(T(ai, "提议甲"), T(ai, "提议乙")),
       "补丁提议的加边走完了「审查 → 安全点应用」");

    /* 停用/启用 */
    ok(patch_set_enabled(ai, "好补丁", 0) == 0, "停用补丁");
    size_t n_before = good.n_sense;
    (void)patch_sense_all(ai, out, 8);
    ok(good.n_sense == n_before, "停用的补丁不参与感知");
    ok(patch_set_enabled(ai, "好补丁", 1) == 0 && good.enabled == 1, "重新启用");

    /* 卸载：destroy 要被调用，表里要真的摘掉（不能留空洞） */
    Patch tmp; memset(&tmp, 0, sizeof tmp);
    tmp.name = "临时补丁"; tmp.kind = PATCH_SENSE; tmp.priv = &tp; tmp.destroy = tp_destroy;
    (void)patch_register(r, &tmp, ai->pool, NULL);
    size_t n_patch = r->n;
    ok(patch_unmount_by_name(ai, "临时补丁") == 0, "卸载补丁");
    ok(tp.destroyed == 1, "卸载时 destroy 被调用（补丁自己腾资源）");
    ok(r->n == n_patch - 1 && patch_find(r, "临时补丁") == NULL, "表里真的摘掉了");
    ok(patch_find(r, "好补丁") != NULL, "别的补丁没被误伤");
    ai_free(ai);

    /* 沉默：单独一个内核（否则会先被别的补丁占掉提醒冷却） */
    AI *ai2 = ai_new();
    Micro *mk2 = micro_of(ai2);
    Patch silent; memset(&silent, 0, sizeof silent);
    silent.name = "哑补丁"; silent.kind = PATCH_SENSE; silent.sense = tp_sense_silent;
    (void)patch_register(&mk2->patches, &silent, ai2->pool, NULL);
    ai2->pool->current_tick += MICRO_PATCH_SILENT + 5;
    micro_step(ai2, NULL, 0, "");
    ok(has_token(ai2, "[提醒:补丁沉默:哑补丁]"), "挂着却一直不输出的补丁被点了名");
    ai_free(ai2);
}

/* ============================ ⑥ 元认知：冻结与关小 ============================ */
static void test_metacog(void) {
    printf("== ⑥ 元认知（停止条件 / 自我关小） ==\n");
    AI *ai = ai_new();
    Micro *mk = micro_of(ai);
    /* 每轮固化一条新边，而预算故意设成 1：连续 MICRO_WINDOW 轮顶满 → 判定过热 → 冻结 */
    mk->safe.max_new_edges_step = 1;
    for (int step = 0; step < MICRO_WINDOW; ++step) {
        char na[16], nb[16];
        snprintf(na, sizeof na, "热甲%d", step);
        snprintf(nb, sizeof nb, "热乙%d", step);
        fanout(ai, na, 10);
        fanout(ai, nb, 10);
        char *w[2] = {na, nb};
        micro_step(ai, w, 2, "测试");
    }
    ok(mk->safe.freeze == 1, "速率长期顶满预算 → 冻结（停止条件真的会触发）");
    ok(has_token(ai, "[提醒:自修改过热]"), "冻结时留下「自修改过热」提醒");
    ok(mk->safe.n_freeze == 1, "冻结计数在走");
    {
        ModIntent probe = mkintent(MOD_ADD_ASSOC, T(ai, "热甲0"), T(ai, "热乙1"));
        ok(safety_review(&mk->safe, ai->pool, &probe) == SAFE_DENY_FROZEN, "冻结后一律拒绝");
    }
    safety_reset(ai);
    ok(mk->safe.freeze == 0, "::safe reset 能解冻（冻结不是死锁）");

    /* 提案多被否 → 关小提案，并且有期限（不是一次踩死） */
    size_t throttles_before = mk->meta.n_throttles;
    mk->safe.max_new_edges_step = 0;      /* 一切新建边都会被拒 */
    for (int step = 0; step < MICRO_WINDOW; ++step) {
        char na[16], nb[16];
        snprintf(na, sizeof na, "否甲%d", step);
        snprintf(nb, sizeof nb, "否乙%d", step);
        fanout(ai, na, 10);
        fanout(ai, nb, 10);
        char *w[2] = {na, nb};
        micro_step(ai, w, 2, "测试");
        mk->meta.denied_window += 8;      /* 造出「拒绝远多于应用」的局面 */
    }
    ok(mk->meta.n_throttles > throttles_before, "提案长期被否 → 元认知关小提案");
    ok(has_token(ai, "[提醒:提案多被否]"), "关小时留下提醒");
    ok(mk->coact_off == 1, "关小是真的少提（共现固化被关掉）");
    mk->meta.denied_window = 0;
    mk->propose_off = 1;                  /* 让关小期到点 */
    micro_step(ai, NULL, 0, "");
    ok(mk->coact_off == 0, "关小有期限：过期自动恢复");
    ai_free(ai);
}

int main(void) {
    test_queue();
    test_safety();
    test_apply();
    test_detectors();
    test_patches();
    test_metacog();
    printf("\n微内核自测：%d 项通过，%d 项失败\n", passes, fails);
    return fails ? 1 : 0;
}
