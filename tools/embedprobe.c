/* tools/embedprobe.c — 量 `ai_embed_build`（分布式表示）的时间/内存规模（§6.235）
 *
 *   ./embedprobe session.bin 5
 *
 * 输出：词数、边数、单次耗时、按当前实现估算的峰值内存（acc + E 两块 float/int32 矩阵）。
 * 目的：给"要不要做分块（chunked）"一个数 —— 分块会改浮点累加次序（emb 数值变），
 * 或者按块重扫全边（时间 × 块数），都不是"顺手"能做的。
 *
 * 编译：cc -std=c11 -O2 -o embedprobe tools/embedprobe.c -lm
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
    const char *path = (argc > 1) ? argv[1] : "session.bin";
    int reps = (argc > 2) ? atoi(argv[2]) : 3;
    AI *ai = ai_load_file(path);
    if (!ai) { fprintf(stderr, "load failed: %s\n", path); return 1; }
    size_t n = ai->pool->count, e = 0;
    for (size_t i = 0; i < n; ++i) e += ai->pool->tokens[i]->count;
    printf("tokens=%zu edges=%zu VSA_DIM=%d\n", n, e, VSA_DIM);
    printf("估算峰值：acc=int32[n*%d]=%.1f MB，E=float[n*%d]=%.1f MB（合计 %.1f MB）\n",
           VSA_DIM, (double)n * VSA_DIM * 4.0 / 1048576.0,
           VSA_DIM, (double)n * VSA_DIM * 4.0 / 1048576.0,
           2.0 * (double)n * VSA_DIM * 4.0 / 1048576.0);
    /* rv 生成占多少：每条边的正/反向各生成一次 1024 维 ±1 向量 */
    {
        int8_t rv[VSA_DIM];
        int reps2 = 100000;
        double r0 = now_ms();
        for (int i = 0; i < reps2; ++i) emb_randvec((uint32_t)i, rv);
        double r1 = now_ms();
        double per = (r1 - r0) / reps2 * 1000.0;   /* 微秒/次 */
        printf("emb_randvec：%.3f µs/次 ⇒ 按 2×edges 次计 ≈ %.1f ms\n",
               per, per * (double)(2 * e) / 1000.0);
    }
    double t0 = now_ms();
    for (int i = 0; i < reps; ++i) ai_embed_build(ai);
    double t1 = now_ms();
    printf("ai_embed_build × %d：%.2f ms/次\n", reps, (t1 - t0) / (reps ? reps : 1));
    ai_free(ai);
    return 0;
}
