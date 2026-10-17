/* tools/openprobe.c — 量开放式关系问句（`谁 R B` 反查 / `A R 什么` 正查）的代价
 *
 * 背景（§6.211）：外部审查说 `agent_open_relation` 的反查是 O(pool->count) 全池扫，
 * 规模上去必须建反向索引。先量今天的账：
 *   ./openprobe session.bin 100 "谁 会 飞"      ← 反查（全池）
 *   ./openprobe session.bin 100 "猫 会 什么"    ← 正查（走邻居）
 * 输出：每次的毫秒数 + 扫了多少词（ai->n_scan_tokens 的增量）。
 *
 * 编译：cc -std=c11 -O2 -o openprobe tools/openprobe.c -lm
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
    int reps = (argc > 2) ? atoi(argv[2]) : 100;
    const char *q = (argc > 3) ? argv[3] : "谁 会 飞";
    AI *ai = ai_load_file(path);
    if (!ai) { fprintf(stderr, "load failed: %s\n", path); return 1; }
    size_t nw = 0;
    char **words = ai_tokenize(ai, q, &nw);
    if (!words || nw == 0) { fprintf(stderr, "tokenize failed\n"); return 1; }
    ai->rel_open = 1;

    size_t before = ai->n_scan_tokens;
    float conf = 0.0f;
    char *r = agent_open_relation(ai, words, nw, &conf);
    size_t after = ai->n_scan_tokens;
    printf("pairs_n=%zu pool_n=%zu query=%s  answer=%s  scanned=%zu\n",
           ai->pairs_n, ai->pool->count, q, r ? r : "(null)", after - before);
    free(r);

    double t0 = now_ms();
    for (int i = 0; i < reps; ++i) { free(agent_open_relation(ai, words, nw, &conf)); }
    double t1 = now_ms();
    printf("×%d : %.3f ms/次\n", reps, (t1 - t0) / (reps ? reps : 1));

    ai_free_words(words, nw);
    ai_free(ai);
    return 0;
}
