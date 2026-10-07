/* tools/affectprobe.c — 「情绪耦合表」到底会不会被触发？（量出来，别猜）
 *
 * 局限登记里写着：「情绪耦合只有 5 行、参数手调」。这张表值不值得扩，
 * 先要知道**它在真语料上命中不命中**：如果 `疲劳`/`唤醒`/`舒适` 全在区间外，
 * 那这张表再准也等于没有；如果经常命中，才谈得上调参。
 *
 * 用法：./build_affectprobe <模型> <语料> [最多回放多少行]
 * 做法：回放语料 → 每行走一次 `agent_step` 之外的最小路径（只更新内部状态：惊讶/疲劳/唤醒/舒适），
 *       每行都按 `AI_AFFECT` 表算一次命中，统计**每一行命中次数**与驱动量的分布。
 */
#include "../agent.h"
#include "../affect.h"
#include "../model_io.h"
#include <stdio.h>
#include <math.h>

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    const char *corpus = (argc > 2) ? argv[2] : "_corpus.txt";
    size_t limit = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 20000;
    AI *ai = ai_load_file(model);
    if (!ai) { printf("load fail\n"); return 1; }
    FILE *f = fopen(corpus, "r");
    if (!f) { printf("open fail: %s\n", corpus); ai_free(ai); return 1; }

    size_t cap = limit + 8;
    float *qc = (float *)malloc(cap * sizeof *qc);
    float *qa = (float *)malloc(cap * sizeof *qa);
    float *qf = (float *)malloc(cap * sizeof *qf);
    float *qd = (float *)malloc(cap * sizeof *qd);
    if (!qc || !qa || !qf || !qd) { fclose(f); ai_free(ai); return 1; }
    size_t hits[AI_AFFECT_N + 1];
    for (size_t i = 0; i <= AI_AFFECT_N; ++i) hits[i] = 0;
    double sum_comfort = 0, sum_arousal = 0, sum_fatigue = 0, sum_discomfort = 0;
    size_t lines = 0;
    char line[1024];
    while (fgets(line, sizeof line, f) && lines < limit) {
        lines++;
        /* **走真流水线**：只用 ai_inner_update(固定惊讶) 喂的话，量的是探针自己
         * （实测：5000 行常惊讶把疲劳顶到 0.90、舒适压到 0.10 —— 那是探针的毛病，
         *  不是系统的脾气）。这里就按 CLI 一样跑一整轮，再看驱动量与耦合表。 */
        Agent ag;
        memset(&ag, 0, sizeof ag);
        ag.brain = ai;
        char out[512];
        (void)agent_step(&ag, line, out, sizeof out);
        affect_policy_step(ai);          /* 一轮结束后再折算（就是实际生效的那一步） */
        for (size_t i = 0; i < AI_AFFECT_N; ++i) {
            const AffectCoupling *c = &AI_AFFECT[i];
            float v = ai_drive_value(ai, c->drive);
            if (v >= c->lo && v <= c->hi) hits[i]++;
        }
        if (lines % 500 == 0)          /* 轨迹：看疲劳是不是"睡一觉就缓过来" */
            printf("    [%5zu 轮] 舒适 %.2f 唤醒 %.2f 疲劳 %.2f 不适 %.2f 睡过 %zu 次\n",
                   lines, (double)ai->inner.comfort, (double)ai->inner.arousal,
                   (double)ai->inner.fatigue, (double)ai->inner.discomfort, ai->n_sleeps);
        sum_comfort += (double)ai->inner.comfort;
        sum_arousal += (double)ai->inner.arousal;
        sum_fatigue += (double)ai->inner.fatigue;
        sum_discomfort += (double)ai->inner.discomfort;
        if (lines <= cap) {
            qc[lines - 1] = ai->inner.comfort;
            qa[lines - 1] = ai->inner.arousal;
            qf[lines - 1] = ai->inner.fatigue;
            qd[lines - 1] = ai->inner.discomfort;
        }
    }
    fclose(f);
    printf("回放 %zu 行（每行都算一次耦合表）\n", lines);
    printf("  驱动量均值：舒适 %.3f  唤醒 %.3f  疲劳 %.3f  不适 %.3f\n",
           sum_comfort / (double)(lines ? lines : 1),
           sum_arousal / (double)(lines ? lines : 1),
           sum_fatigue / (double)(lines ? lines : 1),
           sum_discomfort / (double)(lines ? lines : 1));
    printf("  耦合表命中（区间 [lo,hi] 落点）：\n");
    for (size_t i = 0; i < AI_AFFECT_N; ++i) {
        const AffectCoupling *c = &AI_AFFECT[i];
        printf("    %-8s → %-8s  区间[%.2f,%.2f]  修正 %+d  命中 %zu 次（%.1f%%）  %s\n",
               c->drive, c->param, (double)c->lo, (double)c->hi, c->delta, hits[i],
               100.0 * (double)hits[i] / (double)(lines ? lines : 1), c->note ? c->note : "");
    }
    /* 分位数：阈值要按**实测分布**来定，别拍 */
    {
        size_t n = lines < cap ? lines : cap;
        if (n) {
            qsort(qc, n, sizeof *qc, cmpf);
            qsort(qa, n, sizeof *qa, cmpf);
            qsort(qf, n, sizeof *qf, cmpf);
            qsort(qd, n, sizeof *qd, cmpf);
            printf("  驱动量分位（p05 / p50 / p95）：\n");
            printf("    舒适 %.2f / %.2f / %.2f\n", (double)qc[n / 20], (double)qc[n / 2], (double)qc[(n * 19) / 20]);
            printf("    唤醒 %.2f / %.2f / %.2f\n", (double)qa[n / 20], (double)qa[n / 2], (double)qa[(n * 19) / 20]);
            printf("    疲劳 %.2f / %.2f / %.2f\n", (double)qf[n / 20], (double)qf[n / 2], (double)qf[(n * 19) / 20]);
            printf("    不适 %.2f / %.2f / %.2f\n", (double)qd[n / 20], (double)qd[n / 2], (double)qd[(n * 19) / 20]);
            /* 舒适与唤醒是不是同一个变量的正反面？（相关性强 → 两行说的是同一件事） */
            double sc = 0, sa = 0, sca = 0, vc = 0, va = 0;
            for (size_t i = 0; i < n; ++i) { sc += qc[i]; sa += qa[i]; }
            sc /= (double)n; sa /= (double)n;
            for (size_t i = 0; i < n; ++i) {
                double dc = qc[i] - sc, da = qa[i] - sa;
                sca += dc * da; vc += dc * dc; va += da * da;
            }
            double r = (vc > 0 && va > 0) ? sca / sqrt(vc * va) : 0.0;
            printf("  舒适×唤醒 相关系数 r=%.2f%s\n", r,
                   (r < -0.7) ? "（**强负相关**：这两行其实在说同一件事）" : "");
        }
    }
    size_t used = 0;
    for (size_t i = 0; i < AI_AFFECT_N; ++i) if (hits[i]) used++;
    printf("  实际参与过决策的行：%zu / %zu\n", used, (size_t)AI_AFFECT_N);
    if (used == 0) printf("  → 结论：这张表在真语料上**一行都没命中**（扩表之前先修区间/驱动量）\n");
    free(qc); free(qa); free(qf); free(qd);
    ai_free(ai);
    return 0;
}
