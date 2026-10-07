/* main.c — 入口
 *
 * 语料：原语格式，按空白切分，交给 token_lang.h 的 token_run_names 执行。
 *       [原语:对象] / [原语:连接] 等由解释器执行，不进入图作为内容节点。
 * 模型：model.bin（model_io.h）—— 完整 AI 快照，存盘 / 读盘能力。
 *
 * 用法：见 ::help（调用方式只有一张表 USAGE[]，别在这儿再抄一份；
 *       命令行参数、默认值都在那张表里，抄第二份就是让文档漂移）。
 */
#include "agent.h"
#include "token_lang.h"
#include "model_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define DEFAULT_MODEL "model.bin"

/* 内存态指纹：把整张图滚成一个 FNV 哈希，用来验证 model.bin 存取是否无损。
 * 覆盖 tick、每个 Token 的元数据、每条边的全部字段与端点 id。 */
static uint64_t state_fingerprint(const AI *ai) {
    const TokenPool *p = ai->pool;
    uint64_t h = 1469598103934665603ULL;
    #define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ULL; } while (0)
    MIX(p->current_tick);
    MIX(p->count);
    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        MIX(t->id);
        if (t->name) { for (const char *s = t->name; *s; ++s) MIX((unsigned char)*s); }
        MIX(t->misses); MIX(t->recent_misses);
        MIX(t->is_category); MIX(t->is_garbage); MIX(t->mem_state);
        MIX(t->birth_tick); MIX(t->last_touch_tick);
        MIX(t->parent ? t->parent->id : TOKEN_ID_NONE);
        MIX(t->next_in_time ? t->next_in_time->id : TOKEN_ID_NONE);
        MIX(t->perspective_of ? t->perspective_of->id : TOKEN_ID_NONE);
        MIX(t->count);
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            MIX(e->n_subs); MIX(e->n_not);
            for (uint8_t k = 0; k < e->n_subs; ++k) MIX(e->subs[k] ? e->subs[k]->id : TOKEN_ID_NONE);
            for (uint8_t k = 0; k < e->n_not;  ++k) MIX(e->subs_not[k] ? e->subs_not[k]->id : TOKEN_ID_NONE);
            MIX(e->main ? e->main->id : TOKEN_ID_NONE);
            MIX(e->action ? e->action->id : TOKEN_ID_NONE);
            MIX(e->hits); MIX(e->seq); MIX(e->mem_state);
            MIX(e->birth_tick); MIX(e->last_touch_tick);
        }
    }
    #undef MIX
    return h;
}

/* 分维度指纹：定位「加载后哪里不一样」时用 */
static void fingerprint_dims(const AI *ai) {
    const TokenPool *p = ai->pool;
    uint64_t hm = 1469598103934665603ULL, he = hm, hp = hm, hhv = hm;
    #define MIXH(H, v) do { (H) ^= (uint64_t)(v); (H) *= 1099511628211ULL; } while (0)
    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        MIXH(hm, t->id); MIXH(hm, t->misses); MIXH(hm, t->recent_misses);
        MIXH(hm, t->is_category); MIXH(hm, t->is_garbage); MIXH(hm, t->mem_state);
        MIXH(hm, t->birth_tick); MIXH(hm, t->last_touch_tick);
        MIXH(hm, t->count); MIXH(hm, t->cap); MIXH(hm, t->next_seq);
        if (t->name) for (const char *s = t->name; *s; ++s) MIXH(hm, (unsigned char)*s);
        MIXH(hp, t->parent ? t->parent->id : TOKEN_ID_NONE);
        MIXH(hp, t->next_in_time ? t->next_in_time->id : TOKEN_ID_NONE);
        MIXH(hp, t->perspective_of ? t->perspective_of->id : TOKEN_ID_NONE);
        if (t->hv) for (size_t k = 0; k < VSA_DIM; ++k) MIXH(hhv, (uint8_t)t->hv[k]);
        if (t->hv_prev) for (size_t k = 0; k < VSA_DIM; ++k) MIXH(hhv, (uint8_t)t->hv_prev[k]);
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            MIXH(he, e->n_subs); MIXH(he, e->n_not);
            for (uint8_t k = 0; k < e->n_subs; ++k) MIXH(he, e->subs[k] ? e->subs[k]->id : TOKEN_ID_NONE);
            for (uint8_t k = 0; k < e->n_not;  ++k) MIXH(he, e->subs_not[k] ? e->subs_not[k]->id : TOKEN_ID_NONE);
            MIXH(he, e->main ? e->main->id : TOKEN_ID_NONE);
            MIXH(he, e->action ? e->action->id : TOKEN_ID_NONE);
            MIXH(he, e->hits); MIXH(he, e->seq); MIXH(he, e->mem_state);
            MIXH(he, e->birth_tick); MIXH(he, e->last_touch_tick);
        }
    }
    #undef MIXH
    printf("[指纹拆解] tick=%llu meta=%016llx edge=%016llx ptr=%016llx vox=%016llx\n",
           (unsigned long long)p->current_tick,
           (unsigned long long)hm, (unsigned long long)he,
           (unsigned long long)hp, (unsigned long long)hhv);
}

/* 逐 Token / 逐边指纹，用来 diff 出「到底哪一条不一样」 */
static void fingerprint_all(const AI *ai) {
    const TokenPool *p = ai->pool;
    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        uint64_t hm = 1469598103934665603ULL, he = hm;
        #define MIXH(H, v) do { (H) ^= (uint64_t)(v); (H) *= 1099511628211ULL; } while (0)
        MIXH(hm, t->id); MIXH(hm, t->misses); MIXH(hm, t->recent_misses);
        MIXH(hm, t->is_category); MIXH(hm, t->is_garbage); MIXH(hm, t->mem_state);
        MIXH(hm, t->birth_tick); MIXH(hm, t->last_touch_tick);
        MIXH(hm, t->count); MIXH(hm, t->cap); MIXH(hm, t->next_seq);
        if (t->name) for (const char *s = t->name; *s; ++s) MIXH(hm, (unsigned char)*s);
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            MIXH(he, e->n_subs); MIXH(he, e->n_not);
            for (uint8_t k = 0; k < e->n_subs; ++k) MIXH(he, e->subs[k] ? e->subs[k]->id : TOKEN_ID_NONE);
            for (uint8_t k = 0; k < e->n_not;  ++k) MIXH(he, e->subs_not[k] ? e->subs_not[k]->id : TOKEN_ID_NONE);
            MIXH(he, e->main ? e->main->id : TOKEN_ID_NONE);
            MIXH(he, e->action ? e->action->id : TOKEN_ID_NONE);
            MIXH(he, e->hits); MIXH(he, e->seq); MIXH(he, e->mem_state);
            MIXH(he, e->birth_tick); MIXH(he, e->last_touch_tick);
        }
        #undef MIXH
        printf("T%zu\t%016llx\t%016llx\t%u\t%u\t%s\n", i,
               (unsigned long long)hm, (unsigned long long)he,
               (unsigned)t->cap, (unsigned)t->count, t->name ? t->name : "(无名)");
    }
}

/* 数全池的边（只读） */
static size_t ai_stats_edges(const AI *ai) {
    size_t na = 0;
    for (size_t i = 0; i < ai->pool->count; ++i)
        na += ai->pool->tokens[i]->count;
    return na;
}

/* 只读查找：按名字找 Token，不改图 */
static const Token *find_token_const(const TokenPool *p, const char *name) {
    if (!p || !name) return NULL;
    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        if (!t->is_garbage && t->name && strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

/* ============================ 原语层 → Token 行为 ============================
 *
 * 原语栈机（token_lang.h）不认识 AI、也不许直接改图；它只把「行为编号 + 操作数」
 * 交给宿主。这里就是**唯一**的宿主实现：编号 → 行为名走 `TOKEN_OPS[]`
 * （行为名只写在 micro.h 那张表里一处），再走 `token_op_run` → 安全内核。
 * 编号与行为的对应由 tools/optest.c 钉着（改了 TOKEN_OPS 的顺序那里当场变红）。 */
static int interp_op_host(void *user, InterpOpKind op, char *const *args, size_t n) {
    AI *ai = (AI *)user;
    if (!ai || (int)op < 0 || op >= IPOP_N) return -1;
    const char *name = TOKEN_OPS[op].name;
    if (!name) return -1;
    char note[240];
    int got = token_op_run(ai, name, args, n, note, sizeof note);
    if (got < 0) printf("[原语 %s 没跑成] %s\n", name, note);
    else         printf("[原语 %s] %s\n", name, note);
    return got;
}

/* 读一行原语，把它当作程序执行。
 * 例：[原语:对象] 猫 [原语:对象] 动物 [原语:连接]
 * 执行后：图里多一条 猫 → 动物 的直接边，没有中间原语节点。
 *
 * 整行交给 ai_tokenize：它把 [原语:…] 当整体、正文按「池里认识的词」最长匹配。
 * 这样语料里的词和用户输入切出来的词**必然是同一个 Token**，不会错位。 */
static void learn_primitive_line(AI *ai, const char *line) {
    if (!ai || !line) return;

    size_t n = 0;
    char **words = ai_tokenize(ai, line, &n);
    if (!words || n == 0) { ai_free_words(words, n); return; }

    const char **names = (const char **)malloc(n * sizeof *names);
    if (!names) { ai_free_words(words, n); return; }
    size_t m = 0;
    for (size_t i = 0; i < n; ++i)
        if (words[i][0]) names[m++] = words[i];

    if (m > 0) {
        /* 两件事都要做：
         *  ① token_observe_names —— 把整行的 n-gram 序列（含 [对话:结束] 这类标记）
         *     记成边。解释器只会「执行」原语，不会记录它，缺了这步收句标记就没有入边。
         *  ② token_run_names —— 让原语真正生效（建 猫→动物 这种直接边）。 */
        token_observe_names(ai->pool, names, m, 5);   /* 窗口 5：让「回复内部词序」有更多可学/可打角色的转移 */
        token_run_names_ctx(ai->pool, names, m, interp_op_host, ai);
        ai->n_learned += m;
        ai->pool->current_tick++;       /* 让 birth_tick 随语料推进 */

        /* 语料里的分隔符就是「输入 / 回复」的边界：把 尾词→分割 和 分割→回复首词
         * 这两条主干边顶重，让它成为从输入尾词出去时最可能被选中的一条。 */
        for (size_t i = 0; i < m; ++i) {
            if (strcmp(names[i], AI_DIALOG_SPLIT_NAME) != 0) continue;
            Token *tail = (i > 0) ? pool_token_intern(ai->pool, names[i - 1]) : NULL;
            Token *head = (i + 1 < m) ? pool_token_intern(ai->pool, names[i + 1]) : NULL;
            ai_boost_reply_bridge(ai, tail, head);
        }
    }
    free(names);
    ai_free_words(words, n);
}

/* 回复槽强化 —— 让生成的句子真的沿着学过的回复走。
 *
 * 语料里 `[原语:对象] A [原语:连接] [原语:对象] B [原语:连接]`：A 段是「输入」、
 * B 段是「回复」。只靠共现学出来的边会把「回复内部的词序」和随便什么共现同等对待，
 * 游走很快漂到「我 / 喜欢 / 不喜欢」这类高频词上（表现为答非所问）。
 * 这里把 B 段内部每一对相邻词多顶 48 次；普通共现边的 hits 是个位数，
 * 于是从回复首词出发时，「走学过的回复」的概率远高于漂移。 */
#define REPLY_SLOT_BOOST 12

/* 解析 "[话题:N]" 行。就地做，不引额外函数 —— 之前用 strncmp 的版本在
 * GCC 下把行首字面量优化成了别的东西，判定始终失败（本轮踩过的坑）。 */
static void reinforce_reply_slots(AI *ai, const char *path, size_t abstraction_interval) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    size_t done = 0;
    uint8_t cur_topic = ASSOC_TOPIC_NONE;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        if (len && line[len-1] == '\n') line[len-1] = 0;
        if (!line[0]) continue;

        {   /* 显式话题标注：之后的行都属于这个话题，直到下一个标注 */
            if (line[0] == '[' &&
                strncmp(line + 1, "\xe8\xaf\x9d\xe9\xa2\x98:", 7) == 0) {
                char *endp = NULL;
                long v = strtol(line + 8, &endp, 10);
                if (endp && *endp == ']' && v >= 0 && v <= 255) {
                    cur_topic = (uint8_t)v;
                    continue;
                }
            }
        }

        size_t n = 0;
        char **words = ai_tokenize(ai, line, &n);
        if (!words || n == 0) { ai_free_words(words, n); continue; }

        /* 按「段」收集 Token，再给段内所有 n-gram 转移打角色标签。
         * 段 = 紧跟 [原语:对象] 之后的内容词；遇到任何其他 [原语:*] 收尾。
         * 窗口和 token_observe_names 完全一致，
         * 保证「观察到的边」和「打上角色的边」是同一批。 */
        Token *seg[256];
        size_t seg_n = 0;
        int in_seg = 0;
        Token *prev_tail = NULL;    /* 上一段的最后一个内容词 */
        Token *prev_seg[AI_MAX_SENT];
        size_t prev_seg_n = 0;      /* 上一段的完整内容词序列（整句检索用） */
        int prev_was_obj = 0;       /* 上一段是不是 [原语:对象] 段 */
        int seg_is_obj = 0;         /* 当前这一段是不是 [原语:对象] 段 */

        for (size_t i = 0; i <= n; ++i) {
            int at_end = (i == n);
            int is_marker = (!at_end && words[i][0] == '[');

            if (at_end || is_marker) {
                /* 这一段是不是被 [对话:结束] 正常收尾？
                 * 必须在冲刷之前判定 —— 冲刷发生在处理这个标记之前，
                 * 等到底下再置位就已经来不及了。 */
                /* 把结尾的 [对话:结束] 摘掉：它不参与句尾模式的计数 */
                size_t plen = seg_n;
                while (plen > 0 && ai_is_reply_end(seg[plen - 1])) plen--;

                if (in_seg && plen > 0) {
                    /* ① 段内转移：窗口和 token_observe_names 一致。
                     *    每条边同时带上「离句尾还有几个词」的句尾模式，
                     *    生成时据此知道该收尾了。 */
                    for (size_t a = 0; a + 1 < plen; ++a) {
                        size_t rest = plen - 1 - a;
                        uint8_t pat = (rest == 0) ? ASSOC_PAT_TAIL0
                                    : (rest == 1) ? ASSOC_PAT_TAIL1
                                                  : ASSOC_PAT_TAIL2P;
                        size_t e2 = a + 5;
                        if (e2 > plen) e2 = plen;
                        for (size_t b = a + 1; b < e2; ++b)
                            if (seg[a] != seg[b])
                                token_tag_full(seg[a], seg[b], ASSOC_ROLE_REPLY,
                                               cur_topic, pat, REPLY_SLOT_BOOST);
                    }
                    /* ② 跨段主干：输入尾词 → 回复首词。
                     *    这是「接话」的入口 —— 从输入尾词出发时角色寄存器还是 NONE，
                     *    只有这条边也打成回复角色，游走才会一步跨进回复段，
                     *    而不是被语料别处的高频共现带走。 */
                    if (prev_tail && prev_tail != seg[0])
                        token_tag_role_topic(prev_tail, seg[0], ASSOC_ROLE_REPLY,
                                             cur_topic, REPLY_SLOT_BOOST);
                }
                {   /* 记下这一段的尾词（跳过结束标记），供下一段做跨段主干 */
                    Token *last = NULL;
                    for (size_t a = 0; a < plen; ++a)
                        if (!ai_is_reply_end(seg[a])) last = seg[a];
                    if (last) prev_tail = last;
                }
                /* 整句对：(上一段内容词, 这一段内容词)。
                 * 语料是「输入段 [对话:结束] 回复段」的线性结构，
                 * 所以相邻两段正好构成「输入 → 回复」。 */
                int cur_is_obj = seg_is_obj;
                /* 只把「两头都被 [对话:结束] 正常收尾的对象段」配成对。
                 * seed_full 里大量 [原语:对象] … [原语:对象] … [原语:分割] 的
                 * 片段不是一问一答，配进去会污染检索库（实测会检索出
                 * 「你好 → 朋友」这种边片段）。 */
                /* 只用「被显式话题标注过的块」里的相邻段做检索库。
                 * 这比纠缠「段有没有被 [对话:结束] 收尾」的布尔标记可靠得多：
                 * 话题块就是生成器里写死的「一问一答」，边界天然正确；
                 * seed_full / generated 没有话题标注，直接不参与。 */
                if (plen > 0 && prev_seg_n > 0 && prev_was_obj && cur_is_obj &&
                    cur_topic != ASSOC_TOPIC_NONE) {
                    size_t cn = plen > AI_MAX_SENT ? AI_MAX_SENT : plen;
                    ai_sent_add(ai, prev_seg, prev_seg_n, seg, cn, cur_topic);
                }
                if (getenv("PAIRDBG")) fprintf(stderr, "FLUSH plen=%zu prev_n=%zu prevobj=%d curobj=%d pairs=%zu\n", plen, prev_seg_n, prev_was_obj, cur_is_obj, ai->pairs_n);
                /* 只有 [原语:对象] 段才是「一句完整的话」；其它段（[原语:连接]
                 * 这类）会把链断开，免得把不相关的片段配成对。 */
                if (plen > 0 && cur_is_obj) {
                    size_t cn = plen > AI_MAX_SENT ? AI_MAX_SENT : plen;
                    memcpy(prev_seg, seg, cn * sizeof *prev_seg);
                    prev_seg_n = cn;
                    prev_was_obj = 1;
                } else if (plen > 0) {
                    prev_seg_n = 0;
                    prev_was_obj = 0;
                }
                seg_n = 0;
                if (at_end) break;

                if (strcmp(words[i], "[原语:对象]") == 0) {
                    in_seg = 1;
                    seg_is_obj = 1;
                } else if (strcmp(words[i], AI_REPLY_END_NAME) == 0) {
                    /* 结束标记要作为段的最后一个位置参与打标，
                     * 这样「最后一词 → 结束标记」也有回复角色（收句靠它）。 */
                    if (in_seg) {
                        Token *end = pool_token_intern(ai->pool, words[i]);
                        if (end && seg_n < 256) seg[seg_n++] = end;
                    }
                    in_seg = 0;
                    seg_is_obj = 0;
                } else {
                    in_seg = 0;
                    seg_is_obj = 0;
                }
                continue;
            }

            Token *t = pool_token_intern(ai->pool, words[i]);
            if (!t) continue;
            /* 话题投票表：话题块里出现过的词都记下，生成时用来给输入投票选话题 */
            if (cur_topic != ASSOC_TOPIC_NONE) ai_topic_vote(ai, cur_topic, t);
            if (!in_seg) continue;
            if (seg_n < 256) seg[seg_n++] = t;
        }
        ai_free_words(words, n);
        (void)done; (void)abstraction_interval;
    }
    fclose(f);

}

/* 把抽象形成跑到不动点。
 * ai_form_abstractions 每次最多建 3 个类别，所以「调用几次」直接决定最终抽象数
 * （实测同一份语料 55 vs 67）。训练收尾和读盘收尾都跑这个，两条路径才会落到
 * 同一个不动点，加载后的行为才与原地训练一致。 */
static void stabilize_abstractions(AI *ai, size_t max_rounds) {
    if (!ai) return;
    size_t prev = ai->n_abstractions;
    for (size_t i = 0; i < max_rounds; ++i) {
        ai_form_abstractions(ai);
        if (ai->n_abstractions == prev) return;
        prev = ai->n_abstractions;
    }
}

/* 用整份语料训练。返回成功读入的行数。 */
static size_t train_on_corpus(AI *ai, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return 0; }
    char line[4096];
    size_t n = 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        if (len && line[len-1] == '\n') line[len-1] = 0;
        if (line[0]) { learn_primitive_line(ai, line); n++; }
    }
    fclose(f);
    reinforce_reply_slots(ai, path, 2000);
    rel_names_intern(ai);               /* 关系词先进词池 */
    relations_load(ai, "relations.txt"); /* 静态关系知识（对话里学的那些在模型里） */
    rel_excl_build(ai);                 /* 「独占」由数据量出来 */
    frames_load(ai, "frames.txt");
    stabilize_abstractions(ai, 2000);   /* 最后一个会造词/加边的步骤 */
    /* ⚠ 三个派生层（emb / 槽类 / idf）必须排在**所有改图的步骤之后**，
     *   而且与 model_io.h 的装载收尾顺序完全一致 —— 它们都是「从最终的图现算」。
     *   早算一步 = 算在旧的图上：存盘存的是最终的图，读盘时又按最终的图重算，
     *   两边的派生层就不一样了。
     *   ① idf 还额外按当时的 pool->count 分配数组，早算会让后造的词越界读堆内存
     *      （拿到地址相关的垃圾浮点 → 联想链都不一样，实测差 50 个词）；
     *   ② emb 早算的症状更隐蔽：词池逐字节相同，但 810 个 Token 的 emb 不同。 */
    ai_embed_build(ai);           /* 分布式表示：从最终的图现算一遍 */
    ai_slots_build(ai);
    ai_idf_rebuild(ai);
    printf("[预训练: %zu 行]\n", n);
    return n;
}

/* 批量训练：语料一次性灌完，并按行数周期内联做「巩固 + 抽象 + 睡眠回放」。
 * 原来这三件事挂在交互循环的「空闲睡眠」上，纯烧语料时永远不触发，
 * 训练出来的图就是很稀疏的初始态。这里按语料行确定性推进。 */
static void train_corpus_batch(AI *ai, const char *path, size_t interval) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return; }
    if (interval == 0) interval = 200;

    char line[4096];
    size_t n = 0, next_report = 1;
    for (;;) {
        if (!fgets(line, sizeof line, f)) break;
        size_t len = strlen(line);
        if (len && line[len-1] == '\n') line[len-1] = 0;
        if (!line[0]) continue;
        learn_primitive_line(ai, line);
        n++;

        if (n % interval) continue;
        ai->turns = n;

        /* 1 启发式巩固：按 hits 重排每个 Token 的边，再整体衰减 */
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t->is_garbage || t->count == 0) continue;
            token_consolidate(t);
            token_decay_hits(t, ai->decay_factor);
        }
        /* 3 睡眠回放：把这一段轨迹按 0.9/0.1/深度 6 再扩散一遍 */
        size_t k = ai->trace_n < 32 ? ai->trace_n : 32;
        for (size_t i = 0; i < k; ++i) {
            Token *t = ai->trace[i];
            if (!t || t->is_garbage) continue;
            ActivateCtx act;
            activate_ctx_init(&act, 0.9f, 0.1f, 6);
            token_activate_from(t, NULL, 0, 0.9f, &act);
            activate_ctx_free(&act);
        }
        ai->n_sleeps++;
        ai->inner.fatigue = 0.0f;
        ai->trace_n = 0;
        ai->sleep_target = 0;
        ai->sleep_cursor = 0;
        ai->sleep_in_abstraction = 0;
        /* 4 遗忘 */
        if (ai->gc.gc_interval && (n % (interval * ai->gc.gc_interval) == 0))
            ai_gc(ai);

        while (n >= next_report * 5000) {
            fprintf(stderr, "  ... %zu 行  Token=%zu 边=%zu 抽象=%zu 睡眠=%zu\n",
                    n, ai->pool->count, ai_stats_edges(ai),
                    ai->n_abstractions, ai->n_sleeps);
            next_report++;
        }
    }
    fclose(f);
    reinforce_reply_slots(ai, path, 2000);
    rel_names_intern(ai);               /* 关系词先进词池 */
    relations_load(ai, "relations.txt"); /* 静态关系知识（对话里学的那些在模型里） */
    rel_excl_build(ai);                 /* 「独占」由数据量出来 */
    frames_load(ai, "frames.txt");
    stabilize_abstractions(ai, 2000);   /* 最后一个会造词/加边的步骤 */
    /* ⚠ 三个派生层（emb / 槽类 / idf）必须排在**所有改图的步骤之后**，
     *   而且与 model_io.h 的装载收尾顺序完全一致 —— 它们都是「从最终的图现算」。
     *   早算一步 = 算在旧的图上：存盘存的是最终的图，读盘时又按最终的图重算，
     *   两边的派生层就不一样了。
     *   ① idf 还额外按当时的 pool->count 分配数组，早算会让后造的词越界读堆内存
     *      （拿到地址相关的垃圾浮点 → 联想链都不一样，实测差 50 个词）；
     *   ② emb 早算的症状更隐蔽：词池逐字节相同，但 810 个 Token 的 emb 不同。 */
    ai_embed_build(ai);           /* 分布式表示：从最终的图现算一遍 */
    ai_slots_build(ai);
    ai_idf_rebuild(ai);
    printf("[批量训练: %zu 行；睡眠回放 %zu 次，抽象 %zu 个]\n",
           n, ai->n_sleeps, ai->n_abstractions);
}

static int file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}


/* ============================ 元命令表 ============================
 *
 * **命令名、别名、参数形状、帮助文字只在这张表里写一次。**
 * 以前它们写了两份：派发链一份（五十个 if）、::help 一份 —— 两份会漂移，
 * 症状是「帮助里有的命令其实不认」或反过来。
 * 现在：
 *   · 派发先查表拿到 id，再 switch(id) —— switch 里**没有字符串**；
 *   · ::help 由这张表生成（连分组标题都是表的 group 列）；
 *   · 表里有而 switch 里漏了的情况，default 分支当场报出来（自检）；
 *   · tests/cmdtable.sh 把帮助里列出的每条命令都真的打一遍，确认都认。
 * 加一条命令 = 表里加一行 + switch 里写它的行为（行为是代码，没法进表）。
 *
 * 参数形状 CARG_*：NONE 不收参数（给了报用法）/ OPT 可选 / REQ 必需（缺了报用法）。
 * 帮助文字里的 %M 会替换成 DEFAULT_MODEL，免得默认文件名写死在两处。 */
typedef enum {
    CMD_HELP,
    CMD_STATS,
    CMD_SAVE,
    CMD_LOAD,
    CMD_TRAIN,
    CMD_BATCH,
    CMD_GEN,
    CMD_THINK,
    CMD_FREE,
    CMD_WALK,
    CMD_FRAMEGATE,
    CMD_COIN,
    CMD_SEG,
    CMD_RELAY,
    CMD_FALL,
    CMD_COT,
    CMD_HYPO,
    CMD_WORLD,
    CMD_VIEW,
    CMD_DER,
    CMD_STYLE,
    CMD_REL,
    CMD_WHY,
    CMD_RELINFER,
    CMD_REVISE,             /* 观测改口语义开关（默认开） */
    CMD_OP,                 /* Token 行为（图操作）：接线 / 插词 / 回收临时 */
    CMD_PREMISE,            /* 问：这两个词之间的前提 token 是什么 */
    CMD_PREMISEOUT,         /* 前提说明是否追加进答复（默认关） */
    CMD_NOTEPAD,            /* AI 自己用记事本：写草稿 / 什么时候发送 */
    CMD_SRC,                /* 来源账：这条知识是哪来的（基准/观测/推论/他说的/假设） */
    CMD_TRIGGERS,           /* 对话触发：有哪几种说法能直接干活 */
    CMD_TRUST,              /* 逐条可信度是否参与判定（默认关） */
    CMD_PREMHOLD,           /* 前提成立的判据档：说到的（默认）/ 够得着 / 有据 */
    CMD_WGATE,              /* 写入分级闸门：情境层的随口一说要不要落库 */
    CMD_DRIVE,              /* 情绪参与决策：耦合表 + 当前生效的修正 */
    CMD_ORDER,              /* 时间顺序：谁先出现 */
    CMD_NEAR,
    CMD_ANALOGY,
    CMD_TOKENS,
    CMD_EXCL,
    CMD_FRAMES,
    CMD_OTHERS,
    CMD_PRIMS,
    CMD_GC,
    CMD_ASM,
    CMD_LEARN,
    CMD_EDGES,
    CMD_QUIT,
    CMD_DELIB,
    CMD_PENDING,
    CMD_THINKRUN,
    CMD_THINKOUT,
    CMD_THINKLEARN,
    CMD_THINKCACHE,
    CMD_THINKBUDGET,
    CMD_MOD,
    CMD_SAFE,
    CMD_PATCH,
    CMD_FINGERPRINT,
    CMD_FPALL,
    CMD_FPONE,
    CMD_PAIRS,
    CMD_TOPICS,
    CMD_POOLDUMP,
    CMD_DERIVEDUMP,
    CMD_WTRACE,
    CMD_NONE
} CmdId;

typedef enum { CARG_NONE = 0, CARG_OPT, CARG_REQ } CmdArgKind;

typedef struct {
    const char *name;
    const char *alias;
    CmdArgKind  arg;
    const char *group;
    const char *help;
    CmdId       id;
} CmdDef;

static const CmdDef CMDS[] = {
    { "help", "?", 0, "基本",
      "这份帮助", CMD_HELP },
    { "stats", NULL, 0, "基本",
      "统计（Token/边/状态/计数器）", CMD_STATS },
    { "save", NULL, 1, "基本",
      "保存完整模型快照（默认 %M）", CMD_SAVE },
    { "load", NULL, 1, "基本",
      "加载模型快照（默认 %M），替换当前脑", CMD_LOAD },
    { "train", NULL, 2, "基本",
      "用原语语料继续训练（只灌图）", CMD_TRAIN },
    { "batch", NULL, 2, "基本",
      "批量训练：每 N 行内联做巩固/抽象/睡眠回放（默认 200）", CMD_BATCH },
    { "gen", NULL, 2, "基本",
      "从当前模型生成一段话", CMD_GEN },
    { "think", NULL, 2, "基本",
      "看每个词的感知子图大小", CMD_THINK },
    { "free", NULL, 1, "基本",
      "自由联想（只想不回，看关联链走到哪）", CMD_FREE },
    { "walk", NULL, 1, "基本",
      "是否让联想直接决定输出（默认 off）", CMD_WALK },
    { "framegate", NULL, 1, "基本",
      "帧槽位是否允许读关联链产物（默认 on）", CMD_FRAMEGATE },
    { "coin", NULL, 1, "基本",
      "允许现场造词：没见过的复合词造成 Token 并接线（默认 on）", CMD_COIN },
    { "seg", NULL, 1, "基本",
      "允许换切分找帧：拆解（好吗→好+吗）/ 合并（人工+智能）（默认 on）", CMD_SEG },
    { "relay", NULL, 1, "基本",
      "允许中转词容错：和/跟/与、还是/或者、会/能 互认（默认 on）", CMD_RELAY },
    { "fall", NULL, 1, "基本",
      "允许降级答复：证据不够时改说「我 不 太 懂 <你说的词>」（默认 on）", CMD_FALL },
    { "cot", NULL, 1, "基本",
      "打印上一轮的思维链（每一步在做什么、成没成、为什么）", CMD_COT },
    { "hypo", NULL, 1, "基本",
      "看/撤回反事实假设（多层嵌套；drop N 抽掉指定那层）\n说话里也能撤：算了 / 撤回 第 2 个 假设 / 撤回 关于 猫 的 假设\n撤回 猫 属于 鸟 / 猫 属于 鸟 不算 了（按内容撤指定那层）", CMD_HYPO },
    { "world", NULL, 1, "基本",
      "开放世界（默认，没写=不知道）/ 闭合世界（没写=假）", CMD_WORLD },
    { "view", NULL, 1, "基本",
      "站到别人的视角（我自己的假设他看不见）", CMD_VIEW },
    { "der", NULL, 1, "基本",
      "看上一轮的**推导链**（按步摊开，带验证标记）；\n也可现场构造：::der 猫 有 毛", CMD_DER },
    { "style", NULL, 1, "基本",
      "换一种说法说同一份链（0结论/1结论+链/2只链/3符号）", CMD_STYLE },
    { "rel", NULL, 2, "基本",
      "列出这个词的关系知识（属于/有/会/怕…）", CMD_REL },
    { "why", NULL, 2, "基本",
      "问 A 关系 B 成不成立，并打出推导路径", CMD_WHY },
    { "relinfer", NULL, 1, "基本",
      "关系层开关：推理 + 对话中现学知识（默认 on）", CMD_RELINFER },
    { "revise", NULL, 1, "基本",
      "改口语义：后说的观测替掉先说的对立极性（默认 on；off 回到旧语义做 A/B）", CMD_REVISE },
    { "op", NULL, 1, "基本",
      "Token 行为（图操作）：::op 看清单，::op <行为> <参数…> 跑一条（经安全内核）", CMD_OP },
    { "premise", NULL, 2, "基本",
      "问 A 与 B 之间的**前提 token** 是什么（前提是普通 token：联想照走、逻辑查得到）", CMD_PREMISE },
    { "drive", NULL, 1, "基本",
      "情绪参与决策：off（默认）| on；列出**耦合表**与当前生效的修正", CMD_DRIVE },
    { "order", "<A> <B>", 2, "基本",
      "时间顺序：这两个词**谁先出现**（按首次见到的 tick），并给顺序证据", CMD_ORDER },
    { "wgate", NULL, 1, "基本",
      "写入分级闸门：on = 情境层的随口一说**不落库**（借 imp 评分）；off（默认）", CMD_WGATE },
    { "premhold", NULL, 1, "基本",
      "前提成立的判据：mention（默认，只在注意窗口里算成立）| activate（图上够得着也算）", CMD_PREMHOLD },
    { "trust", NULL, 1, "基本",
      "逐条可信度是否参与判定：off（默认，只说清我更信哪一条）| on（差得多时按它选边）", CMD_TRUST },
    { "triggers", NULL, 0, "基本",
      "对话触发（自然语言 → Token 行为）有哪几种说法；关键词表各只有一处", CMD_TRIGGERS },
    { "src", "<A> <关系> <B>", 3, "基本",
      "**来源账**：这条知识是哪来的 —— 我的图（基准/观测/推论）、推得出来吗、他说的、假设层", CMD_SRC },
    { "notepad", NULL, 1, "基本",
      "AI 自己用记事本（输出侧补丁）：off（默认）| on（写结论并发送）| hold（没结论就不说）", CMD_NOTEPAD },
    { "premiseout", NULL, 1, "基本",
      "把「有前提、没成立」追加进答复（默认 on；off 是 A/B 的另一半）", CMD_PREMISEOUT },
    { "near", NULL, 2, "基本",
      "按分布式表示（emb）列出语义邻居", CMD_NEAR },
    { "analogy", NULL, 2, "基本",
      "关系类比 A:B :: C:?（emb 空间向量算术）", CMD_ANALOGY },
    { "tokens", NULL, 1, "基本",
      "列出关联最多的前 N 个 Token", CMD_TOKENS },
    { "excl", NULL, 0, "基本",
      "「独占」判定表：样本数 / 似然比 / 是印证还是改口", CMD_EXCL },
    { "frames", NULL, 0, "基本",
      "结构化帧库（问法骨架 + 槽位）", CMD_FRAMES },
    { "others", NULL, 0, "基本",
      "他者模型：每个人名下**他相信的**事实（存盘里）", CMD_OTHERS },
    { "prims", NULL, 0, "基本",
      "列出所有原语", CMD_PRIMS },
    { "gc", NULL, 0, "基本",
      "手动跑一遍遗忘", CMD_GC },
    { "asm", NULL, 2, "基本",
      "语言层直接执行 <原语序列>", CMD_ASM },
    { "learn", NULL, 2, "基本",
      "按共现学一句普通文本", CMD_LEARN },
    { "edges", NULL, 2, "基本",
      "列出某个 Token 的所有边（含 hits/状态；标出推出来的缓存）", CMD_EDGES },
    { "quit", "exit", 0, "基本",
      "退出（不自动存盘；要存用 ::save）", CMD_QUIT },
    { "delib", NULL, 1, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "看这一轮的思维过程（目标→查/推/类比/查矛盾→收束）+ 结论 + 停因 + 机制注册表", CMD_DELIB },
    { "pending", NULL, 0, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "看还挂着的未解问题（跨轮会拿新知识再试）", CMD_PENDING },
    { "thinkrun", NULL, 1, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "每轮是否都想一遍（默认 on）", CMD_THINKRUN },
    { "thinkout", NULL, 1, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "是否让思维接管输出（默认 on；只接管「矛盾」「跨轮想通」「缓存被推翻」）", CMD_THINKOUT },
    { "thinklearn", NULL, 1, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "思维能否改脑（默认 on；只经修改意图队列 + 安全内核）", CMD_THINKLEARN },
    { "thinkcache", NULL, 1, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "推理沉淀：想通的记成一条缓存（默认 on；标成『推出来的』，\n可被观测推翻、可撤销、前提没了会自动失效）", CMD_THINKCACHE },
    { "thinkbudget", NULL, 2, "思维（议程 / 跨轮未解问题 / 元认知看思维）",
      "思维步数预算（默认 12）", CMD_THINKBUDGET },
    { "mod", NULL, 1, "微内核（自修改 / 安全内核 / 补丁）",
      "自修改总开关（默认 on）；不给参数看状态与最近应用\n::mod coact|gap|prune [on|off]  单个检测器：共现固化 / 缺口命名 / 休眠剪枝\n::mod try 加边|加Token|打关系|删边|强化 …  手动提一条意图（调试入口，走的同一条\n审查/应用链路；它不是自修改，自修改必须是系统自己提的）", CMD_MOD },
    { "safe", NULL, 1, "微内核（自修改 / 安全内核 / 补丁）",
      "安全内核总闸（默认 on）；不给参数看预算/拒绝理由/审计\n::safe newtoken|remove [on|off]  允许造新 Token / 允许删边（默认都允许）\n::safe freeze on|off   手动冻结/解冻；::safe reset 解冻并清空本步预算", CMD_SAFE },
    { "patch", NULL, 1, "微内核（自修改 / 安全内核 / 补丁）",
      "补丁注册表与健康（默认一个都没挂 —— 内核不依赖补丁）\n::patch mount|unmount <名字>  挂载/卸载内建补丁；::patch on|off <名字> 启停\n::patch list        列出可用内建补丁", CMD_PATCH },
    { "fingerprint", "fp", 0, "调试/诊断",
      "内存态指纹（跨进程 diff 用）", CMD_FINGERPRINT },
    { "fpall", NULL, 0, "调试/诊断",
      "逐维度指纹（定位「哪里不一样」）", CMD_FPALL },
    { "fpone", NULL, 2, "调试/诊断",
      "一个 Token 的逐字段转储", CMD_FPONE },
    { "pairs", NULL, 1, "调试/诊断",
      "整句记忆库（可带过滤词）", CMD_PAIRS },
    { "topics", NULL, 0, "调试/诊断",
      "话题表", CMD_TOPICS },
    { "pooldump", NULL, 0, "调试/诊断",
      "整个词池 + 每一条边（按 id/名字，可跨进程 diff）", CMD_POOLDUMP },
    { "derivedump", NULL, 0, "调试/诊断",
      "派生层（idf/hv/emb/槽类/句库/话题/关系表）同样可 diff", CMD_DERIVEDUMP },
    { "wtrace", NULL, 1, "调试/诊断",
      "游走跟踪：每次选边的候选表、权重、rng、抽签结果", CMD_WTRACE },
    { NULL, NULL, CARG_NONE, NULL, NULL, CMD_NONE }
};

/* 按名字（或别名）查表。命令名**整词**匹配：::rel 与 ::relinfer 不会互相吃掉。 */
static const CmdDef *cmd_find(const char *name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; CMDS[i].name; ++i) {
        if (strcmp(CMDS[i].name, name) == 0) return &CMDS[i];
        if (CMDS[i].alias && strcmp(CMDS[i].alias, name) == 0) return &CMDS[i];
    }
    return NULL;
}

/* 帮助：**从表生成**（分组标题也是表的一列）。%M 换成默认模型文件名。 */
static void cmd_help_print(void) {
    printf("元命令（:: 与单个 : 都认）：共 %zu 条\n", sizeof CMDS / sizeof CMDS[0] - 1);
    const char *group = NULL;
    for (size_t i = 0; CMDS[i].name; ++i) {
        const CmdDef *c = &CMDS[i];
        if (!c->group) continue;
        if (!group || strcmp(group, c->group) != 0) {
            group = c->group;
            printf("\n%s：\n", group);
        }
        char head[64];
        if (c->alias) snprintf(head, sizeof head, "%s(%s)", c->name, c->alias);
        else          snprintf(head, sizeof head, "%s", c->name);
        printf("  ::%-20s ", head);
        const char *h = c->help;
        int first = 1;
        while (h && *h) {
            const char *nl = strchr(h, '\n');
            size_t len = nl ? (size_t)(nl - h) : strlen(h);
            char buf[512];
            size_t k = 0;
            for (size_t q = 0; q < len && k + 1 < sizeof buf; ++q) {
                if (h[q] == '%' && h[q + 1] == 'M') {
                    const char *m = DEFAULT_MODEL;
                    for (size_t w = 0; m[w] && k + 1 < sizeof buf; ++w) buf[k++] = m[w];
                    q++;
                } else {
                    buf[k++] = h[q];
                }
            }
            buf[k] = 0;
            if (first) { printf("%s\n", buf); first = 0; }
            else       { printf("                       %s\n", buf); }
            if (!nl) break;
            h = nl + 1;
        }
    }
}

/* 调用方式（命令行 usage）：**只此一份**。
 * 以前文件头的注释里抄了一份、cmd_help() 里又打印一份，而且两份已经漂移
 * （`--batch --save` 只出现在其中一份里，`--interval` / `-h` 两份都没有）——
 * 「同一份清单出现在两处」的症状不是崩溃，是**文档骗人**。
 * `%M` 会替换成 DEFAULT_MODEL；命令行参数都在这张表里，说明也只有这一份。 */
typedef struct {
    const char *form;   /* 调用形状 */
    const char *what;   /* 这一行干什么 */
} UsageDef;

static const UsageDef USAGE[] = {
    { "AIPoject",                             "有 ./%M 就加载它，否则最小种子" },
    { "AIPoject <语料>",                      "先按语料训练，再进对话" },
    { "AIPoject <语料> --batch",              "批量训练（每 --interval 行做一次巩固，默认 200）" },
    { "AIPoject <语料> --save <文件>",        "训练后把快照存到指定路径" },
    { "AIPoject --model <文件>",              "启动时从指定模型文件加载" },
    { "AIPoject -h | --help",                 "只打印这份帮助就退出" },
    { NULL, NULL }
};

static void cmd_help(void) {
    /* 帮助**由元命令表生成**（命令名/用法只有一份，见文件上面的 CMDS[]）。
     * 这里只补两段不属于命令的东西：调用方式与语料格式示例。 */
    cmd_help_print();
    printf("\n用法：\n");
    for (size_t i = 0; USAGE[i].form; ++i) {
        char buf[256];
        size_t k = 0;
        for (const char *h = USAGE[i].what; *h && k + 1 < sizeof buf; ++h) {
            if (h[0] == '%' && h[1] == 'M') {
                for (const char *m = DEFAULT_MODEL; *m && k + 1 < sizeof buf; ++m)
                    buf[k++] = *m;
                h++;
            } else {
                buf[k++] = *h;
            }
        }
        buf[k] = 0;
        /* 中文一字占两列，按**显示宽度**补齐 —— 按字节数 %-40s 会参差不齐 */
        size_t w = 0;
        for (const char *f = USAGE[i].form; *f;) {
            unsigned char c = (unsigned char)*f;
            if (c < 0x80) { w += 1; f += 1; }
            else if ((c & 0xE0) == 0xC0) { w += 2; f += 2; }
            else if ((c & 0xF0) == 0xE0) { w += 2; f += 3; }
            else { w += 2; f += 4; }
        }
        printf("  %s%*s%s\n", USAGE[i].form,
               w < 40 ? (int)(40 - w) : 2, "", buf);
    }
    printf("\n语料行示例（原语格式，交给 token_lang.h 解释）：\n"
           "  [原语:对象] 猫 [原语:对象] 动物 [原语:连接]\n"
           "  [原语:对象] 我 [原语:对象] 认为 [原语:对象] 猫 [原语:对象] 好 [原语:认为]\n");
}

static void cmd_tokens(const AI *ai, size_t top) {
    const TokenPool *p = ai->pool;
    size_t shown = 0;
    printf("[Token 前 %zu（按边数）]\n", top);
    /* 简单选择：反复挑当前最大 —— 语料规模下够用，不引额外内存 */
    size_t last_count = (size_t)-1;
    while (shown < top) {
        const Token *best = NULL;
        for (size_t i = 0; i < p->count; ++i) {
            const Token *t = p->tokens[i];
            if (t->is_garbage || t->count == 0) continue;
            if (t->count >= last_count) continue;
            if (!best || t->count > best->count) best = t;
        }
        if (!best) break;
        printf("  %-16s 边=%-4zu 分类=%d 状态=%u 最后碰=%llu\n",
               best->name ? best->name : "(无名)", best->count,
               (int)best->is_category, (unsigned)best->mem_state,
               (unsigned long long)best->last_touch_tick);
        last_count = best->count;
        shown++;
    }
}

static void cmd_edges(const AI *ai, const char *word) {
    const Token *t = find_token_const(ai->pool, word);
    if (!t) { printf("[没有这个 Token: %s]\n", word); return; }
    printf("[%s] 有 %zu 条边：\n", word, t->count);
    for (size_t i = 0; i < t->count; ++i) {
        const AssocEntry *e = &t->assocs[i];
        printf("  → %-16s hits=%-4u seq=%-4u 角色=%u 状态=%u",
               e->main && e->main->name ? e->main->name : "(无名)",
               e->hits, e->seq, (unsigned)e->role, (unsigned)e->mem_state);
        if (e->n_subs) {
            printf("  语境[");
            for (uint8_t k = 0; k < e->n_subs; ++k)
                printf("%s%s", k ? "," : "",
                       e->subs[k] && e->subs[k]->name ? e->subs[k]->name : "?");
            printf("]");
        }
        if (e->n_not) {
            printf("  非[");
            for (uint8_t k = 0; k < e->n_not; ++k)
                printf("%s%s", k ? "," : "",
                       e->subs_not[k] && e->subs_not[k]->name ? e->subs_not[k]->name : "?");
            printf("]");
        }
        if (edge_is_premise_link(e))
            printf("  **前提**（%s 是这两个词之间的前提 token）",
                   e->main && e->main->name ? e->main->name : "?");
        else if (e->action) printf("  动作=%s", e->action->name ? e->action->name : "?");
        /* **来源档放行尾**：老看护按「状态=N 语境[...]」的顺序钉格式，
         * 追加在最后就不会打断它们（来源是**新增**信息，不是改老字段）。 */
        printf("  来源=%s", assoc_role_name(e->role));
        printf("\n");
    }
}

static void cmd_prims(AI *ai) {
    /* **从原语表生成**（`token_lang.h` 的 PRIMS[] + PRIM_SYMS[]）——
     * 以前这里手抄了第三份名字清单，加原语时它不会跟着变：
     * 症状是 `::prims` 漏列新原语（**文档骗人**，和当年命令表那个坑同一类）。 */
    size_t np = prim_count(), ns = 0;
    while (PRIM_SYMS[ns]) ns++;
    printf("[原语 %zu 条 + 符号 %zu 个]\n", np, ns);
    for (size_t i = 0; PRIMS[i].name; ++i) {
        printf("  %s\n", PRIMS[i].name);
        if (ai) (void)pool_token_intern(ai->pool, PRIMS[i].name);   /* 列出来就顺带认得出 */
    }
    for (size_t i = 0; PRIM_SYMS[i]; ++i) {
        printf("  %s （符号）\n", PRIM_SYMS[i]);
        if (ai) (void)pool_token_intern(ai->pool, PRIM_SYMS[i]);
    }
}

/* 生成：从 seed 词出发。 */
static void cmd_gen(AI *ai, const char *arg) {
    if (!arg || !*arg) { printf("[用法] ::gen <词>\n"); return; }
    char seedbuf[256];
    snprintf(seedbuf, sizeof seedbuf, "%s", arg);
    Token *seed = pool_token_intern(ai->pool, seedbuf);
    if (!seed) { printf("[空的 seed]\n"); return; }
    float conf = 0.0f;
    char *resp = ai_plan_and_act(ai, seed, NULL, &conf, NULL, 0);
    printf("conf=%.3f  %s\n", (double)conf, resp ? resp : "");
    free(resp);
}

/* 自由联想：只跑关联链，把「想出来的东西」打印出来（不决定回复）。 */
static void cmd_free(AI *ai, const char *text) {
    if (!text || !*text) {
        /* 没给词：把上一轮的内部联想拿出来看 */
        if (ai->last_idea) printf("[内部联想] %s\n", ai->last_idea);
        else printf("[还没有联想产物；给它一个词，或先说句话]\n");
        return;
    }
    size_t n;
    char **words = ai_tokenize(ai, text, &n);
    if (!words || n == 0) { ai_free_words(words, n); printf("[空的]\n"); return; }
    Token *seed = pool_token_intern(ai->pool, words[n - 1]);
    float conf = 0.0f;
    char *idea = ai_plan_and_act(ai, seed, NULL, &conf, words, n);
    printf("[自由联想] %s\n", idea ? idea : "(没联想出来)");
    if (idea) {
        /* 用 last_walk_path（被选中那条候选的路径）：last_path 是循环里最后生成的那条，
         * 和上面打印的产物未必是同一条。模型刚加载、还没游走过时退回 last_path。 */
        const AIPath *wp = (ai->last_walk_path.n > 0) ? &ai->last_walk_path : &ai->last_path;
        printf("[路径]");
        for (size_t i = 0; i < wp->n; ++i)
            printf(" %s", wp->path[i] && wp->path[i]->name
                          ? wp->path[i]->name : "?");
        printf("\n");
    }
    free(idea);
    ai_free_words(words, n);
}

/* 思考：看每个词的感知子图。 */
static void cmd_think(AI *ai, const char *text) {
    if (!text || !*text) { printf("[用法] ::think <文本>\n"); return; }
    size_t n;
    char **words = ai_tokenize(ai, text, &n);
    if (!words || n == 0) { ai_free_words(words, n); printf("[没有可思考的词]\n"); return; }
    printf("[思考 %zu 个词]\n", n);
    for (size_t i = 0; i < n; ++i) {
        Token *t = pool_token_intern(ai->pool, words[i]);
        printf("  %-14s 边=%zu 状态=%u 缺失感=%.2f\n", words[i],
               t ? t->count : 0, t ? (unsigned)t->mem_state : 0,
               t ? (double)token_gap(t) : 1.0);
    }
    ai_free_words(words, n);
}

/* 语言层直接执行一串原语名（与语料同一路径）。 */
static void cmd_asm(AI *ai, const char *src) {
    if (!src || !*src) { printf("[用法] ::asm <原语序列>\n"); return; }
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", src);
    const char *names[256];
    size_t n = 0;
    char *q = buf;
    while (*q && n < 256) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) break;
        char *s = q;
        while (*q && *q != ' ' && *q != '\t') q++;
        if (*q) *q++ = 0;
        names[n++] = s;
    }
    if (n == 0) { printf("[空的序列]\n"); return; }
    token_run_names_ctx(ai->pool, names, n, interp_op_host, ai);
    printf("[执行了 %zu 个 token]\n", n);
}

int main(int argc, char **argv) {
    const char *corpus = NULL;
    const char *model_path = DEFAULT_MODEL;
    const char *save_after_train = NULL;
    int    batch = 0;
    size_t interval = 200;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) { model_path = argv[++i]; continue; }
        if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) { save_after_train = argv[++i]; continue; }
        if (strcmp(argv[i], "--batch") == 0) { batch = 1; continue; }
        if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v > 0) interval = (size_t)v;
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { cmd_help(); return 0; }
        if (!corpus) corpus = argv[i];
    }

    Agent *a = agent_new(stdin_perceive, stdout_act, NULL);
    if (!a) { fprintf(stderr, "agent_new 失败\n"); return 1; }

    /* 1) 给了语料 → 先训练 */
    size_t trained = 0;
    if (corpus) {
        if (batch) train_corpus_batch(a->brain, corpus, interval);
        else       trained = train_on_corpus(a->brain, corpus);
    }

    /* 2) 没给语料 → 尝试加载模型 */
    int loaded = 0;
    if (!corpus && file_exists(model_path)) {
        AI *m = ai_load_file(model_path);
        if (m) {
            ai_free(a->brain);
            a->brain = m;
            loaded = 1;
            printf("[已加载模型 %s]\n", model_path);
        } else {
            fprintf(stderr, "[加载失败 %s —— 退到最小种子]\n", model_path);
        }
    }
    if (!corpus && !loaded) {
        ai_learn_text(a->brain, "我 是 我");
        ai_learn_text(a->brain, "我 在 这里");
        ai_learn_text(a->brain, "你 好");
    }

    /* 3) 训练后按需立刻存盘 */
    if (save_after_train) {
        size_t sz = ai_save_file(a->brain, save_after_train);
        if (sz) printf("[训练完成，已保存 %zu 字节 → %s]\n", sz, save_after_train);
        else    printf("[保存失败 → %s]\n", save_after_train);
    }

    ai_stats(a->brain);
    if (trained) printf("[本次训练 %zu 行；'::save' 保存模型]\n", trained);
    printf("(Ctrl-D 退出，'::help' 看命令，'::save 文件' 保存模型)\n\n");

    int want_quit = 0;
    while (1) {
        if (want_quit) break;
        const char *input = stdin_perceive(NULL);
        if (!input) break;

        /* 元命令：::xxx 与 :xxx 都认 */
        const char *cmd = NULL;
        if (input[0] == ':') cmd = (input[1] == ':') ? input + 2 : input + 1;

        if (cmd) {
            while (*cmd == ' ') cmd++;
            if (*cmd == 0) { cmd_help(); continue; }     /* 裸 :: 直接给帮助 */
            /* 拆成「命令名 + 参数」，查**元命令表**拿 id。
             * 名字整词匹配（::rel 与 ::relinfer 不会互相吃掉）。 */
            char cname[64];
            const char *carg = NULL;
            {
                const char *sp = strchr(cmd, ' ');
                size_t nl = sp ? (size_t)(sp - cmd) : strlen(cmd);
                if (nl >= sizeof cname) nl = sizeof cname - 1;
                memcpy(cname, cmd, nl); cname[nl] = 0;
                carg = sp ? sp + 1 : NULL;
                while (carg && *carg == ' ') carg++;
            }
            const CmdDef *cd = cmd_find(cname);
            if (!cd) {
                printf("[未知命令 %s —— '::help' 看帮助]\n", cname);
                continue;
            }
            if (cd->arg == CARG_NONE && carg && *carg) {
                printf("[%s 不收参数 —— 用法：::%s]\n", cname, cname);
                continue;
            }
            if (cd->arg == CARG_REQ && (!carg || !*carg)) {
                printf("[%s 要参数 —— 用法见 ::help]\n", cname);
                continue;
            }
            switch (cd->id) {
            case CMD_HELP: { cmd_help(); continue; }
            case CMD_STATS: {
                ai_stats(a->brain);
                (void)micro_of(a->brain); micro_stats(a->brain);
                (void)think_of(a->brain); think_stats(a->brain);
                continue;
            }
            /* ---- 思维循环 ---- */
            case CMD_DELIB: {
                (void)think_of(a->brain);
                think_report(a->brain);
                continue;
            }
            case CMD_PENDING: {
                (void)think_of(a->brain);
                think_pending_report(a->brain);
                continue;
            }
            case CMD_THINKRUN: case CMD_THINKOUT: case CMD_THINKLEARN: case CMD_THINKCACHE: {
                const char *sp = strchr(cmd, ' ');
                const char *arg = sp ? sp + 1 : NULL;
                while (arg && *arg == ' ') arg++;
                int on = (arg && !strcmp(arg, "off")) ? 0 : 1;
                if (strncmp(cmd, "thinkrun", 8) == 0)        think_set_run(a->brain, on);
                else if (strncmp(cmd, "thinkout", 8) == 0)   think_set_out(a->brain, on);
                else if (strncmp(cmd, "thinkcache", 10) == 0) think_set_cache(a->brain, on);
                else                                        think_set_learn(a->brain, on);
                printf("[%s = %s]\n",
                       strncmp(cmd, "thinkrun", 8) == 0 ? "每轮想一遍" :
                       strncmp(cmd, "thinkout", 8) == 0 ? "思维接管输出" :
                       strncmp(cmd, "thinkcache", 10) == 0 ? "推理沉淀（想通了就记住）"
                                                          : "思维可改脑",
                       on ? "开" : "关");
                continue;
            }
            case CMD_THINKBUDGET: {
                const char *arg = (cmd[11] == ' ') ? cmd + 12 : NULL;
                while (arg && *arg == ' ') arg++;
                long v = arg ? strtol(arg, NULL, 10) : 0;
                if (v > 0) {
                    think_set_budget(a->brain, (size_t)v);
                    printf("[思维步数预算 = %ld（上限 %d）]\n", v, THINK_MAX_STEP);
                } else {
                    printf("[用法] ::thinkbudget <步数>（上限 %d）\n", THINK_MAX_STEP);
                }
                continue;
            }
            /* ---- 微内核：自修改通道 ---- */
            case CMD_PREMISE: {
                /* 「逻辑知道这两个之间的前提 token 是什么」——这一条就是那句话的实现。
                 * 前提 token 是**普通 token**（联想照走），所以这里只回答它是谁。 */
                const char *arg = (cmd[7] == ' ') ? cmd + 8 : NULL;
                while (arg && *arg == ' ') arg++;
                char w1[64] = {0}, w2[64] = {0};
                if (!arg || sscanf(arg, "%63s %63s", w1, w2) != 2) {
                    printf("[用法] ::premise <A> <B>（问这两个之间的前提 token 是什么）\n");
                    continue;
                }
                Token *A = pool_find(a->brain->pool, w1);
                Token *B = pool_find(a->brain->pool, w2);
                if (!A || !B) { printf("[找不到词：%s %s]\n", w1, w2); continue; }
                const Token *ps[8];
                int pol[8];
                size_t np = token_premises(A, B, ps, pol, 8);
                if (np == 0) {
                    printf("[%s 与 %s 之间没有前提]（直接连结，或压根不相连）\n", w1, w2);
                } else {
                    printf("[%s 与 %s 之间的前提 token：%zu 个]\n", w1, w2, np);
                    for (size_t i = 0; i < np; ++i) {
                        const Token *p = ps[i];
                        int ok = ai_premise_cond_ok(a->brain, p, pol[i]);
                        /* 说清**是靠哪一档**成立的：提到过 / 图上够得着 / 有据 */
                        const char *how = "没成立";
                        if (ok) {
                            int in_focus = 0;
                            for (size_t k = 0; k < a->brain->focus_n; ++k)
                                if (a->brain->focus_ring[k] == p) { in_focus = 1; break; }
                            if (in_focus)                        how = "成立（提到过）";
                            else if (ai_premise_wellfounded(a->brain, p)) how = "成立（够得着·有据）";
                            else                                 how = "成立（够得着）";
                        }
                        printf("    %s%s  （%s；普通 token：联想照走、::rel/::edges 都能正常处理它）\n",
                               pol[i] < 0 ? "前提不：" : "前提：",
                               p && p->name ? p->name : "?", how);
                    }
                }
                continue;
            }
            case CMD_NOTEPAD: {
                /* 注意：`cmd` 已经剥掉 `::`（见主循环），所以下标按 "notepad"=7 算 */
                const char *arg = (cmd[7] == ' ') ? cmd + 8 : NULL;
                while (arg && *arg == ' ') arg++;
                Think *t = think_of(a->brain);
                if (arg && *arg) {
                    if (!strcmp(arg, "off"))       t->notepad = 0;
                    else if (!strcmp(arg, "on"))   t->notepad = 1;
                    else if (!strcmp(arg, "hold")) t->notepad = 2;
                    else { printf("[用法] ::notepad off|on|hold\n"); continue; }
                }
                printf("[AI 自己用记事本 = %s]\n",
                       t->notepad == 0 ? "关（默认）" : t->notepad == 1 ? "开：有结论就写草稿并发送"
                                                                      : "hold：没结论就按住不说");
                printf("  前提：::patch mount 记事本（没挂补丁时它什么都不做）\n");
                printf("  %s\n", t->notepad ? "它会走受审通道调用 记事本 的操作（::delib 里有痕）"
                                            : "只留在 ::delib 的痕上，不改输出");
                continue;
            }
            case CMD_PREMISEOUT: {
                const char *arg = (cmd[10] == ' ') ? cmd + 11 : NULL;
                while (arg && *arg == ' ') arg++;
                Think *t = think_of(a->brain);
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) t->premise_out_on = 1;
                    else if (strcmp(arg, "off") == 0) t->premise_out_on = 0;
                    else { printf("[用法] ::premiseout on|off\n"); continue; }
                }
                printf("[前提说明进答复 = %s]（%s）\n",
                       t->premise_out_on ? "开（默认）" : "关",
                       t->premise_out_on
                           ? "有连结因前提没成立而没用时，会在回答后面追加一句说明"
                           : "只留在 ::delib 的痕上，不改回答");
                printf("  量过的：tests/premiseout_ab.sh（8 套行为看护，开/关两遍 → 0/8 变）\n");
                continue;
            }
            case CMD_OP: {
                /* Token 行为（图操作）：一张 TOKEN_OPS[] 表，表里的展开函数只产出
                 * ModIntent，逐条走「审查 → 记账 → 应用 → 审计」——
                 * 和自修改同一条路，所以「Token 有行为」并不等于绕过安全内核。 */
                const char *arg = (cmd[2] == ' ') ? cmd + 3 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) {
                    printf("[Token 行为（图操作）：%zu 条]\n", token_op_count());
                    for (size_t i = 0; TOKEN_OPS[i].name; ++i)
                        printf("  ::op %-12s %-22s %s\n", TOKEN_OPS[i].name,
                               TOKEN_OPS[i].shape, TOKEN_OPS[i].help);
                    size_t ntemp = 0;
                    for (size_t i = 0; i < a->brain->pool->count; ++i)
                        if (token_is_temp(a->brain->pool->tokens[i]) &&
                            !a->brain->pool->tokens[i]->is_garbage) ntemp++;
                    printf("  当前未回收的临时 Token：%zu 个（不存盘；::op 回收临时 清理）\n", ntemp);
                    continue;
                }
                char *args[4] = {NULL, NULL, NULL, NULL};
                size_t nargs = 0;
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", arg);
                for (char *q = tmp; *q && nargs < 4; ) {
                    while (*q == ' ' || *q == '\t') q++;
                    if (!*q) break;
                    args[nargs++] = q;
                    while (*q && *q != ' ' && *q != '\t') q++;
                    if (*q) *q++ = 0;
                }
                char onote[240];
                int got = token_op_run(a->brain, args[0], args + 1, nargs ? nargs - 1 : 0,
                                       onote, sizeof onote);
                if (got < 0) printf("[行为没跑成] %s\n", onote);
                else         printf("[行为 %s] %s\n", args[0], onote);
                continue;
            }
            case CMD_MOD: {
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { (void)micro_of(a->brain); micro_report(a->brain); continue; }
                /* ::mod try …：手动提一条意图，走同一条审查/应用路径。
                 * 这是**调试入口**，不是自修改（自修改必须是系统自己提的）。 */
                if (strncmp(arg, "try", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
                    const char *rest = arg + 3;
                    while (*rest == ' ') rest++;
                    char op[16] = {0}, x[40] = {0}, y[40] = {0}, z[40] = {0};
                    int nn = sscanf(rest, "%15s %39s %39s %39s", op, x, y, z);
                    if (nn < 2) {
                        printf("[用法] ::mod try 加边 <A> <B> | 加Token <名字> [<连回>] | "
                               "打关系 <A> <关系> <B> | 删边 <A> <B> | 强化 <A> <B>\n");
                        continue;
                    }
                    ModIntent in;
                    memset(&in, 0, sizeof in);
                    Token *A = x[0] ? pool_token_intern(a->brain->pool, x) : NULL;
                    Token *B = y[0] ? pool_token_intern(a->brain->pool, y) : NULL;
                    Token *C = z[0] ? pool_token_intern(a->brain->pool, z) : NULL;
                    int bad = 0;
                    if (!strcmp(op, "加边") || !strcmp(op, "add")) {
                        in.type = MOD_ADD_ASSOC; in.target = A; in.main = B;
                    } else if (!strcmp(op, "删边") || !strcmp(op, "del")) {
                        in.type = MOD_REMOVE_ASSOC; in.target = A; in.main = B;
                    } else if (!strcmp(op, "强化") || !strcmp(op, "bump")) {
                        in.type = MOD_BUMP_HITS; in.target = A; in.main = B; in.bump = REL_BOOST;
                    } else if (!strcmp(op, "加Token") || !strcmp(op, "addtok")) {
                        in.type = MOD_ADD_TOKEN; in.target = B;
                        snprintf(in.name, sizeof in.name, "%s", x);
                    } else if (!strcmp(op, "打关系") || !strcmp(op, "rel")) {
                        const RelDef *rd = rel_by_name(y);
                        if (!rd) { printf("[没有这个关系：%s（::rel 看词表）]\n", y); continue; }
                        in.type = MOD_TAG_REL; in.target = A; in.main = C; in.rel_id = rd->id;
                    } else {
                        bad = 1;
                    }
                    if (bad) {
                        printf("[用法] ::mod try 加边 <A> <B> | 加Token <名字> [<连回>] | "
                               "打关系 <A> <关系> <B> | 删边 <A> <B> | 强化 <A> <B>\n");
                        continue;
                    }
                    safety_step_begin(a->brain);      /* 一条命令算一步 */
                    char note[128];
                    SafeReason r = micro_try(a->brain, &in, note, sizeof note);
                    if (r == SAFE_OK)
                        printf("[手动意图已应用] %s %s\n", micro_mod_type_name(in.type), note);
                    else
                        printf("[手动意图被拒] %s —— %s\n", micro_mod_type_name(in.type),
                               SAFE_REASON_NAME[r]);
                    continue;
                }
                char sub[16] = {0}, val[16] = {0};
                int nf = sscanf(arg, "%15s %15s", sub, val);
                if (nf < 1) { printf("[用法] ::mod [on|off] | ::mod coact|gap|prune|cond [on|off]\n"); continue; }
                if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
                    int on = !strcmp(sub, "on");
                    micro_set_mod(a->brain, on);
                    printf("[自修改通道 = %s]\n", on ? "开" : "关");
                } else if (!strcmp(sub, "coact") || !strcmp(sub, "gap") || !strcmp(sub, "prune") ||
                           !strcmp(sub, "cond")) {
                    if (val[0] && strcmp(val, "on") && strcmp(val, "off")) {
                        printf("[用法] ::mod %s [on|off]\n", sub);
                        continue;
                    }
                    int on = !val[0] || !strcmp(val, "on");
                    micro_set_detector(a->brain, sub, on);
                    printf("[检测器 %s = %s]\n", sub, on ? "开" : "关");
                } else {
                    printf("[用法] ::mod [on|off] | ::mod coact|gap|prune|cond [on|off]\n");
                }
                continue;
            }
            /* ---- 微内核：安全内核 ---- */
            case CMD_SAFE: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { (void)micro_of(a->brain); safety_report(a->brain); continue; }
                char sub[16] = {0}, val[16] = {0};
                int nf = sscanf(arg, "%15s %15s", sub, val);
                if (nf < 1) { printf("[用法] ::safe [on|off|reset|freeze on|off|newtoken on|off|remove on|off|route on|off]\n"); continue; }
                if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
                    int on = !strcmp(sub, "on");
                    safety_set(a->brain, "mod", on);
                    printf("[安全内核总闸 = %s（关 = 一律拒绝自修改）]\n", on ? "开" : "关");
                } else if (!strcmp(sub, "reset")) {
                    safety_reset(a->brain);
                    printf("[安全内核：已解冻并清空本步预算与关小状态]\n");
                } else if (!strcmp(sub, "route")) {
                    /* 统一写入通道：把**知识类**写入（关系事实）改成走受审通道。
                     * 默认关 —— 观测直写是正常路径；打开是"让内核看住知识写入"。 */
                    if (val[0] && strcmp(val, "on") && strcmp(val, "off")) {
                        printf("[用法] ::safe route on|off\n");
                        continue;
                    }
                    int on = !strcmp(val, "on");
                    (void)micro_set_route(a->brain, on);
                    printf("[统一写入通道 路由 = %s]\n", on ? "开（关系事实走受审通道）" : "关（直写）");
                    continue;
                } else if (!strcmp(sub, "freeze")) {
                    if (val[0] && strcmp(val, "on") && strcmp(val, "off")) { printf("[用法] ::safe freeze on|off\n"); continue; }
                    int on = !val[0] || !strcmp(val, "on");
                    safety_set(a->brain, "freeze", on);
                    printf("[冻结 = %s]\n", on ? "是（一律拒绝）" : "否");
                } else if (!strcmp(sub, "newtoken") || !strcmp(sub, "remove")) {
                    if (val[0] && strcmp(val, "on") && strcmp(val, "off")) { printf("[用法] ::safe %s on|off\n", sub); continue; }
                    int on = !val[0] || !strcmp(val, "on");
                    safety_set(a->brain, sub, on);
                    printf("[%s = %s]\n", !strcmp(sub, "newtoken") ? "造新Token" : "删边", on ? "允许" : "禁止");
                } else {
                    printf("[用法] ::safe [on|off|reset|freeze on|off|newtoken on|off|remove on|off]\n");
                }
                continue;
            }
            /* ---- 微内核：补丁注册表 ---- */
            case CMD_PATCH: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { (void)micro_of(a->brain); patch_report(a->brain); continue; }
                char sub[16] = {0}, name[32] = {0};
                int nf = sscanf(arg, "%15s %31s", sub, name);
                if (nf >= 1 && !strcmp(sub, "list")) { patch_list_builtin(); continue; }
                if (nf < 2) { printf("[用法] ::patch [mount|unmount|on|off <名字>|op <名字> <操作> [参数…]] | ::patch list\n"); continue; }
                if (!strcmp(sub, "op")) {
                    /* 人工/测试入口：::patch op 记事本 写 你好 —— 和 AI 自己走
                     * 「调用补丁」行为是同一条路（同一个 op 派发器）。 */
                    char rest[512];
                    snprintf(rest, sizeof rest, "%s", arg + strlen(sub));
                    char *sp = rest;
                    while (*sp == ' ') sp++;
                    char *pn = strtok(sp, " ");
                    char *opn = pn ? strtok(NULL, " ") : NULL;
                    if (!pn || !opn) { printf("[用法] ::patch op <补丁> <操作> [参数…]\n"); continue; }
                    /* 参数按空格切，但 **双引号里算一个**（这样 `改发言 "猫 跑" 走`
                     * 才能替换带空格的串 —— 以前单 token 的限制就在这儿）。 */
                    char *argv[6];
                    size_t an = 0;
                    {
                        char *q = strtok(NULL, "");
                        while (q && an < 6) {
                            while (*q == ' ') q++;
                            if (!*q) break;
                            if (*q == '"') {
                                q++;
                                char *close = strchr(q, '"');
                                if (close) *close = 0;
                                argv[an++] = q;
                                q = close ? close + 1 : NULL;
                            } else {
                                char *sp = strchr(q, ' ');
                                if (sp) *sp = 0;
                                argv[an++] = q;
                                q = sp ? sp + 1 : NULL;
                            }
                        }
                    }
                    char rep[200];
                    int rc = patch_op_call_by_name(a->brain, pn, opn, argv, an, rep, sizeof rep);
                    if (rc < 0) printf("[补丁 %s 不认这个操作，或没挂着：%s]\n", pn, opn);
                    else        printf("[补丁 %s.%s] %s\n", pn, opn, rep[0] ? rep : "ok");
                    continue;
                }
                if (!strcmp(sub, "mount")) {
                    int rc = patch_mount_builtin(a->brain, name);
                    if (rc == 0)       printf("[已挂载补丁 %s]\n", name);
                    else if (rc == -2) printf("[%s 已经挂着了]\n", name);
                    else               printf("[没有这个补丁：%s（::patch list 看可用）]\n", name);
                } else if (!strcmp(sub, "unmount")) {
                    if (patch_unmount_by_name(a->brain, name) == 0) printf("[已卸载补丁 %s]\n", name);
                    else printf("[没有挂载 %s]\n", name);
                } else if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
                    int on = !strcmp(sub, "on");
                    if (patch_set_enabled(a->brain, name, on) == 0)
                        printf("[补丁 %s = %s]\n", name, on ? "启用" : "停用");
                    else printf("[没有挂载 %s]\n", name);
                } else {
                    printf("[用法] ::patch [mount|unmount|on|off <名字>] | ::patch list\n");
                }
                continue;
            }
            case CMD_FINGERPRINT: {
                size_t na = 0;
                for (size_t i = 0; i < a->brain->pool->count; ++i)
                    na += a->brain->pool->tokens[i]->count;
                printf("[指纹] %016llx  tick=%llu token=%zu 边=%zu 已学=%zu 轮次=%zu\n",
                       (unsigned long long)state_fingerprint(a->brain),
                       (unsigned long long)a->brain->pool->current_tick,
                       a->brain->pool->count, na, a->brain->n_learned, a->brain->turns);
                fingerprint_dims(a->brain);
                continue;
            }
            case CMD_FPALL: { fingerprint_all(a->brain); continue; }
            case CMD_FPONE: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { printf("[用法] ::fpone <词>\n"); continue; }
                const Token *t = find_token_const(a->brain->pool, arg);
                if (!t) { printf("[没有 %s]\n", arg); continue; }
                printf("[%s] count=%zu cap=%zu next_seq=%u\n", arg, t->count, t->cap, t->next_seq);
                for (size_t j = 0; j < t->count; ++j) {
                    const AssocEntry *e = &t->assocs[j];
                    printf("  E%zu main=%s act=%s hits=%u seq=%u mem=%u bt=%llu lt=%llu ns=%u",
                           j, e->main ? e->main->name : "-",
                           e->action ? e->action->name : "-", e->hits, e->seq,
                           (unsigned)e->mem_state,
                           (unsigned long long)e->birth_tick,
                           (unsigned long long)e->last_touch_tick, (unsigned)e->n_subs);
                    for (uint8_t k = 0; k < e->n_subs; ++k)
                        printf(" sub[%u]=%s", k, e->subs[k] && e->subs[k]->name ? e->subs[k]->name : "?");
                    printf("\n");
                }
                continue;
            }
            case CMD_PAIRS: {
                size_t top = 12;
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                if (arg) { long v = strtol(arg, NULL, 10); if (v > 0 && v < 10000) top = (size_t)v; }
                printf("[整句库 %zu 对]\n", a->brain->pairs_n);
                for (size_t i = 0; i < a->brain->pairs_n && i < top; ++i) {
                    const AISentPair *p = &a->brain->pairs[i];
                    printf("  %zu [话题%u] IN:", i, (unsigned)p->topic);
                    for (size_t k = 0; k < p->in_n; ++k)
                        printf(" %s", p->in[k] ? p->in[k]->name : "?");
                    printf("   OUT:");
                    for (size_t k = 0; k < p->out_n; ++k)
                        printf(" %s", p->out[k] ? p->out[k]->name : "?");
                    printf("\n");
                }
                continue;
            }
            case CMD_TOPICS: {
                for (uint8_t t = 0; t < AI_MAX_TOPICS; ++t) {
                    if (a->brain->topics[t].n == 0) continue;
                    printf("话题 %u: %zu 个词  例:", (unsigned)t, a->brain->topics[t].n);
                    for (size_t k = 0; k < a->brain->topics[t].n && k < 8; ++k)
                        printf(" %s", a->brain->topics[t].seen[k]->name);
                    printf("\n");
                }
                printf("当前锁定话题 = %u\n", (unsigned)a->brain->cur_topic);
                continue;
            }
            case CMD_FRAMES: {
                FrameSet *fs = (FrameSet *)a->brain->frames;
                if (!fs) { printf("[没有帧库]\n"); continue; }
                printf("[帧库 %zu 个]  帧生成次数=%zu\n", fs->n_fr, a->brain->n_frames);
                for (size_t i = 0; i < fs->n_fr; ++i) {
                    Frame *fr = &fs->fr[i];
                    printf("  %-14s 问法=%zu 回复=%zu", fr->name,
                           fr->ask.n_tmpl, fr->ans.n_tmpl);
                    for (size_t k = 0; k < fr->n_slot; ++k)
                        printf("  <%s>允许%zu", fr->slot[k].name, fr->slot[k].n_fill);
                    printf("\n");
                }
                continue;
            }
            case CMD_PRIMS: { cmd_prims(a->brain); continue; }
            case CMD_GC: { ai_gc(a->brain); ai_stats(a->brain); continue; }
            case CMD_SAVE: {
                const char *path = (cmd[4] == ' ') ? cmd + 5 : model_path;
                while (*path == ' ') path++;
                if (!*path) path = model_path;
                size_t sz = ai_save_file(a->brain, path);
                if (sz) printf("[已保存 %zu 字节 → %s]\n", sz, path);
                else    printf("[保存失败 → %s]\n", path);
                continue;
            }
            case CMD_LOAD: {
                const char *path = (cmd[4] == ' ') ? cmd + 5 : model_path;
                while (*path == ' ') path++;
                if (!*path) path = model_path;
                AI *m = ai_load_file(path);
                if (m) {
                    ai_free(a->brain);
                    a->brain = m;
                    printf("[已加载模型 %s]\n", path);
                    ai_stats(a->brain);
                } else {
                    printf("[加载失败 %s]\n", path);
                }
                continue;
            }
            case CMD_BATCH: {
                const char *path = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (path && *path == ' ') path++;
                if (!path || !*path) { printf("[用法] ::batch <语料文件> [间隔]\n"); continue; }
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s", path);
                size_t iv = interval;
                char *sp = strchr(tmp, ' ');
                if (sp) { *sp = 0; long v = strtol(sp + 1, NULL, 10); if (v > 0) iv = (size_t)v; }
                train_corpus_batch(a->brain, tmp, iv);
                ai_stats(a->brain);
                continue;
            }
            case CMD_TRAIN: {
                const char *path = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (path && *path == ' ') path++;
                if (!path || !*path) { printf("[用法] ::train <语料文件>\n"); continue; }
                size_t n = train_on_corpus(a->brain, path);
                printf("[继续训练 %zu 行]\n", n);
                ai_stats(a->brain);
                continue;
            }
            case CMD_FREE: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                cmd_free(a->brain, arg);
                continue;
            }
            case CMD_WALK: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->walk_output = 1;
                    else if (strcmp(arg, "off") == 0) a->brain->walk_output = 0;
                    else { printf("[用法] ::walk on|off\n"); continue; }
                }
                printf("[联想直接输出 = %s]\n",
                       a->brain->walk_output ? "开（输出会是关联链的产物）" : "关（默认：只做内部思考）");
                continue;
            }
            case CMD_FRAMEGATE: {
                const char *arg = (cmd[9] == ' ') ? cmd + 10 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->assoc_slot_off = 0;
                    else if (strcmp(arg, "off") == 0) a->brain->assoc_slot_off = 1;
                    else { printf("[用法] ::framegate on|off\n"); continue; }
                }
                printf("[帧读关联链产物 = %s]（放行次数 %zu）\n",
                       a->brain->assoc_slot_off
                           ? "关（槽位只认 frames.txt 白名单）"
                           : "开（默认：白名单外的词按关联链证据放行）",
                       a->brain->n_frame_assoc);
                continue;
            }
            case CMD_COIN: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->coin_off = 0;
                    else if (strcmp(arg, "off") == 0) a->brain->coin_off = 1;
                    else { printf("[用法] ::coin on|off\n"); continue; }
                }
                printf("[现场造词 = %s]（已造出 %zu 个）\n",
                       a->brain->coin_off
                           ? "关（槽位取值不认识的复合词就按字拆）"
                           : "开（默认：把「火锅」这种没见过的词造成 Token 并现场接线）",
                       a->brain->n_coined);
                continue;
            }
            case CMD_SEG: {
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->seg_off = 0;
                    else if (strcmp(arg, "off") == 0) a->brain->seg_off = 1;
                    else { printf("[用法] ::seg on|off\n"); continue; }
                }
                printf("[换切分找帧 = %s]（拆解 %zu 次 / 合并 %zu 次）\n",
                       a->brain->seg_off ? "关（只用切词器那一版切分）"
                                         : "开（默认：主切分不中时试拆解/合并）",
                       a->brain->n_split, a->brain->n_merge);
                continue;
            }
            case CMD_RELAY: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->relay_off = 0;
                    else if (strcmp(arg, "off") == 0) a->brain->relay_off = 1;
                    else { printf("[用法] ::relay on|off\n"); continue; }
                }
                printf("[中转词容错 = %s]（生效 %zu 次）\n",
                       a->brain->relay_off ? "关（模板的「和」只认「和」）"
                                           : "开（默认：和/跟/与、还是/或者、会/能 互认）",
                       a->brain->n_relay);
                continue;
            }
            case CMD_COT: {
                AI *b = a->brain;
                if (b->cot.n == 0) {
                    printf("[思维链] 还没有——先说句话\n");
                    continue;
                }
                printf("[思维链：上一轮 %zu 步，累计 %zu 步]\n", b->cot.n, b->n_cot);
                for (size_t i = 0; i < b->cot.n; ++i) {
                    const CotStep *s = &b->cot.step[i];
                    printf("  %zu. %-4s %s  %s%s\n",
                           i + 1, cot_kind_name(s->kind),
                           s->ok ? "[成]" : "[没成]",
                           s->what,
                           s->why[0] ? "" : "");
                    if (s->why[0]) printf("        └ %s\n", s->why);
                }
                continue;
            }
            case CMD_FALL: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->fall_off = 0;
                    else if (strcmp(arg, "off") == 0) a->brain->fall_off = 1;
                    else { printf("[用法] ::fall on|off\n"); continue; }
                }
                printf("[降级答复 = %s]（兜住 %zu 次）\n",
                       a->brain->fall_off
                           ? "关（骨架接不住就只剩拒答）"
                           : "开（默认：改说「我 不 太 懂 <你 说 的 词>」）",
                       a->brain->n_hedge);
                continue;
            }
            case CMD_OTHERS: {
                /* 他者模型：每个人名下**他相信的事实**（不是我的知识）。
                 * 这是「归属」从标注变成持久状态的地方 —— 存盘重载还在。 */
                AI *B = a->brain;
                if (B->others_n == 0) { printf("[还没有「别人」的模型 —— 说一句「他 以为 …」试试]\n"); continue; }
                for (size_t i = 0; i < B->others_n; ++i) {
                    const OtherModel *om = &B->others[i];
                    printf("[他者 %zu] %s  信任=%.2f 互动=%zu 听他说的=%zu 条\n",
                           i, om->who && om->who->name ? om->who->name : "?", (double)om->trust,
                           om->n_interactions, om->n_bel_learned);
                    for (size_t k = 0; k < om->n_bel; ++k) {
                        const HypoFact *f = &om->bel[k];
                        const RelDef *rd = rel_by_id(f->rid);
                        printf("    %s %s %s %s\n",
                               f->a && f->a->name ? f->a->name : "?",
                               f->neg ? "不" : "",
                               rd && rd->name ? rd->name : "?",
                               f->b && f->b->name ? f->b->name : "?");
                    }
                }
                continue;
            }
            case CMD_EXCL: {
                /* 「独占」是**量**出来的，这里把量它的证据摆出来：
                 * 参照类给的多宾语率 p1、每条关系的样本数、似然比、判定与来源。 */
                static const char *nm[REL_TABLE_MAX + 1];
                nm[REL_IS_A] = "属于"; nm[REL_HAS] = "有"; nm[REL_CAN] = "会";
                nm[REL_FEAR] = "怕";   nm[REL_LIKE] = "喜欢"; nm[REL_EAT] = "吃";
                nm[REL_AT] = "在";
                const double bar = 10.0;   /* REL_EXCL_ODDS */
                printf("[关系独占表] 判据=似然比（≥%.0f:1 才改口）；参照多宾语率 %.1f%%"
                       "（只取「观察到过多宾语」的关系当参照类）\n",
                       bar, a->brain->rel_excl_base / 10.0);
                for (uint8_t r = 1; r <= REL_AT; ++r) {
                    if (!nm[r]) continue;
                    size_t sg = a->brain->rel_excl_single[r];
                    size_t mu = a->brain->rel_excl_multi[r];
                    double llr = (double)a->brain->rel_excl_llr[r];
                    double odds = exp(llr < 0 ? -llr : llr);
                    const char *side = (llr < 0) ? "独占" : "多值";
                    /* 印证 / 改口 比的是 AI_RELS 里的**声明值**（那才是先验），
                     * 不是当前值 —— 当前值已经被数据算过了，拿它比永远是「印证」。 */
                    const RelDef *def = rel_by_id((uint8_t)r);
                    int declared = def ? (def->excl ? 1 : 0) : 0;
                    const char *src;
                    if (odds >= bar) {
                        src = (a->brain->rel_excl[r] == declared) ? "数据印证" : "数据改口";
                    } else {
                        src = "证据不够→按声明";
                    }
                    printf("  %-4s %s  样本 %zu 主语（单 %zu / 多 %zu）"
                           "  似然比 %.1f:1 支持%s  [%s]\n",
                           nm[r], a->brain->rel_excl[r] ? "独占" : "多值",
                           sg + mu, sg, mu, odds, side, src);
                }
                continue;
            }
            case CMD_REL: {
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { printf("[用法] ::rel <词>\n"); continue; }
                Token *t = pool_find(a->brain->pool, arg);
                if (!t) { printf("[%s] 不在图里\n", arg); continue; }
                size_t shown = 0;
                printf("[%s] 的关系知识:\n", arg);
                for (size_t i = 0; i < t->count; ++i) {
                    const AssocEntry *e = &t->assocs[i];
                    if (!e->main || !e->main->name) continue;
                    /* 否定关系 */
                    if (e->nrel) {
                        for (size_t k = 0; AI_RELS[k].name; ++k) {
                            if (!(e->nrel & REL_BIT(AI_RELS[k].id))) continue;
                            int dup = 0;
                            for (size_t q = 0; q < k; ++q)
                                if (AI_RELS[q].id == AI_RELS[k].id) { dup = 1; break; }
                            if (dup) continue;
                            printf("   %s --[不 %s]--> %s\n", arg,
                                   AI_RELS[k].name, e->main->name);
                            shown++;
                        }
                    }
                    /* 动态关系 */
                    if (e->dyn) {
                        const char *dn = rel_dyn_name(a->brain, (uint8_t)(REL_DYN_BASE + e->dyn - 1));
                        printf("   %s --[%s]--> %s\n", arg, dn ? dn : "?", e->main->name);
                        shown++;
                    }
                    if (!e->rel) continue;
                    /* 一条边可能同时挂多种关系，逐位打出来 */
                    for (size_t k = 0; AI_RELS[k].name; ++k) {
                        if (!(e->rel & REL_BIT(AI_RELS[k].id))) continue;
                        /* 同名同 id 的别重复打（属于/是 是同一个 id） */
                        int dup = 0;
                        for (size_t q = 0; q < k; ++q)
                            if (AI_RELS[q].id == AI_RELS[k].id) { dup = 1; break; }
                        if (dup) continue;
                        printf("   %s --[%s]--> %s%s\n", arg,
                               AI_RELS[k].name, e->main->name,
                               e->role == ASSOC_ROLE_DERIVED ? "（推出来的，可撤）" : "");
                        shown++;
                    }
                }
                if (!shown) printf("   （还没有）\n");
                continue;
            }
            case CMD_WHY: {
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { printf("[用法] ::why <主语> <关系词> <宾语>\n"); continue; }
                char *dup = (char *)malloc(strlen(arg) + 1);
                if (!dup) continue;
                memcpy(dup, arg, strlen(arg) + 1);
                char *w[3] = {NULL, NULL, NULL}; size_t nw = 0;
                for (char *p2 = dup; *p2 && nw < 3; ) {
                    while (*p2 == ' ') p2++;
                    if (!*p2) break;
                    w[nw++] = p2;
                    while (*p2 && *p2 != ' ') p2++;
                    if (*p2) *p2++ = 0;
                }
                const RelDef *rd = (nw >= 2) ? rel_by_name(w[1]) : NULL;
                if (nw < 3 || !rd) { printf("[用法] ::why <主语> <关系词> <宾语>（关系词要在清单里）\n"); free(dup); continue; }
                Token *A = pool_find(a->brain->pool, w[0]);
                Token *B = pool_find(a->brain->pool, w[2]);
                if (A && B) {          /* 依据的**租约状态**（过期不删记录，只是说出来） */
                    long lei = micro_edge_index(A, B);
                    if (lei >= 0)
                        printf("[依据租约] %s 与 %s 之间那条：%s\n", w[0], w[2],
                               ai_lease_name(ai_edge_lease(a->brain, &A->assocs[lei])));
                }
                if (!A || !B) { printf("[词不在图里]\n"); free(dup); continue; }
                Proof pf;
                rel_solve(a->brain, A, rd->id, B, 0, &pf);
                if (pf.status == 0) rel_solve(a->brain, A, rd->id, B, 1, &pf);
                char line[320];
                if (pf.status != 0) {
                    proof_text(a->brain, &pf, line, sizeof line);
                    printf("[问] %s %s %s ？ → %s（%s）\n", w[0], w[1], w[2],
                           pf.status > 0 ? "真" : "假",
                           pf.n > 2 ? "多跳推导" : "直接事实");
                    printf("  推导: %s\n", line);
                } else if (rel_guess(a->brain, A, rd->id, B, &pf)) {
                    proof_text(a->brain, &pf, line, sizeof line);
                    printf("[问] %s %s %s ？ → **推测**（知识库里查不到）\n", w[0], w[1], w[2]);
                    printf("  依据: %s\n", line);
                } else {
                    printf("[问] %s %s %s ？ → 不知道（既推不出真也推不出假）\n",
                           w[0], w[1], w[2]);
                }
                free(dup);
                continue;
            }
            case CMD_REVISE: {
                /* 观测改口语义：同一对词、同一条关系上，后说的观测替掉先说的对立极性。
                 * 默认开；off 是 A/B 用的对照（回到「两个极性并存 → 报矛盾」的旧语义）。 */
                const char *arg = (cmd[6] == ' ') ? cmd + 7 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) g_obs_revise = 1;
                    else if (strcmp(arg, "off") == 0) g_obs_revise = 0;
                    else { printf("[用法] ::revise on|off\n"); continue; }
                }
                printf("[改口语义 = %s]（后说的观测替掉同一条边上先说的对立极性）\n",
                       g_obs_revise ? "开（默认）" : "关（旧语义：两个极性并存）");
                printf("  本次会话改口 %zu 次；关掉之后同一段对话会报「互相矛盾的依据」。\n",
                       a->brain->n_revised);
                /* **修正的记忆（来源监控）**：改口时落下的 [修正:…] 标记节点。
                 * 它是留痕：说得出「这件事被改过口」，而**不是**知识
                 * （不进关系表、不参与走边）——旧观测也不会因此复活。 */
                size_t nrev = 0;
                printf("  修正的记忆（[修正:…] 标记节点，改口时经受审通道落下）：\n");
                for (size_t i = 0; i < a->brain->pool->count; ++i) {
                    Token *t = a->brain->pool->tokens[i];
                    if (!t || t->is_garbage || !t->name) continue;
                    if (strncmp(t->name, MICRO_REVISE_TOKEN_PREFIX,
                                strlen(MICRO_REVISE_TOKEN_PREFIX)) != 0) continue;
                    nrev++;
                    const char *back = "-";
                    for (size_t j = 0; j < t->count; ++j)
                        if (t->assocs[j].main && t->assocs[j].main->name) {
                            back = t->assocs[j].main->name;
                            break;
                        }
                    printf("    %s  ←  %s\n", t->name, back);
                }
                if (nrev == 0) printf("    （还没有：这一会话里没发生过改口）\n");
                printf("  临时 Token（草稿）：%s（::op 看清单 / ::op 回收临时）\n",
                       "[临:…]，不存盘");
                continue;
            }
            case CMD_RELINFER: {
                const char *arg = (cmd[8] == ' ') ? cmd + 9 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "on") == 0) a->brain->rel_off = 0;
                    else if (strcmp(arg, "off") == 0) a->brain->rel_off = 1;
                    else { printf("[用法] ::relinfer on|off\n"); continue; }
                }
                printf("[关系层 = %s]（现场学到 %zu 条，推理答是 %zu / 答不是 %zu，多跳 %zu）\n",
                       a->brain->rel_off ? "关（不推理、也不现场学知识）"
                                         : "开（默认：类型化关系 + 传递推理 + 对话中现学）",
                       a->brain->n_rel_learned, a->brain->n_rel_yes,
                       a->brain->n_rel_no, a->brain->n_rel_deep);
                continue;
            }
            case CMD_VIEW: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && strcmp(arg, "clear") == 0) {
                    a->brain->view_who = NULL;
                    printf("[回到我自己的视角]\n");
                    continue;
                }
                if (arg && *arg) {
                    a->brain->view_who = pool_token_intern(a->brain->pool, arg);
                    printf("[已站到「%s」的视角]（我自己的假设他看不见）\n", arg);
                } else {
                    printf("[当前视角 = %s]\n",
                           a->brain->view_who && a->brain->view_who->name
                               ? a->brain->view_who->name : "我自己");
                    printf("  说「他 觉得 <问题>」也能临时换过去（只那一轮有效）\n");
                }
                continue;
            }
            case CMD_WORLD: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (strcmp(arg, "open") == 0) a->brain->closed_world = 0;
                    else if (strcmp(arg, "closed") == 0) a->brain->closed_world = 1;
                    else { printf("[用法] ::world open|closed\n"); continue; }
                }
                printf("[世界假设 = %s]\n", a->brain->closed_world
                       ? "闭合（知识库里没写就算假 —— **这是一个假设**）"
                       : "开放（知识库里没写就是不知道，默认）");
                printf("  开放：推不出否定时说「不知道」；否定靠**公理**（比如「属于」独占）。\n");
                printf("  闭合：「没写就是假」，答得快但会自信地说错。\n");
                continue;
            }
            case CMD_DERIVEDUMP: {
                AI *B = a->brain;
                for (size_t i = 0; i < B->pool->count; ++i) {
                    Token *t = B->pool->tokens[i];
                    if (!t || !t->name) continue;
                    uint64_t h1 = 1469598103934665603ull, h2 = h1, h3 = h1;
                    for (int k = 0; k < VSA_DIM; ++k) {
                        if (t->hv)      { h1 = (h1 ^ (uint8_t)t->hv[k])      * 1099511628211ull; }
                        if (t->hv_prev) { h2 = (h2 ^ (uint8_t)t->hv_prev[k]) * 1099511628211ull; }
                        if (t->emb)     { h3 = (h3 ^ (uint8_t)t->emb[k])     * 1099511628211ull; }
                    }
                    uint32_t ib = 0; float iv = 0.0f;
                    if (B->idf && i < B->pool->count) { iv = B->idf[i]; memcpy(&ib, &iv, 4); }
                    printf("D %u %s hv=%016llx hvp=%016llx emb=%016llx idf=%.6g/%08x\n",
                           t->id, t->name, (unsigned long long)h1, (unsigned long long)h2,
                           (unsigned long long)h3, (double)iv, ib);
                }
                for (size_t q = 0; q < AI_MAX_SLOTS; ++q) {
                    if (!B->slots[q].n) continue;
                    printf("S %zu sig=%u n=%zu :", q, B->slots[q].sig, B->slots[q].n);
                    for (size_t m = 0; m < B->slots[q].n; ++m)
                        printf(" %u", B->slots[q].members[m] ? B->slots[q].members[m]->id : 0u);
                    printf("\n");
                }
                for (size_t d = 0; d < B->dynrel_n; ++d)
                    printf("R %zu %s\n", d, B->dynrel_name[d] ? B->dynrel_name[d] : "(null)");
                for (size_t t = 0; t < AI_MAX_TOPICS; ++t) {
                    if (!B->topics[t].n && !B->topics[t].votes) continue;
                    printf("P %zu n=%zu votes=%u :", t, B->topics[t].n, B->topics[t].votes);
                    for (size_t k = 0; k < B->topics[t].n; ++k)
                        printf(" %u", B->topics[t].seen[k] ? B->topics[t].seen[k]->id : 0u);
                    printf("\n");
                }
                for (size_t q = 0; q < B->pairs_n; ++q) {
                    const AISentPair *pr = &B->pairs[q];
                    printf("Q %zu t=%u in:", q, (unsigned)pr->topic);
                    for (size_t k = 0; k < pr->in_n; ++k)
                        printf(" %u", pr->in[k] ? pr->in[k]->id : 0u);
                    printf(" out:");
                    for (size_t k = 0; k < pr->out_n; ++k)
                        printf(" %u", pr->out[k] ? pr->out[k]->id : 0u);
                    printf("\n");
                }
                for (size_t i = 0; i < B->pool->count; ++i) {
                    Token *t = B->pool->tokens[i];
                    if (!t || !t->name) continue;
                    printf("N %u cap=%zu parent=%d\n", t->id, t->cap,
                           t->parent ? (int)t->parent->id : -1);
                }
                printf("Z self=%d good=%d bad=%d unk=%d causal=%d sgood=%d sbad=%d pself=%d"
                       " fself=%d split=%d expr:",
                       B->self ? (int)B->self->id : -1, B->good ? (int)B->good->id : -1,
                       B->bad ? (int)B->bad->id : -1,
                       B->unknowing ? (int)B->unknowing->id : -1,
                       B->causal ? (int)B->causal->id : -1,
                       B->social_good ? (int)B->social_good->id : -1,
                       B->social_bad ? (int)B->social_bad->id : -1,
                       B->past_self ? (int)B->past_self->id : -1,
                       B->future_self ? (int)B->future_self->id : -1,
                       B->split_tok ? (int)B->split_tok->id : -1);
                for (int q = 0; q < EXPR_COUNT; ++q)
                    printf(" %d", B->expr_root[q] ? (int)B->expr_root[q]->id : -1);
                printf("\n");
                for (size_t i = 0; i < B->others_n; ++i) {
                    const OtherModel *om = &B->others[i];
                    printf("O %zu who=%d trust=%.4f inter=%zu nbel=%zu nlearn=%zu :",
                           i, om->who ? (int)om->who->id : -1, (double)om->trust,
                           om->n_interactions, om->n_bel, om->n_bel_learned);
                    for (size_t k = 0; k < om->n_bel; ++k)
                        printf(" %d/%u/%d/%u",
                               om->bel[k].a ? (int)om->bel[k].a->id : -1,
                               (unsigned)om->bel[k].rid,
                               om->bel[k].b ? (int)om->bel[k].b->id : -1,
                               (unsigned)om->bel[k].neg);
                    printf("\n");
                }
                printf("Y pool count=%zu cap=%zu nbuck=%zu nent=%zu tick=%llu rng=%u gc=%u"
                       " trace=%zu cov=%d ratio=%d L1=%zu L2=%zu topic=%u focus=%zu cand=%d"
                       " energy=%.3f turns=%zu skipped=%zu meta=%.3f comfort=%.3f arousal=%.3f"
                       " fatigue=%.3f walks=%zu pairs=%zu skipcap=%zu\n",
                       B->pool->count, B->pool->cap, B->pool->n_buckets, B->pool->n_entries,
                       (unsigned long long)B->pool->current_tick, B->rng, B->gc_rng,
                       B->trace_n, B->last_retrieval_cov, B->last_token_ratio,
                       B->last_input_len, B->last_retrieval_len, B->cur_topic,
                       B->focus_n, B->n_candidates, (double)B->energy,
                       B->turns, B->last_skipped_n, (double)B->meta.total,
                       (double)B->inner.comfort, (double)B->inner.arousal,
                       (double)B->inner.fatigue, B->n_walks, B->pairs_n,
                       B->last_skipped_cap);
                printf("X slots_n=%zu n_frames=%zu n_rel_excl=%zu", B->slots_n, B->n_frames,
                       B->n_rel_excl_learned);
                for (size_t r = 0; r <= REL_TABLE_MAX; ++r) printf(" %u", (unsigned)B->rel_excl[r]);
                printf("\n");
                continue;
            }
            case CMD_WTRACE: {
                g_walk_trace = !g_walk_trace;
                printf("[游走跟踪 = %s]\n", g_walk_trace ? "开" : "关");
                continue;
            }
            case CMD_POOLDUMP: {
                TokenPool *P = a->brain->pool;
                for (size_t i = 0; i < P->count; ++i) {
                    Token *t = P->tokens[i];
                    if (!t || !t->name) continue;
                    printf("T %u %s cnt=%zu nseq=%u miss=%u rmiss=%u cat=%u garb=%u mem=%u b=%llu tt=%llu\n",
                           t->id, t->name, t->count, t->next_seq, t->misses, t->recent_misses,
                           (unsigned)t->is_category, (unsigned)t->is_garbage,
                           (unsigned)t->mem_state,
                           (unsigned long long)t->birth_tick, (unsigned long long)t->last_touch_tick);
                    for (size_t j = 0; j < t->count; ++j) {
                        const AssocEntry *e = &t->assocs[j];
                        printf("  E %d h=%u s=%u mem=%u role=%u top=%u pat=%u rel=%u nrel=%u dyn=%u b=%llu tt=%llu act=%d ns=%u nn=%u",
                               e->main ? (int)e->main->id : -1, e->hits, e->seq,
                               (unsigned)e->mem_state, (unsigned)e->role, (unsigned)e->topic,
                               (unsigned)e->pattern, (unsigned)e->rel, (unsigned)e->nrel,
                               (unsigned)e->dyn,
                               (unsigned long long)e->birth_tick, (unsigned long long)e->last_touch_tick,
                               e->action ? (int)e->action->id : -1,
                               (unsigned)e->n_subs, (unsigned)e->n_not);
                        for (unsigned k = 0; k < e->n_subs; ++k)
                            printf(" s%u", e->subs[k] ? e->subs[k]->id : 0u);
                        for (unsigned k = 0; k < e->n_not; ++k)
                            printf(" n%u", e->subs_not[k] ? e->subs_not[k]->id : 0u);
                        printf("\n");
                    }
                }
                continue;
            }
            case CMD_HYPO: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && strcmp(arg, "clear") == 0) {
                    hypo_clear(&a->brain->hypo);
                    printf("[假设已撤回，回到原本的知识]\n");
                    continue;
                }
                if (arg && strcmp(arg, "drop") == 0) {
                    if (hypo_pop(&a->brain->hypo)) printf("[弹掉最深一层假设]\n");
                    else printf("[没有可撤的假设]\n");
                    continue;
                }
                if (arg && strncmp(arg, "drop ", 5) == 0) {
                    /* ::hypo drop N —— 抽掉**指定**那一层（作用域回溯，不必从最深弹） */
                    int idx = atoi(arg + 5);
                    if (idx < 1 || (size_t)idx > a->brain->hypo.depth) {
                        printf("[用法] ::hypo drop <层号，从 1 起，当前共 %zu 层>\n",
                               a->brain->hypo.depth);
                        continue;
                    }
                    if (hypo_drop(&a->brain->hypo, (size_t)idx - 1))
                        printf("[抽掉了第 %d 层，剩下的层整体下移]\n", idx);
                    continue;
                }
                if (a->brain->hypo.depth == 0) {
                    printf("[当前没有假设]（说「如果 猫 会 飞」就可以进假设世界）\n");
                } else {
                    printf("[当前假设世界：%zu 层]\n", a->brain->hypo.depth);
                    for (size_t k = 0; k < a->brain->hypo.depth; ++k) {
                        const HypoFrame *fr = &a->brain->hypo.fr[k];
                        printf("   第 %zu 层%s：\n", k + 1,
                               (fr->who && fr->who->name) ? "（这一层算在「他」头上）" : "");
                        for (size_t i = 0; i < fr->n; ++i) {
                            const HypoFact *f = &fr->f[i];
                            printf("      %s%s %s %s%s %s\n",
                                   (fr->who && fr->who->name) ? fr->who->name : "",
                                   (fr->who && fr->who->name) ? " 以为：" : "",
                                   f->a && f->a->name ? f->a->name : "?",
                                   f->neg ? "不 " : "",
                                   rel_name_of(a->brain, f->rid),
                                   f->b && f->b->name ? f->b->name : "?");
                        }
                    }
                    printf("   （这些**只在推理时用**，没有写进知识库，也不会存盘）\n");
                }
                continue;
            }
            case CMD_DER: {
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                int st = a->brain->der_style;
                if (arg && *arg) {
                    /* ::der <0-3> = 顺手换个风格再看一遍（等价于 ::style N） */
                    int v = atoi(arg);
                    if (v >= 0 && v <= 3) st = v;
                }
                /* 允许 `::der A 关系 B`：现场构造一条链来看（不改任何东西） */
                char *dup = NULL;
                if (arg && *arg && !isdigit((unsigned char)*arg)) {
                    dup = (char *)malloc(strlen(arg) + 1);
                    if (dup) {
                        memcpy(dup, arg, strlen(arg) + 1);
                        char *w[3] = {NULL, NULL, NULL}; size_t nw = 0;
                        for (char *p2 = dup; *p2 && nw < 3; ) {
                            while (*p2 == ' ') p2++;
                            if (!*p2) break;
                            w[nw++] = p2;
                            while (*p2 && *p2 != ' ') p2++;
                            if (*p2) *p2++ = 0;
                        }
                        const RelDef *rd = (nw >= 2) ? rel_by_name(w[1]) : NULL;
                        Token *A = (nw >= 1) ? pool_find(a->brain->pool, w[0]) : NULL;
                        Token *B = (nw >= 3) ? pool_find(a->brain->pool, w[2]) : NULL;
                        if (nw == 3 && rd && A && B) {
                            Derivation dd;
                            Proof pp;
                            der_reset(&dd);
                            rel_solve(a->brain, A, rd->id, B, 0, &pp);
                            if (pp.status == 0) rel_solve(a->brain, A, rd->id, B, 1, &pp);
                            if (pp.status != 0) {
                                der_from_proof(a->brain, &dd, A, rd->id, B, &pp);
                                dd.has_claim = 1;
                                der_conclude(&dd, pp.status, 85);
                            } else {
                                dd.has_claim = 1;
                                der_goal(&dd, A, rd->id, B);
                                der_gap(&dd, "知识库里推不出真也推不出假");
                                der_conclude(&dd, 0, 0);
                            }
                            der_verify(a->brain, &dd);
                            der_dump(a->brain, &dd, st, stdout);
                        } else {
                            printf("[用法] ::der [0-3]   或   ::der <主语> <关系词> <宾语>\n");
                        }
                        free(dup);
                        continue;
                    }
                }
                der_dump(a->brain, &a->brain->last_der, st, stdout);
                continue;
            }
            case CMD_STYLE: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    int v = atoi(arg);
                    if (v < 0 || v > 3) { printf("[用法] ::style 0|1|2|3\n"); continue; }
                    a->brain->der_style = v;
                }
                printf("[输出风格 = %d：%s]\n", a->brain->der_style,
                       der_style_name(a->brain->der_style));
                printf("  0 = 只给结论   1 = 结论 + 链（默认）   2 = 只给链   3 = 符号化\n");
                printf("  （同一份链，换种说法而已 —— 语言不是重点，链才是）\n");
                if (a->brain->last_der.n) {
                    char line[400];
                    der_say(a->brain, &a->brain->last_der, a->brain->der_style,
                            line, sizeof line);
                    printf("  上一轮这句话现在这么说：%s\n", line);
                }
                continue;
            }
            case CMD_NEAR: {
                const char *arg = (cmd[4] == ' ') ? cmd + 5 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { printf("[用法] ::near <词>\n"); continue; }
                Token *t = pool_token_intern(a->brain->pool, arg);
                if (!t) { printf("[取不到这个词]\n"); continue; }
                int nz = 0;
                if (t->emb) for (size_t d = 0; d < VSA_DIM; ++d) if (t->emb[d]) { nz = 1; break; }
                if (!nz) {
                    printf("[%s] 还没有分布式表示（图里没有它的边）\n", arg);
                    continue;
                }
                /* 一趟扫完取前 8（榜尾比对，规模小，不做堆） */
                Token *top[8] = {0}; float sc[8] = {0};
                for (size_t i = 0; i < a->brain->pool->count; ++i) {
                    Token *u = a->brain->pool->tokens[i];
                    if (!u->emb || u->is_garbage || u == t) continue;
                    if (!u->name || u->name[0] == '[') continue;
                    float v = emb_cosine(t->emb, u->emb);
                    for (int k = 0; k < 8; ++k) {
                        if (!top[k] || v > sc[k]) {
                            for (int m = 7; m > k; --m) { top[m] = top[m-1]; sc[m] = sc[m-1]; }
                            top[k] = u; sc[k] = v;
                            break;
                        }
                    }
                }
                printf("[%s] 的语义邻居（emb 余弦，不要求有直接边）:\n", arg);
                for (int k = 0; k < 8 && top[k]; ++k)
                    printf("   %-8s %5.1f%%\n", top[k]->name, 100.0 * (double)sc[k]);
                continue;
            }
            case CMD_ANALOGY: {
                const char *arg = (cmd[7] == ' ') ? cmd + 8 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { printf("[用法] ::analogy <A> <B> <C>   （A:B :: C:?）\n"); continue; }
                char *dup = (char *)malloc(strlen(arg) + 1);
                if (!dup) continue;
                memcpy(dup, arg, strlen(arg) + 1);
                char *w[3] = {NULL, NULL, NULL};
                size_t nw = 0;
                for (char *p2 = dup; *p2 && nw < 3; ) {
                    while (*p2 == ' ') p2++;
                    if (!*p2) break;
                    w[nw++] = p2;
                    while (*p2 && *p2 != ' ') p2++;
                    if (*p2) *p2++ = 0;
                }
                if (nw < 3) { printf("[用法] ::analogy <A> <B> <C>\n"); free(dup); continue; }
                Token *A = pool_find(a->brain->pool, w[0]);
                Token *B = pool_find(a->brain->pool, w[1]);
                Token *C = pool_find(a->brain->pool, w[2]);
                if (!A || !B || !C) {
                    printf("[词不在图里] %s%s%s\n", A ? "" : w[0],
                           B ? "" : w[1], C ? "" : w[2]);
                    free(dup); continue;
                }
                float sim = 0;
                Token *r = ai_analogy(a->brain, A, B, C, &sim);
                /* 门槛 0.5：低于它就说明**这个图的分布式表示还撑不起关系类比** ——
                 * 与其报一个像「猫:动物::狗:? → 火」这种自信的错答案，
                 * 不如直说推不出来。实测当前 model.bin 会走这条分支：
                 *   cats 例子只有 30%，说明向量里还没有「类别偏移」这种结构。
                 * 这个门槛同时是一个**测量仪器**：哪天它能过了，就说明
                 * 语料/表示到位了（见 docs 第二十七节）。 */
                if (r && sim >= 0.5f)
                    printf("[类比] %s : %s :: %s : ?  →  %s (%.1f%%)\n",
                           w[0], w[1], w[2], r->name, 100.0 * (double)sim);
                else
                    printf("[类比] %s : %s :: %s : ?  →  推不出来"
                           "（最高只有 %s %.1f%%，低于门槛 50%%）\n",
                           w[0], w[1], w[2], r ? r->name : "—", 100.0 * (double)sim);
                free(dup);
                continue;
            }
            case CMD_GEN: {
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                cmd_gen(a->brain, arg);
                continue;
            }
            case CMD_THINK: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                cmd_think(a->brain, arg);
                continue;
            }
            case CMD_TOKENS: {
                size_t top = 20;
                const char *arg = (cmd[6] == ' ') ? cmd + 7 : NULL;
                if (arg) { long v = strtol(arg, NULL, 10); if (v > 0 && v < 100000) top = (size_t)v; }
                cmd_tokens(a->brain, top);
                continue;
            }
            case CMD_DRIVE: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (!strcmp(arg, "on"))       a->brain->affect_on = 1;
                    else if (!strcmp(arg, "off")) a->brain->affect_on = 0;
                    else { printf("[用法] ::drive [on|off]\n"); continue; }
                }
                affect_policy_step(a->brain);
                printf("[情绪参与决策 = %s]\n", a->brain->affect_on ? "开" : "关（默认）");
                printf("  当前驱动：舒适=%.2f 唤醒=%.2f 疲劳=%.2f\n",
                       a->brain->inner.comfort, a->brain->inner.arousal, a->brain->inner.fatigue);
                printf("  耦合表（驱动 → 参数 → 生效条件 → 增减）：\n");
                for (size_t i = 0; i < AI_AFFECT_N; ++i) {
                    const AffectCoupling *c = &AI_AFFECT[i];
                    float v = ai_drive_value(a->brain, c->drive);
                    int active = (v >= c->lo && v <= c->hi);
                    printf("    %-4s → %-6s  %.2f..%.2f  %+d  %s%s\n",
                           c->drive, c->param, c->lo, c->hi, c->delta, c->note,
                           active ? "  ← 现在命中" : "");
                }
                printf("  这一轮生效：思考步数 %+d、生成长度 %+d、激活阈值 %+.2f\n",
                       a->brain->affect_d_steps, a->brain->affect_d_gen,
                       (double)a->brain->affect_d_cutoff);
                printf("  没打开时这些修正**一律不施加**（行为与从前逐字节相同）\n");
                continue;
            }
            case CMD_ORDER: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                char w1[48] = {0}, w2[48] = {0};
                if (!arg || sscanf(arg, "%47s %47s", w1, w2) != 2) {
                    printf("[用法] ::order <A> <B>（谁先出现）\n");
                    continue;
                }
                const Token *A = find_token_const(a->brain->pool, w1);
                const Token *B = find_token_const(a->brain->pool, w2);
                if (!A || !B) { printf("[找不到词：%s %s]\n", w1, w2); continue; }
                if (A == B) { printf("[%s 就是它自己，没有先后]\n", w1); continue; }
                /* 顺序不再只看"首次见到"一个点：给出**区间**（第一次 / 最近一次被碰）——
                 * 只看首次的话，"早就见过但最近一直没动"和"一直在用"看起来一样。 */
                printf("[顺序] %s：首次见到 tick=%llu，最近一次 tick=%llu；"
                       "%s：首次见到 tick=%llu，最近一次 tick=%llu\n",
                       w1, (unsigned long long)A->birth_tick,
                       (unsigned long long)A->last_touch_tick,
                       w2, (unsigned long long)B->birth_tick,
                       (unsigned long long)B->last_touch_tick);
                if (A->birth_tick == B->birth_tick)
                    printf("  → 同一轮里出现的（先后看不出来：tick 一样）\n");
                else
                    printf("  → **%s 先**（早 %llu tick）\n",
                           A->birth_tick < B->birth_tick ? w1 : w2,
                           (unsigned long long)(A->birth_tick < B->birth_tick
                                                ? B->birth_tick - A->birth_tick
                                                : A->birth_tick - B->birth_tick));
                /* 顺序证据：最早把两个词连在一起的 [时刻:] 标记 */
                {
                    unsigned long long best = 0;
                    int found = 0;
                    for (size_t i = 0; i < a->brain->pool->count; ++i) {
                        const Token *m = a->brain->pool->tokens[i];
                        if (!m || !m->name || strncmp(m->name, "[时刻:", 9) != 0) continue;
                        int ha = 0, hb = 0;
                        for (size_t j = 0; j < m->count; ++j) {
                            if (m->assocs[j].main == A) ha = 1;
                            if (m->assocs[j].main == B) hb = 1;
                        }
                        if (!(ha && hb)) continue;
                        unsigned long long n = strtoull(m->name + 9, NULL, 10);
                        if (!found || n < best) { best = n; found = 1; }
                    }
                    if (found)
                        printf("  证据：最早同时连上它们的时刻标记是 [时刻:%llu]\n", best);
                    else
                        printf("  证据：图里没有同时连上它们的 [时刻:] 标记（这次只看首次见到的时间）\n");
                }
                continue;
            }
            case CMD_WGATE: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (!strcmp(arg, "on"))       a->brain->write_gate_off = 0;
                    else if (!strcmp(arg, "off")) a->brain->write_gate_off = 1;
                    else { printf("[用法] ::wgate on|off\n"); continue; }
                }
                printf("[写入分级闸门 = %s]\n", a->brain->write_gate_off ? "关（默认）" : "开");
                printf("  评分：0.30·生词 + 0.25·来源后验 + 0.20·提及 + 0.15·惊讶 + 0.10·不累\n");
                printf("  分层：≥%.2f 长期 / ≥%.2f 知识 / 更低 = 情境层%s\n",
                       AI_WGATE_LONGTERM, AI_WGATE_KNOWLEDGE,
                       a->brain->write_gate_off ? "（关着时照写）" : "（**不落库**）");
                if (a->brain->last_write_imp < 0.0)
                    printf("  最近一次关系写入的重要度：（还没写过）\n");
                else
                    printf("  最近一次关系写入的重要度：%.2f（%s）\n",
                           a->brain->last_write_imp, ai_write_layer(a->brain->last_write_imp));
                continue;
            }
            case CMD_PREMHOLD: {
                const char *arg = (cmd[8] == ' ') ? cmd + 9 : NULL;
                while (arg && *arg == ' ') arg++;
                if (arg && *arg) {
                    if (!strcmp(arg, "verified"))        a->brain->prem_hold_activate = 2;
                    else if (!strcmp(arg, "activate"))   a->brain->prem_hold_activate = 1;
                    else if (!strcmp(arg, "mention"))    a->brain->prem_hold_activate = 0;
                    else { printf("[用法] ::premhold mention|activate\n"); continue; }
                }
                printf("[前提成立判据 = %s]\n", a->brain->prem_hold_activate == 0
                       ? "mention（默认：只在注意窗口里算成立）"
                       : (a->brain->prem_hold_activate == 1
                          ? "activate（窗口里没有时，图上够得着也算）"
                          : "verified（够得着**且前提本身有据**才算）"));
                printf("  activate 档用的是**受限扩散**（深度≤2、能量≥0.25、从权重≥0.3 的焦点词出发）\n");
                printf("  前提本身**被提到**永远算成立；这一档只回答「没提到，但它成立吗」\n");
                continue;
            }
            case CMD_TRUST: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                Think *t = think_of(a->brain);
                if (arg && *arg) {
                    if (!strcmp(arg, "on"))       t->trust_on = 1;
                    else if (!strcmp(arg, "off")) t->trust_on = 0;
                    else { printf("[用法] ::trust on|off\n"); continue; }
                }
                printf("[可信度参与判定 = %s]\n", t->trust_on ? "开" : "关（默认）");
                printf("  可信度是**从已有字段算的**（来源档 + 命中 + 新近），不是存下来的字段；\n");
                printf("  %s\n", t->trust_on
                       ? "两边差 ≥20 时按更可信的那边答（会改行为 —— 已 A/B）"
                       : "只在 ::src / ::delib 里说清「我更信哪一条」，不改判定");
                continue;
            }
            case CMD_TRIGGERS: {
                dialog_trigger_list();
                continue;
            }
            case CMD_SRC: {
                /* **来源监控（谁说的）**：一条知识摆出来，先问它是哪来的。
                 * 五档分得开：我的图（基准/观测/推论）、推出来的（求解器 + 证据档）、
                 * 他说的（他的表，不进我的图）、假设层（只在提问里成立）。 */
                const char *arg = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (arg && *arg == ' ') arg++;
                char w1[48] = {0}, w2[48] = {0}, w3[48] = {0};
                if (!arg || sscanf(arg, "%47s %47s %47s", w1, w2, w3) != 3) {
                    printf("[用法] ::src <A> <关系> <B>（例：::src 猫 会 飞）\n");
                    continue;
                }
                Token *A = pool_find(a->brain->pool, w1);
                Token *B = pool_find(a->brain->pool, w3);
                if (!A || !B) { printf("[找不到词：%s %s]\n", w1, w3); continue; }
                uint8_t rid = rel_id_of(a->brain, w2, 0);   /* 只认表里已有的谓语（读操作不改表） */
                if (!rid) { printf("[不认识的谓语：%s]\n", w2); continue; }
                printf("[来源账] %s %s %s\n", w1, w2, w3);
                /* ① 我的图里这条边在不在、是哪一档、命中多少 */
                long ei = micro_edge_index(A, B);
                if (ei >= 0) {
                    const AssocEntry *e = &A->assocs[ei];
                    int pos = (e->rel & REL_BIT(rid)) != 0;
                    int negb = (e->nrel & REL_BIT(rid)) != 0;
                    printf("  我的图：%s（来源=%s，命中=%u，首次 seq=%u，**可信度=%d**）\n",
                           pos ? "是" : (negb ? "不是" : "有连结但没这个关系"),
                           assoc_role_name(e->role), e->hits, e->seq,
                           ai_credibility(a->brain, e));
                } else {
                    printf("  我的图：没有这条连结\n");
                }
                /* ② 推得出来吗（求解器 + 链上的证据档） */
                {
                    Proof pf2;
                    rel_solve(a->brain, A, rid, B, 0, &pf2);
                    char ch[256];
                    proof_text(a->brain, &pf2, ch, sizeof ch);
                    if (pf2.status == 0) {
                        printf("  推得出来吗：推不出来\n");
                    } else {
                        Derivation d2;
                        der_reset(&d2);
                        der_from_proof(a->brain, &d2, A, rid, B, &pf2);
                        int gaps2 = der_verify(a->brain, &d2);
                        int cls2 = think_evidence_class(&d2);
                        int cache2 = think_evidence_is_cache(&d2);
                        printf("  推得出来吗：%s（依据档 %s%s；验证器 %s）\n",
                               pf2.status > 0 ? "成立" : "不成立",
                               think_evidence_name(cls2, cache2),
                               cache2 ? "·推论缓存" : "",
                               gaps2 ? "有缺口" : "通过");
                        if (ch[0]) printf("      链：%s\n", ch);
                    }
                }
                /* ③ 他说的（他的表里有没有、跟我同向还是相反） */
                size_t shown = 0;
                int my_pos = rel_direct(A, rid, B), my_neg = rel_direct_neg(A, rid, B);
                for (size_t i = 0; i < a->brain->others_n; ++i) {
                    const OtherModel *om = &a->brain->others[i];
                    const HypoFact *f = other_belief_find(om, A, rid, B);
                    if (!f) continue;
                    int his_pos = !f->neg;
                    const char *cmp = (my_pos == 0 && my_neg == 0) ? "（我这边没表过态）"
                                    : ((his_pos && my_neg) || (!his_pos && my_pos))
                                        ? " —— **和我的图相反**" : " —— 和我的图一致";
                    printf("  他说的：%s 说「%s%s %s」（他的表，不进我的图；**可信度=%d**）%s\n",
                           om->who && om->who->name ? om->who->name : "他",
                           f->neg ? "不 " : "", w2, w3, (int)(om->trust * 100.0f), cmp);
                    shown++;
                }
                if (shown == 0) {
                    /* 精确匹配没有 → 按**类别**再找一遍（`猫` vs `猫科`、`鱼` vs `鱼缸` 这类）。
                     * 只影响**呈现**：不改"他信什么"的判定，也不动我的图。 */
                    for (size_t i = 0; i < a->brain->others_n && shown == 0; ++i) {
                        const OtherModel *om = &a->brain->others[i];
                        for (size_t k = 0; k < om->n_bel; ++k) {
                            const HypoFact *f = &om->bel[k];
                            if (f->rid != rid) continue;
                            int a_ok = (f->a == A) || token_is_descendant(A, f->a) ||
                                       token_is_descendant(f->a, A);
                            int b_ok = (f->b == B) || token_is_descendant(B, f->b) ||
                                       token_is_descendant(f->b, B);
                            if (!a_ok || !b_ok) continue;
                            printf("  他说的：%s 说过「%s %s %s」（**按类别**对上的：%s vs %s）\n",
                                   om->who && om->who->name ? om->who->name : "他",
                                   f->a && f->a->name ? f->a->name : "?",
                                   w2,
                                   f->b && f->b->name ? f->b->name : "?",
                                   w1, w3);
                            shown++;
                            break;
                        }
                    }
                    if (shown == 0) printf("  他说的：没听谁说过这条\n");
                }
                /* ③a 租约（TTL）：这条依据现在算稳定/减弱/过期 —— 降级只改状态，不改记录 */
                if (ei >= 0)
                    printf("  租约：%s（TTL %u tick，弱阈值 可信度<%d；**降级不删记录**）\n",
                           ai_lease_name(ai_edge_lease(a->brain, &A->assocs[ei])),
                           (unsigned)AI_LEASE_TTL_TICKS, AI_LEASE_WEAK_CRED);
                /* ③b **反向也报一句**（来源账要能双向问：A→B 有，B→A 未必） */
                {
                    long ei2 = micro_edge_index(B, A);
                    int r2 = rel_direct(B, rid, A), n2 = rel_direct_neg(B, rid, A);
                    printf("  反向（%s %s %s）：%s\n", w3, w2, w1,
                           (r2 || n2) ? (ei2 >= 0 ? "有（连结 + 关系）" : "有关系、没有共现边")
                                      : (ei2 >= 0 ? "只有共现边、没有这个关系" : "没有"));
                }
                /* ④ 假设层（只在提问里成立的那些）—— **说清在第几层**：
                 * 假设本来就是**一个栈**（`Hypo.fr[]` + depth），"他以为我以为…"是两层；
                 * 以前只报"有/没有"，看不出嵌套关系。 */
                {
                    const HypoFact *hf = hypo_find(&a->brain->hypo,
                                                   a->brain->view_who, A, rid, B, NULL);
                    size_t layer = 0;
                    if (hf) {
                        for (size_t k = a->brain->hypo.depth; k-- > 0; ) {
                            const HypoFrame *fr = &a->brain->hypo.fr[k];
                            if (!hypo_frame_visible(fr, a->brain->view_who)) continue;
                            int hit = 0;
                            for (size_t i = 0; i < fr->n; ++i)
                                if (fr->f[i].a == A && fr->f[i].b == B && fr->f[i].rid == rid) { hit = 1; break; }
                            if (hit) { layer = k + 1; break; }
                        }
                        printf("  假设层：第 %zu 层（共 %zu 层，%s）成立\n",
                               layer, a->brain->hypo.depth, hf->neg ? "否定式" : "肯定式");
                    } else {
                        printf("  假设层：没有（当前 %zu 层）\n", a->brain->hypo.depth);
                    }
                }
                continue;
            }
            case CMD_EDGES: {
                const char *arg = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (arg && *arg == ' ') arg++;
                if (!arg || !*arg) { printf("[用法] ::edges <词>\n"); continue; }
                cmd_edges(a->brain, arg);
                continue;
            }
            case CMD_ASM: {
                const char *src = (cmd[3] == ' ') ? cmd + 4 : NULL;
                while (src && *src == ' ') src++;
                cmd_asm(a->brain, src);
                continue;
            }
            case CMD_LEARN: {
                const char *src = (cmd[5] == ' ') ? cmd + 6 : NULL;
                while (src && *src == ' ') src++;
                if (src && *src) { ai_learn_text(a->brain, src); printf("[学了一句]\n"); }
                continue;
            }
            case CMD_QUIT: {
                /* 退出不自动存盘：要存就显式 ::save。
                 * （自动存盘会在测试/脚本里悄悄覆盖掉别人的 model.bin）
                 * 注意别在这里写 `break` —— 那在 switch 里只跳出一层，
                 * 循环会接着跑（实测：退出后又读一行、还答了一句）。
                 * 所以用标志，由循环自己 break。 */
                printf("[退出（未自动存盘；'::save' 显式保存）]\n");
                want_quit = 1;
                continue;
            }
            /* 走到这儿说明表里有这一条、但 switch 里没有实现 —— 那是 bug，
             * 不是用户输入问题（tests/cmdtable.sh 会逐条打一遍来逮它）。 */
            case CMD_NONE:
            default:
                printf("[命令表里有 %s（%s）但没有实现 —— 这是个 bug，请记下来]\n",
                       cname, cd->name);
                continue;
            }
        }

        char out[4096];
        agent_step(a, input, out, sizeof out);
        stdout_act(out, NULL);
    }

    printf("\n");
    ai_stats(a->brain);
    agent_free(a);
    return 0;
}
