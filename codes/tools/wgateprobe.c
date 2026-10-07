/* tools/wgateprobe.c — 「写入分级闸门」评分在真实语料上的分布
 *
 * 目的：`imp = 0.25·生词 + 0.20·自模型信任 + 0.20·提及 + 0.20·信息量 + 0.075·惊讶 + 0.075·不累`
 * 这套评分在真模型上到底落在哪一段？阈值 0.40/0.70 是不是拍出来的？
 * 做法：回放语料，对每一对**已经有了关系边**的词算一次 imp，输出分布分档与分位数。
 *
 * 用法：./build_wgateprobe <模型> <语料> [最多回放多少行] */
#include "../agent.h"
#include "../token_lang.h"
#include "../model_io.h"
#include <stdio.h>

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    const char *corpus = (argc > 2) ? argv[2] : "_corpus.txt";
    size_t limit = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 20000;
    AI *ai = ai_load_file(model);
    if (!ai) { printf("load fail: %s\n", model); return 1; }
    double *v = (double *)malloc((limit + 8) * sizeof *v);
    if (!v) { ai_free(ai); return 1; }
    size_t n = 0;
    /* 关系边在真模型里很少（167 条）——直接**扫全图的带关系位的边**，
     * 它们就是"写进去过的知识"，正好是闸门要判断的那一批。 */
    (void)corpus; (void)limit;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *a = ai->pool->tokens[i];
        if (!a || a->is_garbage || !a->name || a->name[0] == '[') continue;
        for (size_t j = 0; j < a->count; ++j) {
            const AssocEntry *e = &a->assocs[j];
            if (!e->main || !(e->rel || e->nrel)) continue;
            if (!e->main->name || e->main->name[0] == '[') continue;
            if (n < limit) v[n++] = ai_write_importance(ai, a, e->main);
        }
    }
    printf("扫了全图的**关系边** %zu 条（真模型里就这么多）\n", n);
    if (n == 0) { free(v); ai_free(ai); return 0; }
    qsort(v, n, sizeof *v, cmp_double);
    size_t below40 = 0, above70 = 0;
    for (size_t i = 0; i < n; ++i) {
        if (v[i] < AI_WGATE_KNOWLEDGE) below40++;
        if (v[i] >= AI_WGATE_LONGTERM) above70++;
    }
    printf("  最小 %.3f  25%% %.3f  中位 %.3f  75%% %.3f  最大 %.3f\n",
           v[0], v[n / 4], v[n / 2], v[(n * 3) / 4], v[n - 1]);
    printf("  <%.2f（情境层，闸门开会丢弃）：%zu 条（%.1f%%）\n",
           AI_WGATE_KNOWLEDGE, below40, 100.0 * (double)below40 / (double)n);
    printf("  ≥%.2f（长期层）：%zu 条（%.1f%%）\n",
           AI_WGATE_LONGTERM, above70, 100.0 * (double)above70 / (double)n);
    printf("  自模型信任=%.3f 惊讶=%.3f 疲劳=%.3f\n",
           (double)ai->inner.predict_accuracy, (double)ai->inner.discomfort,
           (double)ai->inner.fatigue);
    free(v);
    ai_free(ai);
    return 0;
}
