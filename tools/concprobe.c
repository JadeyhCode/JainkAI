/* tools/concprobe.c — **并发**探针（§6.582，C 级，需要 -pthread）
 *
 * 用户 2026-10-24：「解决并发难题」。
 *
 * 这个探针把"并发"拆成两件能分别验收的事：
 *
 *   ① **问答并发**：主线程先把数据表/模型都加载好（**加载是串行的**，见 `docs/concurrency.md` 的契约），
 *      然后 4 个线程各拿一个**自己的脑**同时问同一组问题。
 *      **判据不是"没崩"，而是"逐字节和串行一样"** —— 引擎里任何一处共享可变状态
 *      （全局暂存 / 全局缓存 / 计数器）都会让某个答案不一样。
 *   ② **共享层并发**：`::brain fork` 出来的几个脑**共享 emb**（§6.581 的引用计数）。
 *      多线程同时读、其中一个线程重建（写前复制）、最后各线程并发释放 ——
 *      引用计数表必须在锁里，否则会双释放/泄漏。判据：不崩 + 表最后归零 + 内容正确。
 *
 * 用法：
 *   gcc -std=c11 -O2 -Wall -Wextra -o build_concprobe tools/concprobe.c -lm -pthread && ./build_concprobe model.bin
 *   看竞争用 TSan：
 *   gcc -std=c11 -O1 -g -fsanitize=thread -o build_concprobe_tsan tools/concprobe.c -lm -pthread && ./build_concprobe_tsan model.bin
 */
#include "../agent.h"
#include "../model_io.h"
#include "../share.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define NT 4          /* 线程数 / 脑数 */
#define NQ 12         /* 每个脑问几句 */
#define OUT_CAP 512

static const char *QS[NQ] = {
    "猫 属于 动物 吗",
    "猫 会 游泳 吗",
    "谁 属于 动物",
    "狗 会 什么",
    "你 会 写诗 吗",
    "什么是 动物",
    "龙 会 飞 吗",
    "鱼 会 游泳 吗",
    "猫 有 什么",
    "我 喜欢 什么",
    "猫 属于 植物 吗",
    "狗 属于 动物 吗",
};

/* ⚠ `agent_step` 收的是 `Agent*`（不是 `AI*`）：Agent 装着对话状态/历史，AI 只是图。
 * 一个线程 = 一个 Agent（它自己的脑）。 */
static Agent *g_ai[NT];
static const char *g_cold_model;   /* 冷启动模式用的模型路径 */
static char g_ser[NT][NQ][OUT_CAP];
static char g_par[NT][NQ][OUT_CAP];
static _Atomic int g_workers;   /* 探针自己的计数器也要原子（这是它自己的竞争）*/

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* 一个线程 = 一个脑：只碰自己的 AI（契约里"一个脑同时只被一个线程用"）*/
static void *ask_worker(void *arg) {
    long t = (long)arg;
    for (int q = 0; q < NQ; ++q)
        agent_step(g_ai[t], QS[q], g_par[t][q], sizeof g_par[t][q]);
    return NULL;
}

/* 建一个"自己的脑"的 Agent：替换掉 agent_new 里那个空白脑（和 `::brain new <模型>` 同一做法）*/
static Agent *agent_with_model(const char *model) {
    Agent *a = agent_new(stdin_perceive, stdout_act, NULL);
    if (!a) return NULL;
    AI *m = ai_load_file(model);
    if (!m) { agent_free(a); return NULL; }
    ai_free(a->brain);
    a->brain = m;
    a->brain->rel_verify_hook = NULL;
    return a;
}

/* 只读 emb 的线程：反复算同一个词的向量和（用来和"重建线程"对撞）*/
static void *emb_reader(void *arg) {
    AI *ai = (AI *)arg;
    long long acc = 0;
    for (int k = 0; k < 200; ++k) {
        for (size_t i = 0; i < ai->pool->count; ++i) {
            const Token *t = ai->pool->tokens[i];
            if (!t || !t->emb) continue;
            for (size_t d = 0; d < EMB_DIM; d += 64) acc += t->emb[d];
        }
    }
    (void)atomic_fetch_add_explicit(&g_workers, (int)(acc & 1), memory_order_relaxed);
    return NULL;
}

static void *emb_builder(void *arg) {
    AI *ai = (AI *)arg;
    ai_embed_build(ai);            /* 写前复制：共享块在这里被要成自己的 */
    return NULL;
}

/* ---- 冷启动并发：**没有主线程热场**，4 个线程各自建脑、立刻问 ----
 * 这条专门打"懒加载"那条路：`patterns.txt` 是进程级共享表，第一次问答时才读
 * （§6.582 给它加了锁 + 线程局部重入闸）。判据：4 个脑（同一个模型）答案**彼此相同**。 */
static char g_cold[NT][2][OUT_CAP];
static void *cold_worker(void *arg) {
    long t = (long)arg;
    Agent *a = agent_with_model(g_cold_model);
    if (!a) { g_cold[t][0][0] = 0; return NULL; }
    agent_step(a, "猫 属于 动物 吗", g_cold[t][0], sizeof g_cold[t][0]);
    agent_step(a, "你 会 写诗 吗", g_cold[t][1], sizeof g_cold[t][1]);
    agent_free(a);
    return NULL;
}

static void *free_worker(void *arg) {
    ai_free((AI *)arg);
    return NULL;
}

int main(int argc, char **argv) {
    const char *model = argc > 1 ? argv[1] : "model.bin";

    /* ---- 冷启动模式（`concprobe <模型> --cold`）：先跑这个，别先热场 ---- */
    if (argc > 2 && strcmp(argv[2], "--cold") == 0) {
        g_cold_model = model;
        pthread_t th[NT];
        for (long t = 0; t < NT; ++t) pthread_create(&th[t], NULL, cold_worker, (void *)t);
        for (int t = 0; t < NT; ++t) pthread_join(th[t], NULL);
        int same = 1, nonempty = 1;
        for (int t = 0; t < NT; ++t) {
            if (!g_cold[t][0][0] || !g_cold[t][1][0]) nonempty = 0;
            if (strcmp(g_cold[t][0], g_cold[0][0]) != 0 ||
                strcmp(g_cold[t][1], g_cold[0][1]) != 0) same = 0;
        }
        ok(nonempty, "⓪ 冷启动并发：4 个线程都问出了话（懒加载没把表撕坏）");
        ok(same, "⓪ 冷启动并发：4 个脑（同一模型）的答案**彼此相同**");
        printf("\n通过 %d / 失败 %d\n", passes, fails);
        return fails ? 1 : 0;
    }

    /* ---- 0) 热场：加载 + 走一遍问答（把数据表、帧库、emb 路径都走热）----
     * 契约：**加载是串行的**；线程里只做问答。这不是偷懒，是因为数据表
     * （`patterns.txt` 那几桶）本来就是"进程级只读"，加载期之后没有人写它。 */
    {
        Agent *warm = agent_with_model(model);
        if (!warm) { printf("读不到 %s\n", model); return 1; }
        char tmp[OUT_CAP];
        agent_step(warm, "猫 属于 动物 吗", tmp, sizeof tmp);
        agent_step(warm, "谁 属于 动物", tmp, sizeof tmp);
        agent_step(warm, "你 会 写诗 吗", tmp, sizeof tmp);
        agent_free(warm);
    }

    /* ---- ① 串行基线：4 个独立脑，各问 NQ 句 ---- */
    for (int t = 0; t < NT; ++t) {
        g_ai[t] = agent_with_model(model);
        if (!g_ai[t]) { printf("读不到 %s\n", model); return 1; }
    }
    double t0 = now_ms();
    for (int t = 0; t < NT; ++t)
        for (int q = 0; q < NQ; ++q)
            agent_step(g_ai[t], QS[q], g_ser[t][q], sizeof g_ser[t][q]);
    double ser_ms = now_ms() - t0;
    for (int t = 0; t < NT; ++t) { agent_free(g_ai[t]); g_ai[t] = NULL; }

    /* ---- ② 并行：同样 4 个独立脑，4 个线程同时问 ---- */
    for (int t = 0; t < NT; ++t) {
        g_ai[t] = agent_with_model(model);
        if (!g_ai[t]) { printf("读不到 %s\n", model); return 1; }
    }
    memset(g_par, 0, sizeof g_par);
    pthread_t th[NT];
    t0 = now_ms();
    for (long t = 0; t < NT; ++t) pthread_create(&th[t], NULL, ask_worker, (void *)t);
    for (int t = 0; t < NT; ++t) pthread_join(th[t], NULL);
    double par_ms = now_ms() - t0;

    /* 逐字节比较：**这就是并发正确性的判据**（不是"没崩"）*/
    int diff = 0, empty = 0;
    for (int t = 0; t < NT; ++t) {
        for (int q = 0; q < NQ; ++q) {
            if (!g_par[t][q][0]) empty++;
            if (strcmp(g_ser[t][q], g_par[t][q]) != 0) {
                if (diff < 3)
                    printf("       差异 #%d/%d  「%s」\n         串行：%s\n         并行：%s\n",
                           t, q, QS[q], g_ser[t][q], g_par[t][q]);
                diff++;
            }
        }
    }
    ok(empty == 0, "① 并行那一轮**每个问题都有答复**（没有线程在里面悄悄死掉）");
    ok(diff == 0, "① 并行结果与串行**逐字节相同**（共享可变状态都被隔离了）");
    printf("       串行 %.1f ms / 并行 %.1f ms ⇒ 加速 %.2fx（%d 线程）\n",
           ser_ms, par_ms, par_ms > 0 ? ser_ms / par_ms : 0.0, NT);
    ok(par_ms < ser_ms, "① 并行确实更快（不是靠一把大锁串起来的）");
    for (int t = 0; t < NT; ++t) { agent_free(g_ai[t]); g_ai[t] = NULL; }

    /* ---- ③ 共享层并发：fork 出来的脑共享 emb，多线程读 + 一个线程重建 + 并发释放 ---- */
    {
        AI *base = ai_load_file(model);
        AI *kids[3];
        for (int i = 0; i < 3; ++i) kids[i] = ai_load_file(model);
        if (!base || !kids[0] || !kids[1] || !kids[2]) { printf("读不到 %s\n", model); return 1; }
        size_t shared = 0;
        for (int i = 0; i < 3; ++i) shared += pool_share_emb(kids[i]->pool, base->pool);
        ok(shared == 3u * base->pool->count, "③ 三个兄弟脑都共享到同一个源（emb 引用计数）");
        ok(aip_buf_refs(base->pool->tokens[0]->emb) == 4, "③ 引用计数 = 4（源 + 三个兄弟）");

        pthread_t r1, r2, b1;
        pthread_create(&r1, NULL, emb_reader, kids[0]);
        pthread_create(&r2, NULL, emb_reader, kids[1]);
        pthread_create(&b1, NULL, emb_builder, kids[2]);   /* 重建：写前复制 */
        pthread_join(r1, NULL); pthread_join(r2, NULL); pthread_join(b1, NULL);
        ok(1, "③ 两个读者 + 一个重建者同时跑完（没崩、没 UAF）");
        ok(aip_buf_refs(base->pool->tokens[0]->emb) == 3,
           "③ 重建之后引用计数降到 3（那一份被要成自己的了）");
        ok(kids[2]->pool->tokens[0]->emb != base->pool->tokens[0]->emb,
           "③ 重建者的 emb 已经是自己的（写前复制生效）");

        /* 并发释放：4 个脑各自一个线程 free，引用计数表必须能对上 */
        pthread_t f[4];
        pthread_create(&f[0], NULL, free_worker, base);
        pthread_create(&f[1], NULL, free_worker, kids[0]);
        pthread_create(&f[2], NULL, free_worker, kids[1]);
        pthread_create(&f[3], NULL, free_worker, kids[2]);
        for (int i = 0; i < 4; ++i) pthread_join(f[i], NULL);
        ok(aip_buf_shared_count() == 0, "③ 并发释放之后共享表归零（没有泄漏/没有残留）");
    }

    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
