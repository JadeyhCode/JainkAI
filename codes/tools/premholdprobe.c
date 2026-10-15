/* tools/premholdprobe.c — 「前提成立」第三档（verified/有据）的判据到底有多严？
 *
 * 局限登记里写着：前提成立的判据是「≥3 条带命中的非推论边」（`AI_PREMISE_MIN_EDGES`/`MIN_HITS`）——
 * 这两个数是**拍的**，没量过。这个探针把它量出来：
 *   ① 全图符号节点的"有据度"分布（有几条"非推论、命中≥2"的实边）；
 *   ② 各候选阈值下，多少比例的符号节点算"有据"（判据太松 = 人人都算，太紧 = 没人是）；
 *   ③ 对着**模型里真实存在的前提链接**（`[前提]`/`[前提不]`）看：这些前提在 mention/activate/
 *      verified 三档下分别会成立多少 —— 这就是"翻默认会不会改行为"的直接数字。
 *
 * 用法：./build_premholdprobe [模型] [阈值扫描上限] */
#include "../agent.h"
#include "../drive.h"
#include "../model_io.h"
#include <stdio.h>

static int cmpsz(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 与 ai.h 里的判据**同源**：非推论、非 `[`-内部、命中 ≥ min_hits（可参数化，便于扫） */
static size_t solid_edges_n(const Token *t, unsigned min_hits) {
    if (!t) return 0;
    size_t n = 0;
    for (size_t i = 0; i < t->count; ++i) {
        const AssocEntry *e = &t->assocs[i];
        if (!e->main || e->hits < min_hits) continue;
        if (e->role == ASSOC_ROLE_DERIVED) continue;
        if (e->main->name && e->main->name[0] == '[') continue;
        n++;
    }
    return n;
}
static size_t solid_edges(const Token *t) { return solid_edges_n(t, AI_PREMISE_MIN_HITS); }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    AI *ai = ai_load_file(model);
    if (!ai) { printf("load fail\n"); return 1; }

    size_t cap = ai->pool->count + 8;
    size_t *v = (size_t *)malloc(cap * sizeof *v);
    if (!v) { ai_free(ai); return 1; }
    size_t n = 0, content = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage || !t->name) continue;
        if (t->name[0] == '[') continue;            /* 内核自有标记不算"内容词" */
        content++;
        v[n++] = solid_edges(t);
    }
    qsort(v, n, sizeof *v, cmpsz);
    printf("内容词 %zu 个（另有 %zu 个内核标记未计）\n", content, ai->pool->count - content);
    if (n) {
        printf("  有据度（「命中≥%d 的非推论实边」条数）分位："
               "p10 %zu / p50 %zu / p90 %zu / 最大 %zu\n",
               AI_PREMISE_MIN_HITS, v[n / 10], v[n / 2], v[(n * 9) / 10], v[n - 1]);
        size_t zero = 0;
        for (size_t i = 0; i < n; ++i) if (v[i] == 0) zero++;
        printf("  一条这样的边都没有的词：%zu（%.1f%%）\n", zero, 100.0 * (double)zero / (double)n);
    }
    printf("  阈值扫描（算「有据」的词占比）：\n");
    for (size_t need = 1; need <= 8; ++need) {
        size_t pass = 0;
        for (size_t i = 0; i < n; ++i) if (v[i] >= need) pass++;
        printf("    需要 ≥%zu 条：%5zu 个（%5.1f%%）%s\n", need, pass,
               100.0 * (double)pass / (double)(n ? n : 1),
               (need == AI_PREMISE_MIN_EDGES) ? "   ← 现在用的" : "");
    }

    printf("  命中门槛扫描（≥3 条边、但把「命中≥N」换掉）：\n");
    for (unsigned mh = 1; mh <= 4; ++mh) {
        size_t pass = 0;
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (!t || t->is_garbage || !t->name || t->name[0] == '[') continue;
            if (solid_edges_n(t, mh) >= AI_PREMISE_MIN_EDGES) pass++;
        }
        printf("    命中≥%u：%5zu 个（%5.1f%%）%s\n", mh, pass,
               100.0 * (double)pass / (double)(content ? content : 1),
               (mh == AI_PREMISE_MIN_HITS) ? "   ← 现在用的" : "");
    }

    /* 真实的前提链接：这些前提在三档下各会成立多少 */
    size_t links = 0, wf = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        for (size_t j = 0; j < t->count; ++j) {
            if (!edge_is_premise_link(&t->assocs[j])) continue;
            links++;
            if (ai_premise_wellfounded(ai, t)) wf++;
        }
    }
    printf("  模型里真实的前提链接：%zu 条；其中前提**有据**的：%zu 条（%.1f%%）\n",
           links, wf, links ? 100.0 * (double)wf / (double)links : 0.0);
    printf("  → verified 档比 activate 档会**多挡掉**这一部分（前提够得着但没据）\n");
    free(v);
    ai_free(ai);
    return 0;
}
