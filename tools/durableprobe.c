/* tools/durableprobe.c — **"教了就忘"回归**（§6.588，C 级）
 *
 * 用户目标第三阶段：「完善学习的机制，使得在交互中学习的能力可用」。
 * 一个最朴素的可用法则：**教过的事实，在长会话里不能自己消失**。
 *
 * 以前会消失：GC（`ai_gc`）按 age / recency / hits 的"淡忘"概率晋级，然后删边删词 ——
 * 用户教的 `太阳 会 发光`、`竹子 属于 植物` 只有一次命中（hits 很小），
 * 几十轮维护之后就和"不常用的共现"一样被清掉（实测 10 条掉到 7 条）。
 *
 * 这一套做的事：
 *   ① 教 10 条（含新词、肯定、属性）；
 *   ② 模拟长会话的维护：每轮推进 20 个 tick、做一次巩固/衰减、再跑一次 `ai_gc`；
 *   ③ 断言：10 条一条不少、每条都**问得出来**（"对 ，"）、GC 确实删过东西（不是空转）；
 *   ④ 存盘 → 读回 → 再验一次（持久化）。
 *
 * 用法：
 *   gcc -std=c11 -O2 -Wall -Wextra -o build_durableprobe tools/durableprobe.c -lm && ./build_durableprobe model.bin
 */
#include "../agent.h"
#include "../model_io.h"
#include "../aicheck.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static Agent *g_ag;
static char   g_out[1024];

static const char *say(AI *ai, const char *s) {
    g_ag->brain = ai;
    memset(g_out, 0, sizeof g_out);
    agent_step(g_ag, s, g_out, sizeof g_out);
    return g_out;
}

static int known(AI *ai, const char *a, const char *rel, const char *b) {
    Token *A = pool_find(ai->pool, a), *B = pool_find(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 0);
    if (!A || !B || !rid) return 0;
    return rel_direct(A, rid, B);
}

/* 维护一轮（和 agent_step 里的维护段同一套调用，只是把 20 个 tick 压成一次模拟）*/
static void maint_round(AI *ai) {
    ai->turns += 20;
    if (ai->consolidate_every) {
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (!t || t->is_garbage || !token_assocs_resident(t)) continue;
            token_consolidate(t);
            token_decay_hits(t, ai->decay_factor);
        }
    }
    ai_gc(ai);
}

int main(int argc, char **argv) {
    const char *model = argc > 1 ? argv[1] : "model.bin";
    int rounds = argc > 2 ? atoi(argv[2]) : 60;
    if (rounds < 1) rounds = 1;

    AI *ai = ai_load_file(model);
    if (!ai) { printf("读不到 %s\n", model); return 1; }
    g_ag = agent_new(stdin_perceive, stdout_act, NULL);
    if (!g_ag) { ai_free(ai); return 1; }
    ai_free(g_ag->brain);
    g_ag->brain = ai;

    const char *f[][3] = {
        {"麒麟", "属于", "神兽"}, {"小龙", "属于", "神兽"}, {"红宝石", "属于", "宝石"},
        {"螃蟹", "有", "钳子"},   {"太阳", "会", "发光"},   {"企鹅", "怕", "热"},
        {"竹子", "属于", "植物"}, {"铜", "属于", "金属"},   {"蝙蝠", "会", "飞"},
        {"蜜蜂", "有", "刺"},
    };
    int n = (int)(sizeof f / sizeof f[0]);
    int taught = 0;
    for (int i = 0; i < n; ++i) {
        char s[128];
        snprintf(s, sizeof s, "%s %s %s", f[i][0], f[i][1], f[i][2]);
        say(ai, s);
        if (known(ai, f[i][0], f[i][1], f[i][2])) taught++;
        else printf("       没学会：%s\n", s);
    }
    ok(taught == n, "10 条事实**当场全部学会**（含 2 字/3 字新词主语）");

    size_t edges0 = 0;
    for (size_t i = 0; i < ai->pool->count; ++i)
        if (ai->pool->tokens[i]) edges0 += ai->pool->tokens[i]->count;
    for (int k = 0; k < rounds; ++k) maint_round(ai);
    size_t del0 = ai->n_gc_deleted_edges + ai->n_gc_deleted_tokens;
    ok(del0 > 0, "维护确实在删东西（GC/衰减不是空转）");

    int after = 0;
    for (int i = 0; i < n; ++i) after += known(ai, f[i][0], f[i][1], f[i][2]);
    ok(after == n, "长会话维护之后：教过的事实**一条不少**（知识不被当共现删掉）");

    int ans = 0;
    for (int i = 0; i < n; ++i) {
        char q[128];
        snprintf(q, sizeof q, "%s %s %s 吗", f[i][0], f[i][1], f[i][2]);
        const char *r = say(ai, q);
        if (strstr(r, "对 ，")) ans++;
        else printf("       答不出：%s → %s\n", q, r);
    }
    ok(ans == n, "长会话维护之后：10 条**都问得出来**（学得进 → 用得上）");

    /* 存盘 → 读回 → 还在吗 */
    size_t sz = ai_save_file(ai, "_durable.bin");
    AI *back = sz ? ai_load_file("_durable.bin") : NULL;
    ok(back != NULL, "存盘 → 读回：文件能读回来");
    if (back) {
        int kept = 0;
        for (int i = 0; i < n; ++i) kept += known(back, f[i][0], f[i][1], f[i][2]);
        ok(kept == n, "存读往返之后：教过的事实仍然在");
        AiCheck ck;
        ok(aip_check_ai(back, &ck) == 1, "读回来的图通过不变式");
        ai_free(back);
    }
    remove("_durable.bin");
    {
        size_t edges1 = 0;
        for (size_t i = 0; i < ai->pool->count; ++i)
            if (ai->pool->tokens[i]) edges1 += ai->pool->tokens[i]->count;
        printf("  [账] 维护 %d 轮；总边 %zu → %zu；反查条目 %zu；GC 删边/删词累计 %zu\n",
               rounds, edges0, edges1, ai->pool->rev_n,
               ai->n_gc_deleted_edges + ai->n_gc_deleted_tokens);
    }

    ai_free(ai);
    free(g_ag);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
