/* tools/scoreprobe.c — 量 `ai_sent_score_row`（整句检索打分那一趟）在**当前库句数**下的代价
 *
 * 背景（§6.209）：外部审查说 `ai_sent_score_row` 是 O(pairs_n) 全库扫，规模上去必须换倒排索引。
 * 但"该不该现在换"要看**今天**的账 —— 这个探针就是那笔账：
 *
 *   ./scoreprobe model.bin 200 "你好"
 *
 * 输出：pairs_n、每次 row 的毫秒数（含记忆化命中/失效两种）。
 * 记忆化键是 (tick, pairs_n, idf_n, 查询词)，所以每次把 tick 往前推一格来强制重算。
 *
 * 编译：cc -std=c11 -O2 -o scoreprobe tools/scoreprobe.c -lm
 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "model.bin";
    int reps = (argc > 2) ? atoi(argv[2]) : 100;
    const char *q = (argc > 3) ? argv[3] : "你好";
    AI *ai = ai_load_file(path);
    if (!ai) { fprintf(stderr, "load failed: %s\n", path); return 1; }

    size_t nw = 0;
    char **words = ai_tokenize(ai, q, &nw);
    if (!words || nw == 0) { fprintf(stderr, "tokenize failed\n"); return 1; }
    Token **toks = (Token **)calloc(nw, sizeof *toks);
    size_t nt = 0;
    for (size_t i = 0; i < nw; ++i) {
        Token *t = pool_token_intern(ai->pool, words[i]);
        if (t) toks[nt++] = t;
    }
    ai_free_words(words, nw);
    if (nt == 0) { fprintf(stderr, "no tokens\n"); return 1; }

    /* 预筛之后真正要算分的候选有多少（这是 0.45 ms 花在哪的关键） */
    (void)ai_char_query_prep(ai->pool ? ai->pool->serial : 0, toks, nt);
    double p0 = now_ms();
    size_t cand = 0, nopre = 0;
    for (size_t i = 0; i < ai->pairs_n; ++i) {
        const uint64_t *cbv = ai_pair_cb_bits(ai, i);
        if (!cbv) { nopre++; continue; }
        if (ai_cb_any(g_charq.cbits, cbv)) cand++;
    }
    double p1 = now_ms();
    printf("pairs_n=%zu tokens_n=%zu query=%s  prefilter_cand=%zu （无位图 %zu）"
           "  预筛循环=%.3f ms\n",
           ai->pairs_n, nt, q, cand, nopre, p1 - p0);

    /* 预热：把 cb_bits 数组拉进缓存，再量"只做预筛"的循环成本 */
    for (int w = 0; w < 3; ++w) {
        size_t cc = 0;
        for (size_t i = 0; i < ai->pairs_n; ++i) {
            const uint64_t *cbv = ai_pair_cb_bits(ai, i);
            if (cbv && ai_cb_any(g_charq.cbits, cbv)) cc++;
        }
        (void)cc;
    }
    {
        double w0 = now_ms();
        int reps2 = 50;
        for (int r = 0; r < reps2; ++r) {
            size_t cc = 0;
            for (size_t i = 0; i < ai->pairs_n; ++i) {
                const uint64_t *cbv = ai_pair_cb_bits(ai, i);
                if (cbv && ai_cb_any(g_charq.cbits, cbv)) cc++;
            }
            (void)cc;
        }
        double w1 = now_ms();
        printf("预筛循环（热，仅扫描）: %.3f ms/次\n", (w1 - w0) / reps2);
    }

    /* ① 首趟（记忆化失效，全库真算） */
    ai->pool->current_tick++;
    double t0 = now_ms();
    const int *row = ai_sent_score_row(ai, toks, nt, NULL);
    double t1 = now_ms();
    printf("miss（全库真算）      : %.3f ms  (row=%p)\n", t1 - t0, (const void *)row);

    /* ② 重复趟（记忆化命中，应当接近 0） */
    double t2 = now_ms();
    for (int i = 0; i < reps; ++i) (void)ai_sent_score_row(ai, toks, nt, NULL);
    double t3 = now_ms();
    printf("hit ×%d（记忆化）     : %.3f ms/次\n", reps, (t3 - t2) / (reps ? reps : 1));

    /* ③ 强制失效 ×reps（真正在规模上要花钱的那条） */
    double t4 = now_ms();
    for (int i = 0; i < reps; ++i) {
        ai->pool->current_tick++;
        (void)ai_sent_score_row(ai, toks, nt, NULL);
    }
    double t5 = now_ms();
    double per = (t5 - t4) / (reps ? reps : 1);
    printf("miss ×%d（每次全库）  : %.3f ms/次  ⇒ 每 10 倍库句 ≈ %.1f ms/次\n",
           reps, per, per * 10.0);

    free(toks);
    ai_free(ai);
    return 0;
}
