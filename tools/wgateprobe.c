/* tools/wgateprobe.c — 「写入分级闸门」量测台
 *
 * 目的（A.7）：闸门评分在真模型上**判别力窄** ——
 *   `imp = 0.25·生词 + 0.20·自模型信任 + 0.20·提及 + 0.20·信息量 + 0.075·惊讶 + 0.075·不累`
 * 里 trust / 惊讶 / 不累 是**整个 AI 一个值**、对所有候选边完全相同，只贡献常数偏移；
 * 真有方差的只有 novelty（三档）、mention（会饱和）、info。实测分布 0.587~0.725，
 * 阈值 0.65/0.70 只切 24%/4.2% —— "闸门"名存实亡。
 *
 * 这份量测台做三件事（**先量再定**，阈值不许拍）：
 *   ① 逐项报**均值/标准差/分位** —— 哪一项在真图上真有方差，一眼看得出；
 *   ② 量一批**候选新项**（边自己的 hits / 边龄 / 新近 / 租约 / 同对对立极性 /
 *      `[修正:…]` 改口痕迹 / 两端厚度），挑"方差够大 + 语义对"的那个；
 *   ③ 报**新评分的分布与按分位定的阈值**（阈值取分位，不拍）。
 *
 * 用法：./build_wgateprobe <模型> [语料（忽略，兼容旧调用）] [最多多少条（默认 20000）] */
#include "../agent.h"
#include "../token_lang.h"
#include "../model_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 一条关系边上的全部量 */
typedef struct {
    double nov, trust, ment, info, surp, awake;   /* 现值公式那六项 */
    double imp;                                   /* 现值评分 */
    double hits, ageb, aget, recb, rect;          /* 边的自身强度/年龄/新近 */
    double cont, rev, thin, lease, role;          /* 对立极性 / 改口痕迹 / 厚度 / 租约 / 角色 */
} Row;

enum { F_NOV, F_TRUST, F_MENT, F_INFO, F_SURP, F_AWAKE, F_IMP,
       F_HITS, F_AGEB, F_AGET, F_RECB, F_RECT, F_CONT, F_REV, F_THIN, F_LEASE, F_ROLE,
       F_N };

static double row_get(const Row *r, int f) {
    switch (f) {
    case F_NOV:   return r->nov;
    case F_TRUST: return r->trust;
    case F_MENT:  return r->ment;
    case F_INFO:  return r->info;
    case F_SURP:  return r->surp;
    case F_AWAKE: return r->awake;
    case F_IMP:   return r->imp;
    case F_HITS:  return r->hits;
    case F_AGEB:  return r->ageb;
    case F_AGET:  return r->aget;
    case F_RECB:  return r->recb;
    case F_RECT:  return r->rect;
    case F_CONT:  return r->cont;
    case F_REV:   return r->rev;
    case F_THIN:  return r->thin;
    case F_LEASE: return r->lease;
    case F_ROLE:  return r->role;
    default:      return 0.0;
    }
}

/* 打印 均值/标准差/五分位；同时把分位点回填（给"按分位定阈值"用） */
static void stats_print(const char *name, const Row *rows, size_t n, int f, double *q_out) {
    double *v = (double *)malloc(n * sizeof *v);
    if (!v) return;
    for (size_t i = 0; i < n; ++i) v[i] = row_get(&rows[i], f);
    qsort(v, n, sizeof *v, cmp_double);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += v[i];
    double mean = sum / (double)n, var = 0.0;
    for (size_t i = 0; i < n; ++i) var += (v[i] - mean) * (v[i] - mean);
    double sd = (n > 1) ? sqrt(var / (double)(n - 1)) : 0.0;
    if (q_out) {
        q_out[0] = v[0]; q_out[1] = v[n / 4]; q_out[2] = v[n / 2];
        q_out[3] = v[(n * 3) / 4]; q_out[4] = v[n - 1];
    }
    printf("  %-24s 均值 %.3f  标准差 %.3f  最小 %.3f  p25 %.3f  中位 %.3f  p75 %.3f  最大 %.3f\n",
           name, mean, sd, v[0], v[n / 4], v[n / 2], v[(n * 3) / 4], v[n - 1]);
    free(v);
}

static int marker_mentions(const char *marker, const char *a, const char *b) {
    if (!marker || !a || !b || !a[0] || !b[0]) return 0;
    return (strstr(marker, a) && strstr(marker, b)) ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    size_t limit = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 20000;
    if (limit == 0) limit = 20000;
    AI *ai = ai_load_file(model);
    if (!ai) { printf("load fail: %s\n", model); return 1; }

    /* 改口痕迹：`[修正:主语_不_关系_宾语]` 这类标记节点的名字里带两端名字就算提过这一对 */
    const size_t plen = strlen(MICRO_REVISE_TOKEN_PREFIX);
    const char **marks = (const char **)malloc(ai->pool->count * sizeof *marks);
    size_t n_mark = 0;
    if (marks) {
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t && !t->is_garbage && t->name &&
                strncmp(t->name, MICRO_REVISE_TOKEN_PREFIX, plen) == 0)
                marks[n_mark++] = t->name;
        }
    }
    printf("改口痕迹节点（%s…）：%zu 个\n", MICRO_REVISE_TOKEN_PREFIX, n_mark);

    Row *rows = (Row *)calloc(limit + 8, sizeof *rows);
    if (!rows) { free(marks); ai_free(ai); return 1; }
    size_t n = 0;
    uint64_t now = ai->pool->current_tick;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *a = ai->pool->tokens[i];
        if (!a || a->is_garbage || !a->name || a->name[0] == '[') continue;
        for (size_t j = 0; j < a->count; ++j) {
            const AssocEntry *e = &a->assocs[j];
            if (!e->main || !(e->rel || e->nrel)) continue;
            if (!e->main->name || e->main->name[0] == '[') continue;
            if (n >= limit) break;
            Row *r = &rows[n];
            const Token *b = e->main;
            size_t ea = a->count, eb = b->count;
            r->nov = (ea < 6 && eb < 6) ? 1.0 : ((ea + eb < 40) ? 0.6 : 0.4);
            r->info = 1.0 - ai_pair_jaccard(a, b);
            r->trust = (double)ai->inner.predict_accuracy;
            if (r->trust < 0.0) r->trust = 0.0;
            if (r->trust > 1.0) r->trust = 1.0;
            r->ment = (double)(token_hits_total(a) + token_hits_total(b)) / 200.0;
            if (r->ment > 1.0) r->ment = 1.0;
            r->surp = ai->inner.discomfort; if (r->surp > 1.0) r->surp = 1.0;
            r->awake = 1.0 - (double)ai->inner.fatigue; if (r->awake < 0.0) r->awake = 0.0;
            r->imp = ai_write_importance(ai, a, b);
            r->hits = (double)e->hits;
            r->ageb = (double)((now > e->birth_tick) ? (now - e->birth_tick) : 0);
            r->aget = (double)((now > e->last_touch_tick) ? (now - e->last_touch_tick) : 0);
            r->recb = ai_recency_weight(ai, (uint64_t)r->ageb);
            r->rect = ai_recency_weight(ai, (uint64_t)r->aget);
            r->cont = (e->rel && e->nrel) ? 1.0 : 0.0;
            r->rev = 0.0;
            for (size_t k = 0; k < n_mark; ++k)
                if (marker_mentions(marks[k], a->name, b->name)) { r->rev = 1.0; break; }
            r->thin = (double)((ea < eb ? ea : eb) > 40 ? 40 : (ea < eb ? ea : eb));
            r->lease = (double)ai_edge_lease(ai, e);
            r->role = (double)e->role;
            n++;
        }
    }
    printf("扫了全图的**关系边** %zu 条（真模型里就这么多）\n", n);
    if (n == 0) { free(rows); free(marks); ai_free(ai); return 0; }

    double q[5];
    printf("\n== 现值公式：逐项在真图上的分布（标准差小＝对判别没贡献）==\n");
    stats_print("生词 novelty", rows, n, F_NOV, NULL);
    stats_print("自模型信任 trust", rows, n, F_TRUST, NULL);
    stats_print("提及 mention", rows, n, F_MENT, NULL);
    stats_print("信息量 info", rows, n, F_INFO, NULL);
    stats_print("惊讶 surprise", rows, n, F_SURP, NULL);
    stats_print("不累 awake", rows, n, F_AWAKE, NULL);
    stats_print("【现值 imp】", rows, n, F_IMP, q);
    printf("  → 现值阈值 %.2f/%.2f 在这批边上切掉 ",
           AI_WGATE_KNOWLEDGE, AI_WGATE_LONGTERM);
    {
        size_t below = 0, above = 0;
        for (size_t i = 0; i < n; ++i) {
            if (rows[i].imp < AI_WGATE_KNOWLEDGE) below++;
            if (rows[i].imp >= AI_WGATE_LONGTERM) above++;
        }
        printf("%zu 条（%.1f%%）/ %zu 条（%.1f%%）\n",
               below, 100.0 * (double)below / (double)n, above, 100.0 * (double)above / (double)n);
    }

    printf("\n== 候选新项：在真图上的分布 ==\n");
    stats_print("边 hits（这条边被印证）", rows, n, F_HITS, NULL);
    stats_print("边龄 birth", rows, n, F_AGEB, NULL);
    stats_print("距上次碰 touch", rows, n, F_AGET, NULL);
    stats_print("新近核 recency(birth)", rows, n, F_RECB, NULL);
    stats_print("新近核 recency(touch)", rows, n, F_RECT, NULL);
    stats_print("同对对立极性 contested", rows, n, F_CONT, NULL);
    stats_print("改口痕迹 revised", rows, n, F_REV, NULL);
    stats_print("两端较薄那端 thin", rows, n, F_THIN, NULL);
    stats_print("租约 lease(0稳/1弱/2过)", rows, n, F_LEASE, NULL);
    stats_print("角色 role", rows, n, F_ROLE, NULL);

    /* ---- 候选证据项 support 的**真实实现**长什么样 ----
     * 分布要在**候选总体**上量：`tests/meas_wgate_cand.sh`（闸门只管候选，不管这批基准边）。
     * 这里只摆公式的形状 —— 以前探针里抄过一份"候选方案"，抄一份就一定会飘（§6.43 踩过）。 */
    printf("\n== 候选证据项 support = cred/85 − %.2f·(hops−1)（真实实现 ai_write_support）==\n",
           AI_WGATE_HOP_PENALTY);
    printf("  链底可信度 │   1 跳    2 跳    3 跳    4 跳    5 跳\n");
    {
        static const int creds[] = { 30, 50, 56, 70, 85 };
        for (size_t ci = 0; ci < sizeof creds / sizeof creds[0]; ++ci) {
            printf("  %3d 分      │", creds[ci]);
            for (int h = 1; h <= 5; ++h) {
                AiWriteCtx c;
                c.ev = DER_EV_RULE; c.hops = h; c.cred = creds[ci];
                printf(" %.3f ", ai_write_support(&c));
            }
            printf("\n");
        }
    }
    printf("  生效阈值：新公式（证据项开）%.2f/%.2f；老公式 %.2f/%.2f\n",
           AI_WGATE_EV_KNOWLEDGE, AI_WGATE_EV_LONGTERM, AI_WGATE_KNOWLEDGE, AI_WGATE_LONGTERM);

    free(rows);
    free(marks);
    ai_free(ai);
    return 0;
}
