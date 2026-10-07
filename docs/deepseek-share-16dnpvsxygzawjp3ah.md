# DeepSeek 分享对话 — 整理后的完整文件树

> 来源：<https://chat.deepseek.com/share/16dnpvsxygzawjp3ah>
> 分享标题：Shared Conversation ｜ model_type：default ｜ 消息 id：116, 117
> 抓取时间：2026-09-22（分享内容时间戳：2026-09-22 20:05:37 CST）
> 抓取方式：`GET https://chat.deepseek.com/api/v0/share/content?share_id=16dnpvsxygzawjp3ah`
> 说明：网页是 SPA 且对自动化环境返回「使用环境异常」，故直接取官方接口 JSON；
> 原始 JSON 见同目录 `deepseek-share-16dnpvsxygzawjp3ah.json`。
>
> **注意：该分享只包含最后一轮问答（消息 116 用户提问 + 117 助手回复）。**
> 助手回复中「一、token.h」是完整源码，已落盘为仓库根目录的 `token.h`；
> 「二/三/四/五」只有改动说明（其余文件在更早的消息里，不在本分享内）。

---

## 用户提问

然后整理一下所有文件

---

## 助手回复（原文）

# 整理后的完整文件树

```
project/
├── token.h          核心：图、Token、边、记忆状态、持久化
├── token_lang.h     语言：原语、栈机、选择器、前关联、条件、桥
├── ai.h             认知：分词、学习、生成、GC、遗忘
├── agent.h          循环：感知 → 思考 → 行动 → 维护
└── main.c           入口：stdin/stdout，元命令，种子加载
```

依赖方向：`main.c → agent.h → ai.h → token.h`；`token_lang.h → token.h`。

三个头文件都遵循单头文件、`static inline`、零依赖、`#ifndef` 卫哨。序列化格式统一为 **v3**——含 `action_id`，不含 tick/state 字段。

---

## 一、token.h —— 完整最终版

```c
/* token.h — 单头文件关联机 v10
 *
 * 核心概念：
 *   Token        概念（词、类别、角色、动作、容器、原语……）
 *   AssocEntry   从一个 Token 到另一个的有向边，带语境、动作、计数
 *   TokenPool    所有 Token 的容器，含名字哈希和当前 tick
 *
 * 学习：hits++（token_touch_entry），token_consolidate 重排
 * 遗忘：mem_state 概率升级（ACTIVE → FADING → DORMANT → FORGOTTEN）
 * 生成：token_associate_weighted / token_activate
 * 元认知：misses / recent_misses / token_gap / pool_unknowing
 * 持久化：token_pool_dump / token_pool_load（v3 格式）
 *
 * 不含：
 *   - 语言层（token_lang.h）
 *   - AI 循环（ai.h）
 *   - GC 策略（ai.h）
 */
#ifndef TOKEN_H
#define TOKEN_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef TOKEN_CTX_MAX
#define TOKEN_CTX_MAX 4
#endif
#ifndef TOKEN_UNKNOWING_NAME
#define TOKEN_UNKNOWING_NAME "（未知）"
#endif
#ifndef TOKEN_GAP_DEFAULT
#define TOKEN_GAP_DEFAULT 0.5f
#endif
#ifndef TOKEN_HASH_INIT
#define TOKEN_HASH_INIT 16u
#endif

#define TOKEN_MAGIC   0x4D435341u
#define TOKEN_VERSION 3u
#define TOKEN_SEQ_NONE ((uint32_t)-1)
#define TOKEN_ID_NONE  ((uint32_t)-1)

typedef struct Token       Token;
typedef struct TokenPool   TokenPool;
typedef struct AssocEntry  AssocEntry;
typedef struct ActivateCtx ActivateCtx;
typedef struct NameEntry   NameEntry;

/* ============================ 类型 ============================ */

typedef enum {
    MEM_ACTIVE    = 0,
    MEM_FADING    = 1,
    MEM_DORMANT   = 2,
    MEM_FORGOTTEN = 3,
} MemState;

typedef void (*MeaningFree)(void *meaning);

typedef struct {
    size_t      (*dump_meaning)(void *meaning, uint8_t *out, size_t cap);
    void       *(*load_meaning)(const uint8_t *in, size_t len);
    MeaningFree  free_meaning;
} MeaningCodec;

struct AssocEntry {
    Token    *subs[TOKEN_CTX_MAX];
    Token    *subs_not[TOKEN_CTX_MAX];
    uint8_t   n_subs, n_not;
    Token    *main;
    Token    *action;
    uint32_t  hits;
    uint32_t  seq;
    uint64_t  birth_tick;
    uint64_t  last_touch_tick;
    uint8_t   mem_state;
};

struct Token {
    uint32_t    id;
    void       *meaning;
    AssocEntry *assocs;
    size_t      count, cap;
    uint32_t    next_seq;
    uint32_t    misses;
    uint32_t    recent_misses;
    Token      *parent;
    uint8_t     is_category;
    uint8_t     is_garbage;
    uint64_t    birth_tick;
    uint64_t    last_touch_tick;
    uint8_t     mem_state;
    TokenPool  *owner;
    const char *name;
};

struct NameEntry {
    uint32_t   hash;
    uint32_t   id;
    NameEntry *next;
};

struct TokenPool {
    Token     **tokens;
    size_t      count, cap;
    Token      *unknowing;
    MeaningFree meaning_free;
    NameEntry **buckets;
    size_t      n_buckets, n_entries;
    uint64_t    current_tick;
};

/* ============================ 哈希 ============================ */

static inline uint32_t tok_hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static inline int pool_rehash(TokenPool *p, size_t new_size) {
    NameEntry **nb = (NameEntry **)calloc(new_size, sizeof *nb);
    if (!nb) return -1;
    for (size_t i = 0; i < p->n_buckets; ++i) {
        NameEntry *e = p->buckets[i];
        while (e) {
            NameEntry *next = e->next;
            size_t idx = e->hash & (new_size - 1);
            e->next = nb[idx]; nb[idx] = e;
            e = next;
        }
    }
    free(p->buckets);
    p->buckets = nb;
    p->n_buckets = new_size;
    return 0;
}

static inline void pool_hash_insert(TokenPool *p, Token *t) {
    if (!t->name) return;
    uint32_t h = tok_hash_str(t->name);
    size_t idx = h & (p->n_buckets - 1);
    NameEntry *e = (NameEntry *)malloc(sizeof *e);
    if (!e) return;
    e->hash = h; e->id = t->id;
    e->next = p->buckets[idx];
    p->buckets[idx] = e;
    p->n_entries++;
    if (p->n_entries * 4 > p->n_buckets * 3)
        (void)pool_rehash(p, p->n_buckets * 2);
}

/* ============================ 池 ============================ */

static inline TokenPool *pool_new(void) {
    return (TokenPool *)calloc(1, sizeof(TokenPool));
}

static inline void pool_set_meaning_free(TokenPool *p, MeaningFree fn) {
    if (p) p->meaning_free = fn;
}

static inline void pool_free(TokenPool *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (p->meaning_free && t->meaning) p->meaning_free(t->meaning);
        free(t->assocs);
        free((void *)t->name);
        free(t);
    }
    free(p->tokens);
    if (p->buckets) {
        for (size_t i = 0; i < p->n_buckets; ++i) {
            NameEntry *e = p->buckets[i];
            while (e) { NameEntry *n = e->next; free(e); e = n; }
        }
        free(p->buckets);
    }
    free(p);
}

static inline Token *pool_token(TokenPool *p, const char *name, void *meaning) {
    if (!p) return NULL;
    if (p->n_buckets == 0 && pool_rehash(p, TOKEN_HASH_INIT) != 0) return NULL;
    if (p->count == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 8;
        Token **nt = (Token **)realloc(p->tokens, ncap * sizeof *nt);
        if (!nt) return NULL;
        p->tokens = nt; p->cap = ncap;
    }
    Token *t = (Token *)calloc(1, sizeof *t);
    if (!t) return NULL;
    if (name) {
        size_t len = strlen(name);
        char *copy = (char *)malloc(len + 1);
        if (!copy) { free(t); return NULL; }
        memcpy(copy, name, len + 1);
        t->name = copy;
    }
    t->id              = (uint32_t)p->count;
    t->meaning         = meaning;
    t->birth_tick      = p->current_tick;
    t->last_touch_tick = p->current_tick;
    t->mem_state       = MEM_ACTIVE;
    t->owner           = p;
    p->tokens[p->count++] = t;
    if (t->name) pool_hash_insert(p, t);
    return t;
}

static inline Token *pool_token_by_id(const TokenPool *p, uint32_t id) {
    if (!p || id >= p->count) return NULL;
    return p->tokens[id];
}

static inline Token *pool_token_intern(TokenPool *p, const char *name) {
    if (!p || !name) return NULL;
    if (p->n_buckets == 0 && pool_rehash(p, TOKEN_HASH_INIT) != 0) return NULL;
    uint32_t h = tok_hash_str(name);
    size_t idx = h & (p->n_buckets - 1);
    for (NameEntry *e = p->buckets[idx]; e; e = e->next) {
        if (e->hash != h) continue;
        Token *t = p->tokens[e->id];
        if (t->name && strcmp(t->name, name) == 0) {
            if (t->is_garbage) {
                t->is_garbage = 0;
                t->birth_tick = p->current_tick;
                t->last_touch_tick = p->current_tick;
                t->mem_state = MEM_ACTIVE;
                t->misses = 0;
                t->recent_misses = 0;
            }
            return t;
        }
    }
    return pool_token(p, name, NULL);
}

static inline Token *pool_token_child(TokenPool *p, Token *parent,
                                      const char *name, void *meaning) {
    Token *t = pool_token(p, name, meaning);
    if (t) t->parent = parent;
    return t;
}

static inline Token *pool_unknowing(TokenPool *p) {
    if (!p) return NULL;
    if (!p->unknowing)
        p->unknowing = pool_token(p, TOKEN_UNKNOWING_NAME, NULL);
    return p->unknowing;
}

static inline void token_set_category(Token *t, int on) {
    if (t) t->is_category = (uint8_t)(on != 0);
}
static inline void token_set_parent(Token *t, Token *parent) {
    if (t) t->parent = parent;
}
static inline int token_is_descendant(const Token *t, const Token *ancestor) {
    if (!t || !ancestor) return 0;
    while (t) { if (t == ancestor) return 1; t = t->parent; }
    return 0;
}

/* ============================ touch ============================ */

static inline void token_touch(Token *t) {
    if (!t || t->is_garbage) return;
    uint64_t now = t->owner ? t->owner->current_tick : 0;
    t->last_touch_tick = now;
    t->mem_state = MEM_ACTIVE;
}

static inline void token_touch_entry(Token *t, AssocEntry *e) {
    if (!t || !e) return;
    e->hits++;
    uint64_t now = t->owner ? t->owner->current_tick : 0;
    e->last_touch_tick = now;
    e->mem_state = MEM_ACTIVE;
}

/* ============================ 关联编辑 ============================ */

static inline int token_add_assoc_ex(Token *t,
                                     Token **subs, size_t n_subs,
                                     Token **subs_not, size_t n_not,
                                     Token *main, Token *action) {
    if (!t || n_subs > TOKEN_CTX_MAX || n_not > TOKEN_CTX_MAX) return -1;
    if (t->count == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 4;
        AssocEntry *na = (AssocEntry *)realloc(t->assocs, ncap * sizeof *na);
        if (!na) return -1;
        t->assocs = na; t->cap = ncap;
    }
    AssocEntry *e = &t->assocs[t->count];
    e->n_subs = (uint8_t)n_subs;
    e->n_not  = (uint8_t)n_not;
    for (size_t i = 0; i < n_subs; ++i) e->subs[i]     = subs[i];
    for (size_t i = 0; i < n_not;  ++i) e->subs_not[i] = subs_not[i];
    e->main   = main;
    e->action = action;
    e->hits   = 0;
    e->seq    = t->next_seq++;
    e->birth_tick      = t->owner ? t->owner->current_tick : 0;
    e->last_touch_tick = e->birth_tick;
    e->mem_state       = MEM_ACTIVE;
    t->count++;
    return 0;
}

static inline int token_add_assoc(Token *t, Token *sub, Token *main) {
    if (sub) { Token *s[1] = { sub }; return token_add_assoc_ex(t, s, 1, NULL, 0, main, NULL); }
    return token_add_assoc_ex(t, NULL, 0, NULL, 0, main, NULL);
}
static inline int token_add_assoc_multi(Token *t, Token **subs, size_t n_subs, Token *main) {
    return token_add_assoc_ex(t, subs, n_subs, NULL, 0, main, NULL);
}
static inline int token_add_assoc_action(Token *t, Token **subs, size_t n_subs,
                                         Token *main, Token *action) {
    return token_add_assoc_ex(t, subs, n_subs, NULL, 0, main, action);
}
static inline int token_add_assoc_not(Token *t,
                                      Token **subs, size_t n_subs,
                                      Token **subs_not, size_t n_not,
                                      Token *main) {
    return token_add_assoc_ex(t, subs, n_subs, subs_not, n_not, main, NULL);
}

static inline int token_add_assoc_unique(Token *t,
                                         Token **subs, size_t n_subs, Token *main) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        const AssocEntry *e = &t->assocs[i];
        if (e->main != main || e->n_subs != n_subs || e->n_not != 0) continue;
        int same = 1;
        for (size_t k = 0; k < n_subs; ++k)
            if (e->subs[k] != subs[k]) { same = 0; break; }
        if (same) return 0;
    }
    return token_add_assoc_ex(t, subs, n_subs, NULL, 0, main, NULL);
}

static inline int token_link_bidirectional(Token *a, Token *b) {
    if (!a || !b) return -1;
    if (token_add_assoc_unique(a, NULL, 0, b) != 0) return -1;
    if (token_add_assoc_unique(b, NULL, 0, a) != 0) return -1;
    return 0;
}

/* 移除 "在 sub 语境下指向 main" 的那条边。sub==NULL 移除无条件边。 */
static inline int token_remove_assoc(Token *t, Token *sub, Token *main) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (e->main != main) continue;
        int hit = 0;
        if (!sub && e->n_subs == 0 && e->n_not == 0) hit = 1;
        else if (sub && e->n_subs == 1 && e->subs[0] == sub) hit = 1;
        if (hit) {
            memmove(e, e + 1, (t->count - i - 1) * sizeof *e);
            t->count--;
            return 0;
        }
    }
    return -1;
}

static inline int token_set_action(Token *t, uint32_t seq, Token *action) {
    if (!t || seq == TOKEN_SEQ_NONE) return -1;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].seq == seq) { t->assocs[i].action = action; return 0; }
    return -1;
}

static inline int token_set_action_by_target(Token *t, Token *sub, Token *main,
                                             Token *action) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (e->main != main) continue;
        if (!sub && e->n_subs == 0) { e->action = action; return 0; }
        if (sub && e->n_subs == 1 && e->subs[0] == sub) { e->action = action; return 0; }
    }
    return -1;
}

static inline void token_clear_actions(Token *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; ++i) t->assocs[i].action = NULL;
}

/* ============================ 语境匹配 ============================ */

static inline int ctx_satisfies(const Token *need, Token *const *ctx, size_t n) {
    if (!need) return 1;
    for (size_t i = 0; i < n; ++i) {
        if (ctx[i] == need) return 1;
        if (need->is_category && token_is_descendant(ctx[i], need)) return 1;
    }
    return 0;
}
static inline int ctx_hits_excluded(const Token *excl, Token *const *ctx, size_t n) {
    if (!excl) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (ctx[i] == excl) return 1;
        if (excl->is_category && token_is_descendant(ctx[i], excl)) return 1;
    }
    return 0;
}
static inline int entry_matches(const AssocEntry *e,
                                Token *const *ctx, size_t n_ctx) {
    for (uint8_t i = 0; i < e->n_subs; ++i)
        if (!ctx_satisfies(e->subs[i], ctx, n_ctx)) return 0;
    for (uint8_t i = 0; i < e->n_not; ++i)
        if (ctx_hits_excluded(e->subs_not[i], ctx, n_ctx)) return 0;
    return 1;
}

/* ============================ 稳定 handle ============================ */

static inline AssocEntry *token_seq_to_entry(Token *t, uint32_t seq) {
    if (!t || seq == TOKEN_SEQ_NONE) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].seq == seq) return &t->assocs[i];
    return NULL;
}
static inline const AssocEntry *token_seq_to_entry_c(const Token *t, uint32_t seq) {
    if (!t || seq == TOKEN_SEQ_NONE) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].seq == seq) return &t->assocs[i];
    return NULL;
}
static inline Token *token_main_by_seq(const Token *t, uint32_t seq) {
    const AssocEntry *e = token_seq_to_entry_c(t, seq);
    return e ? e->main : NULL;
}
static inline int token_hit_seq(Token *t, uint32_t seq) {
    AssocEntry *e = token_seq_to_entry(t, seq);
    if (!e) return 0;
    token_touch_entry(t, e);
    return 1;
}

/* ============================ 联想 ============================ */

static inline size_t token_reach(const Token *t, float energy) {
    if (!t || t->count == 0) return 0;
    if (energy < 0) energy = 0;
    if (energy > 1) energy = 1;
    size_t reach = (size_t)(energy * (float)t->count) + 1;
    if (reach > t->count) reach = t->count;
    return reach;
}

static inline size_t token_associate_idx(const Token *t,
                                         Token *const *ctx, size_t n_ctx,
                                         float energy) {
    size_t reach = token_reach(t, energy);
    for (size_t i = 0; i < reach; ++i)
        if (entry_matches(&t->assocs[i], ctx, n_ctx)) return i;
    return (size_t)-1;
}

static inline Token *token_associate(const Token *t,
                                     Token *const *ctx, size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    return i == (size_t)-1 ? NULL : t->assocs[i].main;
}
static inline Token *token_associate1(const Token *t, Token *ctx, float energy) {
    if (ctx) { Token *c[1] = { ctx }; return token_associate(t, c, 1, energy); }
    return token_associate(t, NULL, 0, energy);
}
static inline uint32_t token_associate_seq(const Token *t,
                                           Token *const *ctx, size_t n_ctx,
                                           float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    return i == (size_t)-1 ? TOKEN_SEQ_NONE : t->assocs[i].seq;
}
static inline void token_hit(Token *t, size_t i) {
    if (t && i < t->count) token_touch_entry(t, &t->assocs[i]);
}
static inline Token *token_associate_tap(Token *t,
                                         Token *const *ctx, size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    if (i == (size_t)-1) return NULL;
    token_touch_entry(t, &t->assocs[i]);
    return t->assocs[i].main;
}
static inline uint32_t token_associate_tap_seq(Token *t,
                                               Token *const *ctx, size_t n_ctx,
                                               float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    if (i == (size_t)-1) return TOKEN_SEQ_NONE;
    token_touch_entry(t, &t->assocs[i]);
    return t->assocs[i].seq;
}

static inline Token *token_associate_weighted(const Token *t,
                                              Token *const *ctx, size_t n_ctx,
                                              float energy, uint32_t *rng) {
    if (!t || !rng) return NULL;
    if (*rng == 0) *rng = 0x9E3779B9u;
    size_t reach = token_reach(t, energy);
    uint64_t total = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &t->assocs[i];
        if (entry_matches(e, ctx, n_ctx)) total += (uint64_t)e->hits + 1;
    }
    if (total == 0) return NULL;
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x;
    uint64_t pick = (uint64_t)x % total, acc = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &t->assocs[i];
        if (entry_matches(e, ctx, n_ctx)) {
            acc += (uint64_t)e->hits + 1;
            if (pick < acc) return e->main;
        }
    }
    return NULL;
}

/* ============================ 空缺感 ============================ */

static inline uint32_t token_hits_total(const Token *t) {
    if (!t) return 0;
    uint32_t s = 0;
    for (size_t i = 0; i < t->count; ++i) s += t->assocs[i].hits;
    return s;
}
static inline float token_gap(const Token *t) {
    if (!t) return 0;
    uint32_t rm = t->recent_misses;
    return (float)rm / (float)(rm + token_hits_total(t) + 1);
}
static inline Token *token_associate_or_unknown(Token *t,
                                                Token *const *ctx, size_t n_ctx,
                                                float energy, TokenPool *pool,
                                                float threshold) {
    if (!t) return NULL;
    Token *r = token_associate(t, ctx, n_ctx, energy);
    if (r) { t->recent_misses /= 2; return r; }
    t->misses++; t->recent_misses++;
    if (pool && token_gap(t) >= threshold) return pool_unknowing(pool);
    return NULL;
}

/* ============================ 巩固 / 衰减 ============================ */

static inline int token_entry_cmp_desc(const void *a, const void *b) {
    const AssocEntry *ea = (const AssocEntry *)a, *eb = (const AssocEntry *)b;
    if (ea->hits != eb->hits) return ea->hits < eb->hits ? 1 : -1;
    if (ea->seq  != eb->seq)  return ea->seq  < eb->seq  ? -1 : 1;
    return 0;
}
static inline void token_consolidate(Token *t) {
    if (t && t->count > 1)
        qsort(t->assocs, t->count, sizeof *t->assocs, token_entry_cmp_desc);
}
static inline void token_decay_hits(Token *t, float factor) {
    if (!t) return;
    if (factor < 0) factor = 0;
    if (factor > 1) factor = 1;
    for (size_t i = 0; i < t->count; ++i)
        t->assocs[i].hits = (uint32_t)((float)t->assocs[i].hits * factor);
    t->recent_misses = (uint32_t)((float)t->recent_misses * factor);
}

/* ============================ 观察学习 ============================ */

static inline void token_observe_pair(Token *a, Token *b) {
    if (!a || !b || a == b) return;
    for (size_t k = 0; k < a->count; ++k) {
        AssocEntry *e = &a->assocs[k];
        if (e->main == b && e->n_subs == 0 && e->n_not == 0) {
            token_touch_entry(a, e);
            return;
        }
    }
    if (token_add_assoc_ex(a, NULL, 0, NULL, 0, b, NULL) == 0)
        token_touch_entry(a, &a->assocs[a->count - 1]);
}

static inline void token_observe(Token *const *seq, size_t n, size_t window) {
    if (!seq || n < 2) return;
    if (window < 2) window = 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        size_t end = i + window;
        if (end > n) end = n;
        for (size_t j = i + 1; j < end; ++j)
            token_observe_pair(seq[i], seq[j]);
    }
}

static inline void token_observe_names(TokenPool *p,
                                       const char *const *names, size_t n,
                                       size_t window) {
    if (!p || !names || n < 2) return;
    Token **seq = (Token **)malloc(n * sizeof *seq);
    if (!seq) return;
    for (size_t i = 0; i < n; ++i) seq[i] = pool_token_intern(p, names[i]);
    token_observe(seq, n, window);
    free(seq);
}

/* ============================ 递归激活 ============================ */

typedef void (*TokenVisitFn)(Token *t, size_t depth, float energy, void *user);

struct ActivateCtx {
    Token       **stack;
    size_t        n, cap;
    float         decay, cutoff;
    size_t        max_depth;
    TokenVisitFn  on_visit;
    void         *user;
    Token        *trigger_self;
    AssocEntry   *trigger_entry;
};

static inline void activate_ctx_init(ActivateCtx *a,
                                     float decay, float cutoff, size_t max_depth) {
    memset(a, 0, sizeof *a);
    a->decay     = decay    > 0 ? decay    : 0.7f;
    a->cutoff    = cutoff   > 0 ? cutoff   : 0.05f;
    a->max_depth = max_depth ? max_depth   : 16;
}
static inline void activate_ctx_free(ActivateCtx *a) {
    if (a) { free(a->stack); a->stack = NULL; a->n = a->cap = 0; }
}
static inline int act_push(ActivateCtx *a, Token *t) {
    if (a->n == a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 8;
        Token **ns = (Token **)realloc(a->stack, ncap * sizeof *ns);
        if (!ns) return -1;
        a->stack = ns; a->cap = ncap;
    }
    a->stack[a->n++] = t;
    return 0;
}
static inline int act_seen(const ActivateCtx *a, const Token *t) {
    for (size_t i = 0; i < a->n; ++i) if (a->stack[i] == t) return 1;
    return 0;
}

static inline void token_activate_action(Token *t,
                                         Token *const *ctx, size_t n_ctx,
                                         float energy, size_t depth,
                                         ActivateCtx *a);

static inline void token_activate(Token *t,
                                  Token *const *ctx, size_t n_ctx,
                                  float energy, size_t depth,
                                  ActivateCtx *a) {
    if (!t || !a) return;
    if (depth > a->max_depth) return;
    if (energy < a->cutoff) return;
    if (act_seen(a, t)) return;
    if (act_push(a, t) != 0) return;
    token_touch(t);

    if (a->on_visit) a->on_visit(t, depth, energy, a->user);

    size_t reach = token_reach(t, energy);
    float next_e = energy * a->decay;
    for (size_t i = 0; i < reach; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        token_touch_entry(t, e);
        if (e->action) {
            Token *ss = a->trigger_self; AssocEntry *se = a->trigger_entry;
            a->trigger_self = t; a->trigger_entry = e;
            token_activate_action(e->action, ctx, n_ctx, next_e, depth + 1, a);
            a->trigger_self = ss; a->trigger_entry = se;
        }
        token_activate(e->main, ctx, n_ctx, next_e, depth + 1, a);
    }
    a->n--;
}

static inline void token_activate_action(Token *t,
                                         Token *const *ctx, size_t n_ctx,
                                         float energy, size_t depth,
                                         ActivateCtx *a) {
    if (!t || !a) return;
    if (depth > a->max_depth) return;
    if (energy < a->cutoff) return;

    if (a->on_visit) a->on_visit(t, depth, energy, a->user);

    size_t reach = token_reach(t, energy);
    float next_e = energy * a->decay;
    for (size_t i = 0; i < reach; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        token_touch_entry(t, e);
        if (e->action) {
            Token *ss = a->trigger_self; AssocEntry *se = a->trigger_entry;
            a->trigger_self = t; a->trigger_entry = e;
            token_activate_action(e->action, ctx, n_ctx, next_e, depth + 1, a);
            a->trigger_self = ss; a->trigger_entry = se;
        }
        token_activate(e->main, ctx, n_ctx, next_e, depth + 1, a);
    }
}

static inline void token_activate_from(Token *t,
                                       Token *const *ctx, size_t n_ctx,
                                       float energy, ActivateCtx *a) {
    token_activate(t, ctx, n_ctx, energy, 0, a);
}

/* ============================ 持久化 ============================ */

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct { uint8_t *buf; size_t len, cap; int overflow; } Writer;
typedef struct { const uint8_t *buf; size_t len, pos; int err; } Reader;

static inline void w_bytes(Writer *w, const void *src, size_t n) {
    if (w->buf) {
        if (w->len + n > w->cap) { w->overflow = 1; return; }
        memcpy(w->buf + w->len, src, n);
    }
    w->len += n;
}
static inline void w_u8 (Writer *w, uint8_t v)  { w_bytes(w, &v, 1); }
static inline void w_u32(Writer *w, uint32_t v) { uint8_t b[4]; put_u32(b, v); w_bytes(w, b, 4); }

static inline int r_bytes(Reader *r, void *dst, size_t n) {
    if (r->pos + n > r->len) { r->err = 1; return -1; }
    if (dst) memcpy(dst, r->buf + r->pos, n);
    r->pos += n;
    return 0;
}
static inline uint8_t  r_u8 (Reader *r) { uint8_t b = 0; r_bytes(r, &b, 1); return b; }
static inline uint32_t r_u32(Reader *r) { uint8_t b[4]; if (r_bytes(r, b, 4)) return 0; return get_u32(b); }

/* 格式（v3，小端）：
 *   magic, version, n_tokens
 *   per token:
 *     id, name_len, name, meaning_len, meaning,
 *     misses, recent_misses, parent_id, is_category, n_assocs
 *     per assoc: n_subs, subs[..], n_not, subs_not[..],
 *                main_id, action_id, hits, seq
 * tick / mem_state 不序列化；加载后所有对象视为 ACTIVE、刚创建。
 */
static inline size_t token_pool_dump(const TokenPool *p,
                                     uint8_t *out, size_t cap,
                                     const MeaningCodec *codec) {
    Writer w = { out, 0, cap, 0 };
    w_u32(&w, TOKEN_MAGIC);
    w_u32(&w, TOKEN_VERSION);
    w_u32(&w, (uint32_t)p->count);

    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        w_u32(&w, t->id);

        const char *name = t->name ? t->name : "";
        size_t nlen = t->name ? strlen(t->name) : 0;
        w_u32(&w, (uint32_t)nlen);
        w_bytes(&w, name, nlen);

        if (codec && codec->dump_meaning && t->meaning) {
            size_t mlen = codec->dump_meaning(t->meaning, NULL, 0);
            if (mlen > 0xFFFFFFFFu) { w.overflow = 1; return 0; }
            w_u32(&w, (uint32_t)mlen);
            if (mlen && w.buf) {
                if (w.len + mlen > w.cap) w.overflow = 1;
                else {
                    uint8_t *tmp = (uint8_t *)malloc(mlen);
                    if (!tmp) w.overflow = 1;
                    else {
                        size_t wrote = codec->dump_meaning(t->meaning, tmp, mlen);
                        if (wrote != mlen) w.overflow = 1;
                        else memcpy(w.buf + w.len, tmp, mlen);
                        free(tmp);
                    }
                }
            }
            w.len += mlen;
        } else w_u32(&w, 0);

        w_u32(&w, t->misses);
        w_u32(&w, t->recent_misses);
        w_u32(&w, t->parent ? t->parent->id : TOKEN_ID_NONE);
        w_u8 (&w, t->is_category);
        w_u32(&w, (uint32_t)t->count);

        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            w_u8(&w, e->n_subs);
            for (uint8_t k = 0; k < e->n_subs; ++k) w_u32(&w, e->subs[k]->id);
            w_u8(&w, e->n_not);
            for (uint8_t k = 0; k < e->n_not; ++k) w_u32(&w, e->subs_not[k]->id);
            w_u32(&w, e->main->id);
            w_u32(&w, e->action ? e->action->id : TOKEN_ID_NONE);
            w_u32(&w, e->hits);
            w_u32(&w, e->seq);
        }
    }
    return w.overflow ? 0 : w.len;
}

static inline TokenPool *token_pool_load(const uint8_t *in, size_t len,
                                         const MeaningCodec *codec) {
    Reader r = { in, len, 0, 0 };
    if (r_u32(&r) != TOKEN_MAGIC) return NULL;
    if (r_u32(&r) != TOKEN_VERSION) return NULL;
    uint32_t n = r_u32(&r);
    if (r.err) return NULL;

    TokenPool *p = pool_new();
    if (!p) return NULL;
    if (codec) p->meaning_free = codec->free_meaning;

    size_t   *assoc_off = (size_t   *)calloc(n ? n : 1, sizeof(size_t));
    uint32_t *assoc_n   = (uint32_t *)calloc(n ? n : 1, sizeof(uint32_t));
    uint32_t *parent_id = (uint32_t *)calloc(n ? n : 1, sizeof(uint32_t));
    if (!assoc_off || !assoc_n || !parent_id) {
        free(assoc_off); free(assoc_n); free(parent_id);
        pool_free(p); return NULL;
    }

    /* 第一遍：建 Token */
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id   = r_u32(&r);
        uint32_t nlen = r_u32(&r);
        char *name = NULL;
        if (nlen) {
            name = (char *)malloc(nlen + 1);
            if (!name) goto fail;
            if (r_bytes(&r, name, nlen) != 0) { free(name); goto fail; }
            name[nlen] = 0;
        }
        uint32_t mlen = r_u32(&r);
        void *meaning = NULL;
        if (mlen) {
            if (codec && codec->load_meaning)
                meaning = codec->load_meaning(r.buf + r.pos, mlen);
            if (r_bytes(&r, NULL, mlen) != 0) {
                free(name);
                if (meaning && codec && codec->free_meaning)
                    codec->free_meaning(meaning);
                goto fail;
            }
        }
        uint32_t t_misses = r_u32(&r);
        uint32_t t_recent = r_u32(&r);
        uint32_t p_id     = r_u32(&r);
        uint8_t  is_cat   = r_u8(&r);
        uint32_t na       = r_u32(&r);

        Token *t = pool_token(p, name, meaning);
        free(name);
        if (!t) {
            if (meaning && codec && codec->free_meaning)
                codec->free_meaning(meaning);
            goto fail;
        }
        if (t->id != id) goto fail;
        t->misses        = t_misses;
        t->recent_misses = t_recent;
        t->is_category   = is_cat;
        parent_id[i]     = p_id;
        assoc_off[i]     = r.pos;
        assoc_n[i]       = na;

        for (uint32_t j = 0; j < na; ++j) {
            uint8_t n_subs = r_u8(&r);
            if (r_bytes(&r, NULL, (size_t)4 * n_subs) != 0) goto fail;
            uint8_t n_not = r_u8(&r);
            if (r_bytes(&r, NULL, (size_t)4 * n_not + 16u) != 0) goto fail;
        }
        if (r.err) goto fail;
    }

    /* 挂 parent */
    for (uint32_t i = 0; i < n; ++i) {
        if (parent_id[i] == TOKEN_ID_NONE) continue;
        Token *t = pool_token_by_id(p, i);
        Token *par = pool_token_by_id(p, parent_id[i]);
        if (!t || !par) goto fail;
        t->parent = par;
    }

    /* 第二遍：重建 assocs */
    for (uint32_t i = 0; i < n; ++i) {
        Token *t = pool_token_by_id(p, i);
        if (!t) goto fail;
        Reader r2 = { in, len, assoc_off[i], 0 };
        for (uint32_t j = 0; j < assoc_n[i]; ++j) {
            uint8_t n_subs = r_u8(&r2);
            if (n_subs > TOKEN_CTX_MAX) goto fail;
            Token *subs[TOKEN_CTX_MAX];
            for (uint8_t k = 0; k < n_subs; ++k) {
                uint32_t sid = r_u32(&r2);
                subs[k] = pool_token_by_id(p, sid);
                if (!subs[k]) goto fail;
            }
            uint8_t n_not = r_u8(&r2);
            if (n_not > TOKEN_CTX_MAX) goto fail;
            Token *subs_not[TOKEN_CTX_MAX];
            for (uint8_t k = 0; k < n_not; ++k) {
                uint32_t sid = r_u32(&r2);
                subs_not[k] = pool_token_by_id(p, sid);
                if (!subs_not[k]) goto fail;
            }
            uint32_t mid  = r_u32(&r2);
            uint32_t aid  = r_u32(&r2);
            uint32_t hits = r_u32(&r2);
            uint32_t seq  = r_u32(&r2);
            Token *main   = pool_token_by_id(p, mid);
            Token *action = (aid == TOKEN_ID_NONE) ? NULL : pool_token_by_id(p, aid);
            if (!main) goto fail;
            if (aid != TOKEN_ID_NONE && !action) goto fail;
            if (token_add_assoc_ex(t, subs, n_subs, subs_not, n_not,
                                   main, action) != 0) goto fail;
            AssocEntry *e = &t->assocs[t->count - 1];
            e->hits = hits;
            e->seq  = seq;
            if (seq >= t->next_seq && seq != TOKEN_SEQ_NONE)
                t->next_seq = seq + 1;
        }
        if (r2.err) goto fail;
    }

    for (size_t i = 0; i < p->count; ++i) {
        const char *nm = p->tokens[i]->name;
        if (nm && strcmp(nm, TOKEN_UNKNOWING_NAME) == 0) {
            p->unknowing = p->tokens[i];
            break;
        }
    }

    free(assoc_off); free(assoc_n); free(parent_id);
    return p;

fail:
    free(assoc_off); free(assoc_n); free(parent_id);
    pool_free(p);
    return NULL;
}

#endif /* TOKEN_H */
```

---

## 二、token_lang.h —— 与 token.h 对齐

`token_lang.h` 逻辑不变，但要**删掉自己的 `token_remove_assoc`**（已在 token.h 里）和**删除重复的哈希/池函数**（不存在）。改动只有一处：

```c
/* 删掉 token_lang.h 里的这段（已移到 token.h）：
static inline int token_remove_assoc(Token *t, Token *sub, Token *main) { ... }
*/
```

其余部分保持 v9 的版本即可——原语名、`PrimKind`、`Interp`、`pool_init_prims`、`interp_exec_prim`、`token_run*`、`focus_selector_*`、`token_run_focus`、`token_activate_lang` 全部原样。

---

## 三、ai.h —— 完整最终版

上面已经给过完整版。要确认的三个点：

1. `ai_gc` 第三遍**直接删**（不再有 `gc_max_population` 门槛）
2. `ai_pick_next` 过滤 `mem_state >= MEM_DORMANT`
3. 所有 `e->hits++` 已换成 `token_touch_entry`

这三条前面那版都满足，**原样使用**。

---

## 四、agent.h —— 与 ai.h 对齐

只改一处：`agent_step` 里原本写了“GC 有条件才跑”，现在直接调 `ai_gc`：

```c
/* agent_step 末尾 */
if (ai->gc.gc_interval && (ai->turns % ai->gc.gc_interval) == 0)
    ai_gc(ai);   /* 内部现在会立即删 FORGOTTEN */
```

其余原样。

---

## 五、main.c —— 无改动

上一版即可。

---

## 六、文件间的一致性检查

| 检查 | 状态 |
|---|---|
| `token.h` 里的 `token_touch` / `token_touch_entry` 被 `ai.h` 调用 | ✓ |
| `token.h` 里的 `token_remove_assoc` 被 `token_lang.h` 调用 | ✓ |
| `token.h` 里的 `MemState` 在 `ai.h` 里使用 | ✓ |
| `token_pool_dump/load` 格式 v3 与 `token.h` 中的字段一致 | ✓ |
| 所有 `e->hits++` 都换成了 touch | ✓ |
| `token_activate` 里 `act_push` 后 `token_touch(t)` | ✓ |
| 加载后 `owner` / `current_tick` / `mem_state` 初始化 | ✓（pool_token 统一处理） |

---

## 七、一句话

**五个文件，职责清晰：`token.h` 是图，`token_lang.h` 是操作图的程序，`ai.h` 是认知循环，`agent.h` 是感知-行动循环，`main.c` 是入口。所有遗忘、状态、tick 都已整合进 `token.h`，`ai.h` 只负责策略，`token_lang.h` 只负责原语。**
