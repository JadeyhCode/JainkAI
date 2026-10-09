/* tools/driveprobe.c — 「驱动耦合表」到底会不会被触发 + 驱动量**彼此独不独立**？（量出来，别猜）
 *
 * 局限登记里写着两条：「驱动耦合只有 5 行、参数手调」「舒适×唤醒共线 r=0.91」。
 * 这张表值不值得扩、要不要加维度，先得知道：
 *   ① 每一行在真语料上**命中不命中**（全在区间外 = 这张表等于没有）；
 *   ② 几个驱动量**彼此独不独立** —— 两个量强相关就意味着两行在说同一件事。
 *
 * ⚠ 血泪（2026-10-07）：原来这里算相关是**先把两个序列各自排序再算**的，
 *   那不是配对数据的相关（排序把"同一轮的搭配"打散了）。现在改成
 *   **按轮配对**存下来、算真 Pearson r，并打一张**相关矩阵**（所有驱动量两两）。
 *
 * 用法：./build_driveprobe <模型> <语料> [最多回放多少行]
 * 做法：回放语料，每行走一次真流水线（`agent_step`）再走一次 `drive_policy_step`，
 *       统计每行命中次数、驱动量分布、以及两两相关。
 */
#include "../agent.h"
#include "../drive.h"
#include "../model_io.h"
#include <stdio.h>
#include <math.h>

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 驱动量清单：**名字来自层里那张表**用的同一组字（`ai_drive_value` 认这些） */
static const char *DRIVES[] = { "舒适", "唤醒", "疲劳", "不适", "好奇", "亲近" };
#define NDRIVE ((int)(sizeof DRIVES / sizeof DRIVES[0]))

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    const char *corpus = (argc > 2) ? argv[2] : "_corpus.txt";
    size_t limit = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 20000;
    AI *ai = ai_load_file(model);
    if (!ai) { printf("load fail\n"); return 1; }
    FILE *f = fopen(corpus, "r");
    if (!f) { printf("open fail: %s\n", corpus); ai_free(ai); return 1; }
    if (limit < 2) limit = 2;

    float **v = (float **)calloc(NDRIVE, sizeof *v);      /* 每个驱动量一列，**按轮配对** */
    float **sorted = (float **)calloc(NDRIVE, sizeof *sorted);
    if (!v || !sorted) { fclose(f); ai_free(ai); return 1; }
    for (int d = 0; d < NDRIVE; ++d) {
        v[d] = (float *)malloc(limit * sizeof *v[d]);
        sorted[d] = (float *)malloc(limit * sizeof *sorted[d]);
        if (!v[d] || !sorted[d]) { fclose(f); ai_free(ai); return 1; }
    }
    size_t hits[AI_DRIVE_N + 1];
    for (size_t i = 0; i <= AI_DRIVE_N; ++i) hits[i] = 0;
    double sum[NDRIVE];
    for (int d = 0; d < NDRIVE; ++d) sum[d] = 0.0;
    size_t lines = 0;
    char line[1024];
    while (fgets(line, sizeof line, f) && lines < limit) {
        /* **走真流水线**：只用 ai_inner_update(固定惊讶) 喂的话，量的是探针自己
         * （实测：5000 行常惊讶把疲劳顶到 0.90、舒适压到 0.10 —— 那是探针的毛病，
         *  不是系统的脾气）。这里就按 CLI 一样跑一整轮，再看驱动量与耦合表。 */
        Agent ag;
        memset(&ag, 0, sizeof ag);
        ag.brain = ai;
        char out[512];
        (void)agent_step(&ag, line, out, sizeof out);
        drive_policy_step(ai);          /* 一轮结束后再折算（就是实际生效的那一步） */
        for (size_t i = 0; i < AI_DRIVE_N; ++i) {
            const DriveCoupling *c = &AI_DRIVE[i];
            float x = ai_drive_value(ai, c->drive);
            if (x >= c->lo && x <= c->hi) hits[i]++;
        }
        for (int d = 0; d < NDRIVE; ++d) {
            float x = ai_drive_value(ai, DRIVES[d]);
            v[d][lines] = x;
            sorted[d][lines] = x;
            sum[d] += (double)x;
        }
        lines++;
        if (lines % 500 == 0)          /* 轨迹：看疲劳是不是"睡一觉就缓过来" */
            printf("    [%5zu 轮] 舒适 %.2f 唤醒 %.2f 疲劳 %.2f 不适 %.2f 好奇 %.2f 亲近 %.2f 睡过 %zu 次\n",
                   lines, (double)ai->inner.comfort, (double)ai->inner.arousal,
                   (double)ai->inner.fatigue, (double)ai->inner.discomfort,
                   (double)ai_drive_value(ai, "好奇"), (double)ai_drive_value(ai, "亲近"),
                   ai->n_sleeps);
    }
    fclose(f);
    if (lines == 0) { printf("语料是空的\n"); ai_free(ai); return 0; }
    printf("回放 %zu 行（每行都算一次耦合表）\n", lines);
    printf("  驱动量均值：");
    for (int d = 0; d < NDRIVE; ++d)
        printf("%s %.3f%s", DRIVES[d], sum[d] / (double)lines, (d + 1 < NDRIVE) ? "  " : "\n");
    printf("  耦合表命中（区间 [lo,hi] 落点）：\n");
    for (size_t i = 0; i < AI_DRIVE_N; ++i) {
        const DriveCoupling *c = &AI_DRIVE[i];
        printf("    %-8s → %-8s  区间[%.2f,%.2f]  修正 %+d  命中 %zu 次（%.1f%%）  %s\n",
               c->drive, c->param, (double)c->lo, (double)c->hi, c->delta, hits[i],
               100.0 * (double)hits[i] / (double)lines, c->note ? c->note : "");
    }
    printf("  驱动量分位（p05 / p50 / p95）：\n");
    for (int d = 0; d < NDRIVE; ++d) {
        qsort(sorted[d], lines, sizeof *sorted[d], cmpf);
        printf("    %-4s %.2f / %.2f / %.2f / %.2f（p05/p50/p90/p95）\n", DRIVES[d],
               (double)sorted[d][lines / 20], (double)sorted[d][lines / 2],
               (double)sorted[d][(lines * 9) / 10], (double)sorted[d][(lines * 19) / 20]);
    }
    /* ---- **真 Pearson 相关矩阵**（按轮配对；不是"各自排序再算"）---- */
    printf("  相关矩阵（Pearson r，按轮配对）：\n");
    printf("        ");
    for (int d = 0; d < NDRIVE; ++d) printf("%6s", DRIVES[d]);
    printf("\n");
    for (int i = 0; i < NDRIVE; ++i) {
        printf("    %-4s", DRIVES[i]);
        for (int j = 0; j < NDRIVE; ++j) {
            if (j > i) { printf("%6s", "·"); continue; }
            if (j == i) { printf("%6s", "1.00"); continue; }
            double mi = sum[i] / (double)lines, mj = sum[j] / (double)lines;
            double sij = 0, sii = 0, sjj = 0;
            for (size_t k = 0; k < lines; ++k) {
                double a = v[i][k] - mi, b = v[j][k] - mj;
                sij += a * b; sii += a * a; sjj += b * b;
            }
            double r = (sii > 0 && sjj > 0) ? sij / sqrt(sii * sjj) : 0.0;
            printf("%6.2f", r);
        }
        printf("\n");
    }
    printf("    |r| ≥ 0.7 的**算共线**（两行其实在说同一件事）；矩阵里没有这种格子才算「维度独立」\n");
    /* ---- **一阶差分**矩阵（Δ每轮）：原值矩阵量的是"都在随时间漂移"，
     *      差分把共同趋势去掉，剩下的才是"两个量**机制上**是不是一回事"。 ---- */
    printf("  相关矩阵（**一阶差分** Δ，去掉共同漂移）:\n");
    printf("        ");
    for (int d = 0; d < NDRIVE; ++d) printf("%6s", DRIVES[d]);
    printf("\n");
    for (int i = 0; i < NDRIVE; ++i) {
        printf("    %-4s", DRIVES[i]);
        for (int j = 0; j < NDRIVE; ++j) {
            if (j > i) { printf("%6s", "·"); continue; }
            if (j == i) { printf("%6s", "1.00"); continue; }
            double mi = 0, mj = 0, sij = 0, sii = 0, sjj = 0, cnt = 0;
            for (size_t k = 1; k < lines; ++k) {
                mi += (double)v[i][k] - v[i][k - 1];
                mj += (double)v[j][k] - v[j][k - 1];
                cnt += 1.0;
            }
            if (cnt > 0) { mi /= cnt; mj /= cnt; }
            for (size_t k = 1; k < lines; ++k) {
                double a = ((double)v[i][k] - v[i][k - 1]) - mi;
                double b = ((double)v[j][k] - v[j][k - 1]) - mj;
                sij += a * b; sii += a * a; sjj += b * b;
            }
            double r = (sii > 0 && sjj > 0) ? sij / sqrt(sii * sjj) : 0.0;
            printf("%6.2f", r);
        }
        printf("\n");
    }
    printf("    差分为 0（量根本不动）时算 0.00 —— 那种量当驱动量**没有意义**，要么换信号要么别用\n");
    size_t used = 0;
    for (size_t i = 0; i < AI_DRIVE_N; ++i) if (hits[i]) used++;
    printf("  实际参与过决策的行：%zu / %zu\n", used, (size_t)AI_DRIVE_N);
    if (used == 0) printf("  → 结论：这张表在真语料上**一行都没命中**（扩表之前先修区间/驱动量）\n");
    free(v); free(sorted);
    ai_free(ai);
    return 0;
}
