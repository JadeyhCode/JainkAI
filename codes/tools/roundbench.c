/* tools/roundbench.c — **每轮开销基准 + 全图扫描记账**（§6.126）
 *
 * 为什么有它：用户要求"优化程序性能 / GC 改队列 / 尽量减少扫全局"。
 * 这类改动**必须先有数字**：每轮多少毫秒、每轮走了多少个词、多少条边。
 * 改完再量一遍，两边的数字都留在 `_t/roundbench_last.tsv`（下面自己追加）。
 *
 * 记账的口径见 ai.h 的 `n_scan_tokens` / `n_scan_edges` ——
 * 它们是维护类活动（GC / 缓存复核 / 巩固）**实际走过**的词数/边数，不是"池子里有多少"。
 *
 * 用法：gcc -std=c11 -O2 -o build_roundbench tools/roundbench.c -lm
 *       ./build_roundbench [模型文件] [轮数]        默认 model.bin 200 轮
 */
#include "../agent.h"
#include "../model_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 边总数（main.c 里的 ai_stats_edges 是 static，工具这边自己数一遍） */
static size_t edges_of(const AI *ai) {
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        if (t && !t->is_garbage) n += t->count;
    }
    return n;
}

/* 缓存（推论边）的规模：多少个词身上挂着推论边、一共多少条。
 * §6.127 的候选队列就是按"有推论边的词"入队的 —— 这个数决定了队列的大小。 */
static void caches_of(const AI *ai, size_t *words, size_t *edges) {
    size_t w = 0, e = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        int has = 0;
        for (size_t j = 0; j < t->count; ++j)
            if (t->assocs[j].role == ASSOC_ROLE_DERIVED && t->assocs[j].main) { e++; has = 1; }
        w += (size_t)has;
    }
    *words = w; *edges = e;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    int rounds = (argc > 2) ? atoi(argv[2]) : 200;
    if (rounds <= 0) rounds = 200;
    Agent *ag = agent_new(NULL, NULL, NULL);
    if (!ag) { printf("建脑失败\n"); return 1; }
    /* ⚠ 装载（读盘 + 现算 emb / idf / 槽类）是**一次性**开销，别把它摊进"每轮"里：
     * 100 轮口径下它能有 0.5~0.7 ms/轮（实测：`ai_slots_build` 一次 30 ms、
     * emb 的 PPMI 两遍累积也是一次性的）。分开计时才看得清"每轮真的花了多少"。 */
    double load_a = now_ms();
    AI *m = ai_load_file(model);
    if (m) { ai_free(ag->brain); ag->brain = m; }
    double load_b = now_ms();
    AI *ai = ag->brain;
    printf("  装载（一次性）：%.1f ms\n", load_b - load_a);

    /* 三种输入轮流：① 关系问句（走推理）② 普通话（走帧/检索）③ 陈述（走学习） */
    const char *inputs[3] = { "猫 有 毛 吗", "你 好 呀", "猫 属于 动物" };
    char out[1024];
    size_t cw = 0, ce = 0;
    caches_of(ai, &cw, &ce);
    size_t t0 = ai->n_scan_tokens, e0 = ai->n_scan_edges;
    double a = now_ms();
    for (int i = 0; i < rounds; ++i) {
        out[0] = 0;
        agent_step(ag, inputs[i % 3], out, sizeof out);
    }
    double b = now_ms();
    size_t dt = ai->n_scan_tokens - t0, de = ai->n_scan_edges - e0;
    printf("每轮 结论行：%.3f ms/轮（%d 轮共 %.1f ms）｜词=%zu 边=%zu\n",
           (b - a) / (double)rounds, rounds, b - a,
           (size_t)ai->pool->count, edges_of(ai));
    printf("  全图扫描/轮：词=%zu 边=%zu（维护类活动实际走过的量）\n",
           dt / (size_t)rounds, de / (size_t)rounds);
    printf("  推论缓存：%zu 个词身上挂着 / 共 %zu 条推论边（§6.127 队列按这个规模走）\n",
           cw, ce);
    printf("   打分备忘（§6.132）：查 %zu 次 / 真重算 %zu 次（%.0f%% 命中）\n",
           ai->n_sc_call, ai->n_sc_miss,
           ai->n_sc_call ? 100.0 * (double)(ai->n_sc_call - ai->n_sc_miss) / (double)ai->n_sc_call : 0.0);
    printf("   库句字符集位图（§6.136）：懒补过 %zu 条 / 库 %zu 条（**只补一次**，不是每轮）\n",
           ai->n_cb_fill, ai->pairs_n);
    printf("  维护开关：GC 每 %zu 轮、巩固每 %zu 轮、睡眠每 %zu 轮、缓存复核每轮\n",
           ai->gc.gc_interval, ai->consolidate_every, ai->sleep_every);
    printf("  睡眠激活（§6.135）：访问 %zu 个 / 上限截断 %zu 次（上限 %d）\n",
           ai->n_sleep_visits, ai->n_sleep_cuts, AI_SLEEP_MAX_VISITS);
    /* §6.135 分段计时：`AIP_SEGTIME=1` 才有一张非零的表（关着时全是 0，别打出来骗人）。 */
    if (ai_seg_on()) {
        uint64_t tot = 0;
        for (int s = 0; s < AI_SEG_N; ++s) tot += ai->seg_ns[s];
        printf("  分段计时（AIP_SEGTIME=1）：合计 %.3f ms/轮\n",
               tot / 1e6 / (double)rounds);
        for (int s = 0; s < AI_SEG_N; ++s) {
            if (!ai->seg_hits[s]) continue;
            printf("    %-24s %8.3f ms/轮  %5.1f%%  每次 %.1f µs（%u 次）\n",
                   AI_SEG_NAMES[s], ai->seg_ns[s] / 1e6 / (double)rounds,
                   tot ? 100.0 * (double)ai->seg_ns[s] / (double)tot : 0.0,
                   (double)ai->seg_ns[s] / 1e3 / (double)ai->seg_hits[s],
                   ai->seg_hits[s]);
        }
    }
    if (getenv("AIP_ROUNDBENCH_TSV")) {
        FILE *f = fopen(getenv("AIP_ROUNDBENCH_TSV"), "a");
        if (f) {
            fprintf(f, "%s\t%d\t%.3f\t%zu\t%zu\t%zu\t%zu\n", model, rounds,
                    (b - a) / (double)rounds, dt / (size_t)rounds, de / (size_t)rounds,
                    (size_t)ai->pool->count, edges_of(ai));
            fclose(f);
        }
    }
    { const char *sp = getenv("AIP_RB_SAVE"); if (sp) ai_save_file(ai, sp); }
    agent_free(ag);
    return 0;
}
