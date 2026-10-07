/* tools/d4probe.c — D4「条件依赖」阈值实测
 *
 * 目的：把「70% / 3 次」这两个拍出来的阈值放到**真实语料**上量一量：
 * 回放语料（每行当一轮），看候选三元组的分布，以及不同阈值下会提多少条。
 *
 * 用法：./build_d4probe <模型> <语料> [最多回放多少行]
 * 输出：分母/分子分布 + 各阈值组合下的提议条数 + 前若干条样例（人工看一眼像不像话）。 */
#include "../agent.h"
#include "../token_lang.h"
#include "../model_io.h"
#include <stdio.h>

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    const char *corpus = (argc > 2) ? argv[2] : "_corpus.txt";
    size_t limit = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 0;
    AI *ai = ai_load_file(model);
    if (!ai) { printf("load fail: %s\n", model); return 1; }
    Micro *mk = micro_of(ai);
    mk->meta.cond_off = 0;                       /* 强行打开检测器 */

    FILE *f = fopen(corpus, "r");
    if (!f) { printf("open fail: %s\n", corpus); ai_free(ai); return 1; }
    char line[1024];
    size_t lines = 0, turns = 0;
    while (fgets(line, sizeof line, f)) {
        lines++;
        if (limit && lines > limit) break;
        size_t n = 0;
        char **w = ai_tokenize(ai, line, &n);
        if (w && n >= 2) {
            ai->pool->current_tick++;
            micro_detect_conditional(ai, w, n, mk);
            turns++;
        }
        ai_free_words(w, n);
        mod_queue_clear(&mk->queue);             /* 只看统计，不落地 */
    }
    fclose(f);
    printf("回放：%zu 行语料（有效轮 %zu）\n", lines, turns);
    printf("候选表：triple=%zu/%d\n", mk->meta.cond_tri_n, MICRO_D4_MAX_TRIPLES);
    /* 分布：分子/分母 的比值分档 */
    int band[5] = {0, 0, 0, 0, 0};               /* <50 / 50-69 / 70-84 / 85-99 / 100 */
    int pairs_ok = 0;
    for (size_t i = 0; i < mk->meta.cond_tri_n; ++i) {
        CondTriple *c = &mk->meta.cond_tri[i];
        if (!c->a || c->n_pair < 2) continue;
        uint32_t np = c->n_pair;
        if (np >= MICRO_D4_MIN_PAIRS) pairs_ok++;
        if (np == 0) continue;
        int pct = (int)((c->n_triple * 100u) / np);
        if (pct < 50) band[0]++;
        else if (pct < 70) band[1]++;
        else if (pct < 85) band[2]++;
        else if (pct < 100) band[3]++;
        else band[4]++;
    }
    printf("分母够（n_pair ≥ %d）的候选：%d 条\n", MICRO_D4_MIN_PAIRS, pairs_ok);
    printf("三元组分子/分母比值分布：<50%%=%d  50-69%%=%d  70-84%%=%d  85-99%%=%d  100%%=%d\n",
           band[0], band[1], band[2], band[3], band[4]);
    /* 各阈值组合下会提多少条（只数，不落地） */
    printf("\n阈值组合 → 会提几条（分子门槛 × 比例门槛）：\n");
    for (int mint = 2; mint <= 4; ++mint) {
        for (int pct = 50; pct <= 90; pct += 10) {
            size_t fire = 0;
            for (size_t i = 0; i < mk->meta.cond_tri_n; ++i) {
                CondTriple *c = &mk->meta.cond_tri[i];
                if (!c->a || !c->p) continue;
                if (c->n_triple < (uint32_t)mint) continue;
                uint32_t np = c->n_pair;
                if (np < (uint32_t)mint) continue;
                if ((int)((c->n_triple * 100u) / np) >= pct) fire++;
            }
            printf("  分子≥%d 且 ≥%d%% ： %zu 条\n", mint, pct, fire);
        }
    }
    /* 样例：**当前阈值下真会提的那些**（人工看一眼像不像话） */
    printf("\n当前阈值（分子≥%d 且 ≥%d%%）下真会提的：\n",
           MICRO_D4_MIN_TRIPLE, MICRO_D4_PCT_NUM);
    size_t shown = 0;
    for (size_t i = 0; i < mk->meta.cond_tri_n; ++i) {
        CondTriple *c = &mk->meta.cond_tri[i];
        if (!c->a || !c->b || !c->p) continue;
        if (c->n_triple < MICRO_D4_MIN_TRIPLE) continue;
        if (c->n_pair < MICRO_D4_MIN_PAIRS) continue;
        if ((size_t)c->n_triple * MICRO_D4_PCT_DEN < (size_t)c->n_pair * MICRO_D4_PCT_NUM) continue;
        printf("  %s —[前提]→ %s → %s   （%u/%u = %d%%）\n",
               c->a->name ? c->a->name : "?", c->p->name ? c->p->name : "?",
               c->b->name ? c->b->name : "?", c->n_triple, c->n_pair,
               (int)(c->n_triple * 100u / c->n_pair));
        shown++;
    }
    printf("  （共 %zu 条）\n", shown);
    ai_free(ai);
    return 0;
}
