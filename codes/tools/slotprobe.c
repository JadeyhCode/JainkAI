/* tools/slotprobe.c — 量「关联链里到底有没有槽位候选」
 *
 * 问题：结构化帧的槽位取值必须落在 frames.txt 手写的白名单里，名单外的词一律
 * 拒绝。可是关联链里其实记着「X 后面实际接过哪些词」（句尾模式 TAIL0/TAIL1/
 * TAIL2P）。如果这份统计足够干净，帧就能改成**从关联链取值**，而不是只认手写表。
 *
 * 这个探针就是先看看那份统计干不干净：
 *   ./build_slotprobe model.bin 喜欢
 * 会按 pattern 把 <词> 的出边分组打出来，跳过 [xxx] 标记与虚词。
 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
#include <math.h>

/* 虚词/功能词：当槽位取值没有意义，先粗暴挡掉 */
static const char *FUNC[] = {
    "的", "了", "是", "在", "和", "也", "都", "就", "很", "不", "吗", "呢",
    "吧", "啊", "我", "你", "他", "她", "它", "们", "这", "那", "有", "会",
    "要", "把", "被", "给", "对", "从", "到", "而", "但", "而", "因为",
    "所以", "如果", "什么", "怎么", "一个", "一", "个", "好", "地", "得",
    "着", "过", "来", "去", "上", "下", "里", "中", "个", "些", "没",
    NULL
};

static int is_func(const char *s) {
    for (int i = 0; FUNC[i]; ++i)
        if (strcmp(s, FUNC[i]) == 0) return 1;
    return 0;
}

static const char *pat_name(uint8_t p) {
    switch (p) {
        case ASSOC_PAT_TAIL0:  return "TAIL0(句尾)";
        case ASSOC_PAT_TAIL1:  return "TAIL1(后接1词)";
        case ASSOC_PAT_TAIL2P: return "TAIL2P(后接2+)";
        default:               return "NONE";
    }
}

/* 取 a→b 这条边（没有就返回 NULL） */
static AssocEntry *edge_of(Token *a, Token *b) {
    if (!a || !b) return NULL;
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].main == b) return &a->assocs[i];
    return NULL;
}

/* 成对模式：给「槽位前一个固定词 P」和「用户实际说的候选 C」，
 * 打出关联链里到底有没有证据。
 * 这是 Gate B 的核心问题：名单外的词能不能靠关联链放行。 */
static void pair_mode(AI *ai, const char *pname, const char *cname) {
    Token *P = pool_find(ai->pool, pname);
    Token *C = pool_find(ai->pool, cname);
    if (!P) { printf("  [%s] 不在图里\n", pname); return; }
    if (!C) { printf("  [%s] 不在图里（关联链无从谈起）\n", cname); return; }
    AssocEntry *e1 = edge_of(P, C);
    AssocEntry *e2 = edge_of(C, P);
    /* 候选的「实词性」：它自己有多少条出边（虚词也有，但少） */
    printf("  %s → %s: %s", pname, cname,
           e1 ? "有边" : "无边");
    if (e1) printf(" hits=%u pat=%s role=%u", e1->hits, pat_name(e1->pattern), e1->role);
    printf(" ｜ %s → %s: %s", cname, pname, e2 ? "有边" : "无边");
    if (e2) printf(" hits=%u pat=%s role=%u", e2->hits, pat_name(e2->pattern), e2->role);
    printf(" ｜ C 出边=%zu 虚词=%d 标记=%d\n", C->count, is_func(cname),
           (C->name && C->name[0] == '['));
}

/* 分布相似度：v 和 e 的出边邻居重叠（Dice×100）。
 * 不看它们彼此有没有边，只看「邻居像不像」——
 * 这是判断「支持」和「画画」是不是同一类槽位取值的候选证据。
 * 注意：符号节点的边里同一个 main 会有重复行，必须先按 main 去重，
 * 否则 Dice 会算出 >100 的分数（探针踩过这个坑）。 */
static int dice100(Token *a, Token *b) {
    if (!a || !b || a->count == 0 || b->count == 0) return 0;
    size_t na = 0, nb = 0, inter = 0;
    for (size_t i = 0; i < a->count; ++i) {
        Token *m = a->assocs[i].main;
        if (!m || (m->name && m->name[0] == '[')) continue;
        size_t k;
        for (k = 0; k < i; ++k) if (a->assocs[k].main == m) break;
        if (k < i) continue;                       /* 重复边，跳过 */
        na++;
        for (size_t j = 0; j < b->count; ++j)
            if (b->assocs[j].main == m) { inter++; break; }
    }
    for (size_t j = 0; j < b->count; ++j) {
        Token *m = b->assocs[j].main;
        if (!m || (m->name && m->name[0] == '[')) continue;
        size_t k;
        for (k = 0; k < j; ++k) if (b->assocs[k].main == m) break;
        if (k < j) continue;
        nb++;
    }
    size_t denom = na + nb;
    return denom ? (int)((2 * inter * 100) / denom) : 0;
}

/* 对每个候选，报告它和「已知好填充」里最像的那一个有多像。 */
static void cls_mode(AI *ai, const char *exlist, int argc, char **argv, int from) {
    Token *ex[FR_MAX_FILL];
    size_t ne = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", exlist);
    char *p = buf;
    while (*p && ne < FR_MAX_FILL) {
        char *s = p;
        while (*p && *p != ',' && *p != ' ') p++;
        char save = *p; *p = 0;
        if (*s) {
            Token *t = pool_find(ai->pool, s);
            if (t) ex[ne++] = t;
            else printf("  (示例 %s 不在图里)\n", s);
        }
        *p = save;
        if (*p == ',' || *p == ' ') p++;
    }
    printf("示例类 %zu 个\n", ne);
    for (int a = from; a < argc; ++a) {
        Token *c = pool_find(ai->pool, argv[a]);
        if (!c) { printf("  %-8s 不在图里\n", argv[a]); continue; }
        int best = -1; const char *bn = "—";
        for (size_t i = 0; i < ne; ++i) {
            int d = dice100(c, ex[i]);
            if (d > best) { best = d; bn = ex[i]->name; }
        }
        printf("  %-8s 与类最相似=%3d (最像 %s)  出边=%zu 虚词=%d\n",
               argv[a], best, bn, c->count, is_func(argv[a]));
    }
}

/* 全体分布：把「与示例类最相似」这个分数在全图内容词上算一遍，
 * 看阈值能不能真的分开。没有这张分布表，阈值就是拍脑袋。 */
static int cmp_pair(const void *A, const void *B) {
    const int *a = (const int *)A, *b = (const int *)B;
    return a[0] - b[0];
}

static void stat_mode(AI *ai, const char *exlist) {
    Token *ex[FR_MAX_FILL];
    size_t ne = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", exlist);
    char *p = buf;
    while (*p && ne < FR_MAX_FILL) {
        char *s = p;
        while (*p && *p != ',' && *p != ' ') p++;
        char save = *p; *p = 0;
        if (*s) { Token *t = pool_find(ai->pool, s); if (t) ex[ne++] = t; }
        *p = save;
        if (*p == ',' || *p == ' ') p++;
    }
    printf("示例类 %zu 个；下面统计全图内容词的「与类最相似」分数分布\n", ne);
    static int score[8192];
    static const char *nm[8192];
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count && n < 8192; ++i) {
        Token *c = ai->pool->tokens[i];
        if (!c->name || c->name[0] == '[' || c->count == 0) continue;
        int best = 0;
        for (size_t j = 0; j < ne; ++j) {
            int d = dice100(c, ex[j]);
            if (d > best) best = d;
        }
        score[n] = best; nm[n] = c->name; n++;
    }
    /* 分布表 */
    size_t bucket[11] = {0};
    for (size_t i = 0; i < n; ++i) {
        int b = score[i] / 10;
        if (b > 10) b = 10;
        bucket[b]++;
    }
    printf("分数桶: ");
    for (int i = 0; i <= 10; ++i)
        printf("%d-%d:%zu ", i * 10, i * 10 + 9, bucket[i]);
    printf("\n");
    /* 局部排序出前 20（用简单选择，n 不大） */
    printf("分数最高的 20 个内容词:\n");
    for (int k = 0; k < 20; ++k) {
        int bi = -1;
        for (size_t i = 0; i < n; ++i)
            if (score[i] >= 0 && (bi < 0 || score[i] > score[bi])) bi = (int)i;
        if (bi < 0) break;
        printf("   %-10s %d%s\n", nm[bi], score[bi], is_func(nm[bi]) ? "  (虚词)" : "");
        score[bi] = -1;
    }
    printf("(按某阈值 T 的通过数，就是帧槽位白名单能扩多少)\n");
    (void)cmp_pair;
}

/* 覆盖率：候选的出边**直接盖住**示例集里多少个词（百分比）。
 * 与 Dice 不同：Dice 比的是「邻居长得像不像」，覆盖率比的是
 * 「这个槽位认识的词，它是不是**逐个都认识**」。
 * 造出来的新词（现场把它接到全部示例词上）覆盖率是 100%。 */
static int cover100(const Token *cand, Token *const *ex, size_t ne) {
    if (!cand || ne == 0) return 0;
    size_t hit = 0;
    for (size_t i = 0; i < ne; ++i) {
        for (size_t j = 0; j < cand->count; ++j)
            if (cand->assocs[j].main == ex[i]) { hit++; break; }
    }
    return (int)((hit * 100) / ne);
}

/* 覆盖率的全图分布：判断这条信号会不会把普通名词也放进来 */
static void cover_mode(AI *ai, const char *exlist) {
    Token *ex[FR_MAX_FILL];
    size_t ne = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", exlist);
    char *p = buf;
    while (*p && ne < FR_MAX_FILL) {
        char *s = p;
        while (*p && *p != ',' && *p != ' ') p++;
        char save = *p; *p = 0;
        if (*s) { Token *t = pool_find(ai->pool, s); if (t) ex[ne++] = t; }
        *p = save;
        if (*p == ',' || *p == ' ') p++;
    }
    printf("示例集 %zu 个；覆盖率分布（全图内容词）\n", ne);
    static int cov[8192];
    static const char *nm[8192];
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count && n < 8192; ++i) {
        Token *c = ai->pool->tokens[i];
        if (!c->name || c->name[0] == '[') continue;
        cov[n] = cover100(c, ex, ne); nm[n] = c->name; n++;
    }
    size_t bucket[11] = {0};
    for (size_t i = 0; i < n; ++i) {
        int b = cov[i] / 10;
        if (b > 10) b = 10;
        bucket[b]++;
    }
    printf("覆盖桶: ");
    for (int i = 0; i <= 10; ++i) printf("%d-%d:%zu ", i * 10, i * 10 + 9, bucket[i]);
    printf("\n");
    printf("覆盖率最高的 16 个:\n");
    for (int k = 0; k < 16; ++k) {
        int bi = -1;
        for (size_t i = 0; i < n; ++i)
            if (cov[i] >= 0 && (bi < 0 || cov[i] > cov[bi])) bi = (int)i;
        if (bi < 0) break;
        printf("   %-10s %d%%%s\n", nm[bi], cov[bi], is_func(nm[bi]) ? "  (虚词)" : "");
        cov[bi] = -1;
    }
}

/* 中转（relay）信号：抽象节点到底能不能当「中转站」用。
 * 问三件事：
 *   ① 有多少个 [抽象N]，各挂几个成员；
 *   ② 有多少符号节点是「死胡同」—— 自己走出去没路（ai_can_continue_from 判 0）；
 *   ③ 这些死胡同里有多少**挂在抽象节点下**（有 parent）——
 *      也就是说「死路时经 parent 绕一步」能救回多少。 */
static void relay_mode(AI *ai) {
    size_t n_abs = 0, n_member = 0, max_member = 0;
    size_t n_dead = 0, n_dead_rescuable = 0, n_live = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->name) continue;
        int is_abs = (strncmp(t->name, "[抽象", 7) == 0);
        if (is_abs) {
            n_abs++;
            size_t m = 0;
            for (size_t j = 0; j < ai->pool->count; ++j) {
                Token *u = ai->pool->tokens[j];
                if (u->parent == t) m++;
            }
            if (m > max_member) max_member = m;
            n_member += m;
            continue;
        }
        if (t->name[0] == '[') continue;
        /* 有没有可用出边（照 ai_can_continue_from 的意思，简化版：
         * 跳过标记、跳过 hits==0 的残边） */
        int live = 0;
        for (size_t j = 0; j < t->count; ++j) {
            Token *m = t->assocs[j].main;
            if (!m || !m->name) continue;
            if (m->name[0] == '[' && !ai_is_reply_end(m) && !ai_is_dialog_split(m)) continue;
            if (t->assocs[j].hits == 0 && !ai_is_reply_end(m) && !ai_is_dialog_split(m)) continue;
            live = 1; break;
        }
        if (live) { n_live++; continue; }
        n_dead++;
        if (t->parent) n_dead_rescuable++;
    }
    printf("[抽象N] 个数=%zu  成员总数=%zu  单个最多成员=%zu\n",
           n_abs, n_member, max_member);
    printf("内容符号节点: 有出边=%zu  死胡同=%zu\n", n_live, n_dead);
    printf("死胡同里**挂在抽象节点下**（可经 parent 绕行）= %zu", n_dead_rescuable);
    if (n_dead) printf("（占死胡同 %.0f%%）", 100.0 * (double)n_dead_rescuable / (double)n_dead);
    printf("\n");
}

/* 「符号神经」体检：这个系统到底有多少是「神经」（分布式表示）的。
 * 问四件事：
 *   ① 边上有多少带类型信息（角色/话题/句尾模式）—— 符号侧的结构化程度；
 *   （§6.299 已删掉 ②③：hv/VSA 那套向量没有消费者，见 audit）
 *   ④ ai_analogy（emb 做 A:B::C:? 推理）有没有被调用过
 *      —— 这个只能在源码里数，这里给出它依赖的数据是否就绪。 */
static void neuro_mode(AI *ai) {
    size_t e_role = 0, e_topic = 0, e_pat = 0, e_all = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->name || t->name[0] == '[') continue;
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            e_all++;
            if (e->role) e_role++;
            if (e->topic) e_topic++;
            if (e->pattern) e_pat++;
        }
    }
    printf("① 内容符号节点的出边 %zu 条：带角色 %zu (%.1f%%)  带话题 %zu (%.1f%%)  带句尾模式 %zu (%.1f%%)\n",
           e_all, e_role, e_all ? 100.0 * (double)e_role / (double)e_all : 0.0,
           e_topic, e_all ? 100.0 * (double)e_topic / (double)e_all : 0.0,
           e_pat, e_all ? 100.0 * (double)e_pat / (double)e_all : 0.0);

    static const char *pairs[][3] = {
        {"猫", "狗", "同是宠物"},
        {"喜欢", "爱", "近义"},
        {"妈妈", "母亲", "同义"},
        {"雪", "雨", "同是天气"},
        {"怕", "恐惧", "近义"},
        {"猫", "量子", "无关（对照）"},
        {NULL, NULL, NULL}
    };
    /* ④ 学出来的分布式表示（emb）：这才该是「神经」那一半 */
    ai_embed_build(ai);
    size_t n_emb = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->name || t->name[0] == '[' || !t->emb) continue;
        for (size_t d = 0; d < EMB_DIM; ++d) if (t->emb[d]) { n_emb++; break; }
    }
    printf("④ emb（PPMI 随机索引，1024 维）非零的符号节点 = %zu\n", n_emb);
    printf("   语义相关词对：\n");
    for (size_t i = 0; pairs[i][0]; ++i) {
        Token *x = pool_find(ai->pool, pairs[i][0]);
        Token *y = pool_find(ai->pool, pairs[i][1]);
        if (!x || !y || !x->emb || !y->emb) continue;
        printf("     %-6s ↔ %-6s  %6.1f%%   (%s)\n",
               pairs[i][0], pairs[i][1],
               100.0 * (double)emb_cosine(x->emb, y->emb), pairs[i][2]);
    }
    {
        size_t n = ai->pool->count;
        double sum = 0, sumsq = 0; size_t cnt = 0;
        for (size_t i = 0; i + 7 < n; i += 7) {
            Token *x = ai->pool->tokens[i];
            Token *y = ai->pool->tokens[i + 7];
            if (!x->name || !y->name || x->name[0] == '[' || y->name[0] == '[') continue;
            if (!x->emb || !y->emb) continue;
            size_t nz = 0;
            for (size_t d = 0; d < EMB_DIM; ++d) if (x->emb[d] || y->emb[d]) { nz = 1; break; }
            if (!nz) continue;
            double c = (double)emb_cosine(x->emb, y->emb);
            sum += c; sumsq += c * c; cnt++;
        }
        if (cnt) {
            double mean = sum / (double)cnt;
            double var = sumsq / (double)cnt - mean * mean;
            printf("   随机词对（%zu 对）：均值 %.1f%%  标准差 %.1f%%\n",
                   cnt, 100.0 * mean, 100.0 * (var > 0 ? sqrt(var) : 0));
        }
    }

    /* ⑤ 关键诊断：前几个主成分吃掉多少方差。
     * 如果第一主成分就吃掉一大半，说明**所有词的向量都差不多** ——
     * 那不是算法问题，是语料问题：499 个词、±5 窗口、清一色的闲聊，
     * 几乎任意两个实词都共现过，共现矩阵本身接近秩 1。 */
    {
        size_t n = ai->pool->count;
        size_t used = 0;
        double *E = (double *)calloc(n * EMB_DIM, sizeof(double));
        double *v = (double *)calloc(EMB_DIM, sizeof(double));
        double *u = (double *)calloc(n, sizeof(double));
        if (E && v && u) {
            for (size_t i = 0; i < n; ++i) {
                Token *t = ai->pool->tokens[i];
                if (!t->emb) continue;
                int nz = 0;
                for (size_t d = 0; d < EMB_DIM; ++d) if (t->emb[d]) { nz = 1; break; }
                if (!nz) continue;
                for (size_t d = 0; d < EMB_DIM; ++d) E[i * EMB_DIM + d] = (double)t->emb[d];
                used++;
            }
            double total = 0;
            for (size_t k = 0; k < used * EMB_DIM; ++k) total += E[k] * E[k];
            printf("   主成分方差占比（%zu 个有向量的词）:", used);
            double inv = 1.0 / sqrt((double)EMB_DIM);
            for (int comp = 0; comp < 3; ++comp) {
                for (size_t d = 0; d < EMB_DIM; ++d) v[d] = inv;
                for (int it = 0; it < 12; ++it) {
                    for (size_t i = 0; i < n; ++i) {
                        double a2 = 0;
                        for (size_t d = 0; d < EMB_DIM; ++d) a2 += E[i * EMB_DIM + d] * v[d];
                        u[i] = a2;
                    }
                    double nu = 0;
                    for (size_t i = 0; i < n; ++i) nu += u[i] * u[i];
                    if (nu <= 0) break;
                    nu = sqrt(nu);
                    for (size_t i = 0; i < n; ++i) u[i] /= nu;
                    for (size_t d = 0; d < EMB_DIM; ++d) v[d] = 0;
                    for (size_t i = 0; i < n; ++i) {
                        if (u[i] == 0) continue;
                        for (size_t d = 0; d < EMB_DIM; ++d) v[d] += E[i * EMB_DIM + d] * u[i];
                    }
                    double nv = 0;
                    for (size_t d = 0; d < EMB_DIM; ++d) nv += v[d] * v[d];
                    if (nv <= 0) break;
                    nv = sqrt(nv);
                    for (size_t d = 0; d < EMB_DIM; ++d) v[d] /= nv;
                }
                /* 奇异值²：用收敛后的 v 重算 u = E v，取 |u|²（= σ²）。
                 * 之前拿归一化后的 u 去算，恒等于 1，占比永远是 0%（量错过一次）。 */
                double sv = 0;
                for (size_t i = 0; i < n; ++i) {
                    double a2 = 0;
                    for (size_t d = 0; d < EMB_DIM; ++d) a2 += E[i * EMB_DIM + d] * v[d];
                    sv += a2 * a2;
                }
                for (size_t i = 0; i < n; ++i) {
                    double p2 = 0;
                    for (size_t d = 0; d < EMB_DIM; ++d) p2 += E[i * EMB_DIM + d] * v[d];
                    for (size_t d = 0; d < EMB_DIM; ++d) E[i * EMB_DIM + d] -= p2 * v[d];
                }
                printf(" PC%d=%.1f%%", comp + 1, total > 0 ? 100.0 * sv / total : 0.0);
            }
            printf("\n");
        }
        free(E); free(v); free(u);
    }
}

/* emb 版类相似度：候选与示例集里最像的那个的余弦（百分数）。
 * 和 Dice 的区别：emb 是**学出来的稠密表示**，所以「危险」这种只有一两条边的
 * 稀疏词也能有分数，而 Dice 恒为 0。 */
static void embcls_mode(AI *ai, const char *exlist, int argc, char **argv, int from) {
    ai_embed_build(ai);
    Token *ex[FR_MAX_FILL];
    size_t ne = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", exlist);
    char *p = buf;
    while (*p && ne < FR_MAX_FILL) {
        char *s = p;
        while (*p && *p != ',' && *p != ' ') p++;
        char save = *p; *p = 0;
        if (*s) { Token *t = pool_find(ai->pool, s); if (t) ex[ne++] = t; }
        *p = save;
        if (*p == ',' || *p == ' ') p++;
    }
    printf("示例集 %zu 个（emb 余弦）\n", ne);
    for (int a = from; a < argc; ++a) {
        Token *c = pool_find(ai->pool, argv[a]);
        if (!c || !c->emb) { printf("  %-8s 不在图里\n", argv[a]); continue; }
        double best = 0; const char *bn = "—";
        for (size_t i = 0; i < ne; ++i) {
            if (!ex[i] || !ex[i]->emb) continue;
            double v = 100.0 * (double)emb_cosine(c->emb, ex[i]->emb);
            if (v > best) { best = v; bn = ex[i]->name; }
        }
        printf("  %-8s 与类最相似=%5.1f%% (最像 %s)  出边=%zu\n",
               argv[a], best, bn, c->count);
    }
}

/* emb 的**对比度**分数：max_e cos(cand,e) − cand 与随机词的余弦均值。
 *
 * 为什么要减：这份图和别的图一样，常见名词之间余弦普遍不低（彼此都共现过），
 * 所以「猫 对食物类 61%」看着高，其实猫对什么类都高。减掉它自己的基线之后，
 * 剩下的才是「这个类对**它**特别像」。这和 PPMI 之于原始频次是同一个道理。 */
static void embz_mode(AI *ai, const char *exlist, int argc, char **argv, int from) {
    ai_embed_build(ai);
    Token *ex[FR_MAX_FILL];
    size_t ne = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", exlist);
    char *p = buf;
    while (*p && ne < FR_MAX_FILL) {
        char *s = p;
        while (*p && *p != ',' && *p != ' ') p++;
        char save = *p; *p = 0;
        if (*s) { Token *t = pool_find(ai->pool, s); if (t) ex[ne++] = t; }
        *p = save;
        if (*p == ',' || *p == ' ') p++;
    }
    printf("示例集 %zu 个（emb 对比度 = 类内最像 − 与随机词均值）\n", ne);
    for (int a = from; a < argc; ++a) {
        Token *c = pool_find(ai->pool, argv[a]);
        if (!c || !c->emb) { printf("  %-8s 不在图里\n", argv[a]); continue; }
        double best = 0; const char *bn = "—";
        for (size_t i = 0; i < ne; ++i) {
            if (!ex[i] || !ex[i]->emb) continue;
            double v = 100.0 * (double)emb_cosine(c->emb, ex[i]->emb);
            if (v > best) { best = v; bn = ex[i]->name; }
        }
        double sum = 0; size_t cnt = 0;
        for (size_t i = 0; i < ai->pool->count; i += 7) {
            Token *t = ai->pool->tokens[i];
            if (!t->name || t->name[0] == '[' || !t->emb || t == c) continue;
            sum += 100.0 * (double)emb_cosine(c->emb, t->emb);
            cnt++;
        }
        double base = cnt ? sum / (double)cnt : 0;
        printf("  %-8s 类内最像=%5.1f%%  基线=%5.1f%%  对比度=%+6.1f (最像 %s)\n",
               argv[a], best, base, best - base, bn);
    }
}

/* 看一个词的 PPMI 上下文到底是些什么 —— 判断嵌入为什么分不开的直接办法。 */
static void ctx_mode(AI *ai, const char *name) {
    Token *t = pool_find(ai->pool, name);
    if (!t || !t->name) { printf("[%s] 不在图里\n", name); return; }
    size_t n = ai->pool->count;
    int64_t *marg = (int64_t *)calloc(n, sizeof(int64_t));
    for (size_t i = 0; i < n; ++i) {
        Token *x = ai->pool->tokens[i];
        if (!x->name || x->name[0] == '[') continue;
        for (size_t j = 0; j < x->count; ++j) {
            Token *m = x->assocs[j].main;
            if (!m || !m->name || m->name[0] == '[') continue;
            marg[i] += x->assocs[j].hits;
        }
    }
    int64_t tot = 0;
    for (size_t i = 0; i < n; ++i) tot += marg[i];
    printf("[%s] 出边 %zu 条，行和 %lld（占总权重 %.3f%%）\n",
           name, t->count, (long long)marg[t->id],
           tot ? 100.0 * (double)marg[t->id] / (double)tot : 0.0);
    /* 取 PPMI 最高的 8 个上下文 */
    Token *top[8] = {0}; double sc[8] = {0};
    for (size_t j = 0; j < t->count; ++j) {
        Token *m = t->assocs[j].main;
        if (!m || !m->name || m->name[0] == '[') continue;
        uint32_t h = t->assocs[j].hits;
        if (!h || marg[t->id] == 0 || marg[m->id] == 0) continue;
        double pmi = log(((double)h / (double)marg[t->id]) /
                         ((double)marg[m->id] / (double)tot));
        if (pmi <= 0) continue;
        for (int k = 0; k < 8; ++k) {
            if (!top[k] || pmi > sc[k]) {
                for (int q = 7; q > k; --q) { top[q] = top[q-1]; sc[q] = sc[q-1]; }
                top[k] = m; sc[k] = pmi;
                break;
            }
        }
    }
    printf("  PPMI 最高的上下文:");
    for (int k = 0; k < 8 && top[k]; ++k)
        printf(" %s(%.2f)", top[k]->name, sc[k]);
    printf("\n");
    /* 它的出边里，落在「高频上下文」上的权重占多少 */
    free(marg);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("用法:\n  %s model.bin dump   <词>...            按句尾模式列出边\n"
               "  %s model.bin pair   <P> <C>...         查 P→C 的关联链证据\n"
               "  %s model.bin cls    <示例,示例> <候选>... 查候选与示例类的分布相似度\n"
               "  %s model.bin stat   <示例,示例>        全图分数分布\n"
               "  %s model.bin cover  <示例,示例>        全图覆盖率分布\n"
               "  %s model.bin relay                    抽象节点/死胡同/可绕行统计\n"
               "  %s model.bin neuro                    「符号神经」体检：结构化程度 + VSA/emb\n"
               "  %s model.bin emb    <示例,示例> <候选>... emb 版类相似度（稀疏词也有分）\n",
               argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    AI *ai = ai_load_file(argv[1]);
    if (!ai) { printf("加载失败\n"); return 1; }
    if (strcmp(argv[2], "neuro") == 0) { neuro_mode(ai); ai_free(ai); return 0; }
    if (strcmp(argv[2], "ctx") == 0) {
        if (argc < 4) { printf("ctx 需要 <词>...\n"); ai_free(ai); return 1; }
        for (int a = 3; a < argc; ++a) ctx_mode(ai, argv[a]);
        ai_free(ai); return 0;
    }
    if (strcmp(argv[2], "embz") == 0) {
        if (argc < 5) { printf("embz 需要 <示例,示例> <候选>\n"); ai_free(ai); return 1; }
        embz_mode(ai, argv[3], argc, argv, 4); ai_free(ai); return 0;
    }
    if (strcmp(argv[2], "emb") == 0) {
        if (argc < 5) { printf("emb 需要 <示例,示例> <候选>\n"); ai_free(ai); return 1; }
        embcls_mode(ai, argv[3], argc, argv, 4); ai_free(ai); return 0;
    }
    if (strcmp(argv[2], "relay") == 0) { relay_mode(ai); ai_free(ai); return 0; }
    if (strcmp(argv[2], "cover") == 0) { cover_mode(ai, argv[3]); ai_free(ai); return 0; }
    if (strcmp(argv[2], "stat") == 0) { stat_mode(ai, argv[3]); ai_free(ai); return 0; }
    if (strcmp(argv[2], "cls") == 0) {
        if (argc < 5) { printf("cls 需要 <示例,示例> <候选>\n"); ai_free(ai); return 1; }
        cls_mode(ai, argv[3], argc, argv, 4);
        ai_free(ai);
        return 0;
    }
    if (strcmp(argv[2], "pair") == 0) {
        if (argc < 5) { printf("pair 需要 <P> <C>\n"); ai_free(ai); return 1; }
        printf("关联链证据：P=%s 对候选 C\n", argv[3]);
        for (int a = 4; a < argc; ++a) pair_mode(ai, argv[3], argv[a]);
        ai_free(ai);
        return 0;
    }
    for (int a = 3; a < argc; ++a) {
        Token *t = pool_find(ai->pool, argv[a]);
        if (!t) { printf("\n[%s] 不在图里\n", argv[a]); continue; }
        printf("\n[%s] 出边 %zu 条，按句尾模式分组（跳过标记/虚词）:\n",
               argv[a], t->count);
        for (int p = 0; p <= 3; ++p) {
            size_t shown = 0;
            printf("  %s:", pat_name((uint8_t)p));
            for (size_t i = 0; i < t->count && shown < 14; ++i) {
                Token *m = t->assocs[i].main;
                if (!m || !m->name || m->name[0] == '[') continue;
                if (t->assocs[i].pattern != p) continue;
                if (is_func(m->name)) continue;
                printf(" %s(%u)", m->name, t->assocs[i].hits);
                shown++;
            }
            if (shown == 0) printf(" —");
            printf("\n");
        }
    }
    ai_free(ai);
    return 0;
}
