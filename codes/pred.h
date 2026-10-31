/* pred.h — **谓词（多元关系）**：把三元图当"谓词 + 参数"用（阶段二 WP1；§6.633）
 *
 * 为什么需要：现有图只有 `A --R--> B`（二元）。阶段二要的"复杂逻辑"里，
 * `敌人(A,B)`、`给(A,B,C)`、`between(A,B,C)` 这种**三元/多元**关系没地方放。
 *
 * **不扩图**（用户的硬约束）：不新增存储结构、不改存档格式。做法：
 *   · **一条谓词实例 = 一个符号节点**：`[谓词:敌人(甲,乙)]`（内核内部命名空间，可参与边、随图存盘）；
 *   · 第 K 个参数 = `[谓词:…] --[参数K]--> 参数值`；
 *   · `参数K` 是**动态关系**（`参数1`…`参数8`）—— 读回来只用现有走边，
 *     名字→id 的对应关系随图存盘（动态关系表 v7 起就进存档）。
 *
 * 为什么把参数写进**节点名**（而不是"一个名字一个节点"）：
 * 一个名字只用一个节点的话，`敌人(甲,乙)` 与 `敌人(甲,丙)` 会共用同一个节点，
 * 第 2 个参数只能返回"最先找到的那个" —— **两个实例就没法区分**（第一版就是这么错的，
 * `predprobe` 当场红）。把参数写进名字 ⇒ 实例 = 节点，`pred_find` 退化成一次 `pool_find`（O(1)）。
 *
 * 已知边界（写死，别偷偷放宽）：
 *   1. 参数最多 `PRED_MAX_ARGS` = 8；超过就**拒绝**（不截断、不猜）；
 *   2. 参数值里**不能有** `(` `)` `,`（会和实例名的格式打架）——有就拒绝；
 *   3. 参数值必须是词/节点，**不能嵌套谓词**（`敌人(A, 兄弟(B,C))` 不支持）；
 *   4. 没有类型/元数注册表：元数由实例名里的参数个数与实际 `参数K` 边共同决定；
 *      `pred_arity()` 会检查"编号 1..N 的边都在"，缺一条就 `ok=0`（如实报坏）。
 */
#ifndef PRED_H
#define PRED_H

/* 先 agent.h：rel_* / token_* / pool_* 与 micro 的内部命名空间都由这一层带进来 */
#include "agent.h"

#define PRED_MAX_ARGS   8
#define PRED_PREFIX     MICRO_PRED_TOKEN_PREFIX     /* `[谓词:` */
#define PRED_ARG_PREFIX "参数"
#define PRED_NAME_CAP   120

/* 第 k 个参数用的动态关系名（k 从 1 开始）。**保留名字**：用户不能拿它当普通谓语。 */
static inline const char *pred_arg_relname(size_t k, char *buf, size_t cap) {
    snprintf(buf, cap, PRED_ARG_PREFIX "%zu", k);
    return buf;
}
static inline int pred_is_arg_rel(const char *name) {
    if (!name) return 0;
    if (strncmp(name, PRED_ARG_PREFIX, strlen(PRED_ARG_PREFIX)) != 0) return 0;
    const char *p = name + strlen(PRED_ARG_PREFIX);
    if (!*p) return 0;
    for (; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}
static inline int pred_is_pred_name(const char *name) {
    return name && strncmp(name, PRED_PREFIX, strlen(PRED_PREFIX)) == 0;
}
/* 实例名里的谓词名（`[谓词:敌人(甲,乙)]` ⇒ `敌人`）；不是谓词节点返回 NULL。
 * 结果写进调用者的 buf（不改动原串）。 */
static inline const char *pred_plain_name(const char *full, char *buf, size_t cap) {
    if (!pred_is_pred_name(full) || !buf || cap == 0) return NULL;
    const char *s = full + strlen(PRED_PREFIX);
    size_t n = 0;
    while (s[n] && s[n] != '(' && n + 1 < cap) { buf[n] = s[n]; n++; }
    buf[n] = 0;
    return buf;
}
/* 参数值能不能放进实例名 */
static inline int pred_arg_text_ok(const char *s) {
    if (!s || !s[0]) return 0;
    for (const char *p = s; *p; ++p)
        if (*p == '(' || *p == ')' || *p == ',') return 0;
    return 1;
}
/* 拼实例名：`[谓词:名字(参数1,参数2)]` */
static inline const char *pred_full_name(const char *name, char *const *args, size_t n,
                                        char *buf, size_t cap) {
    if (!name || !name[0] || !args || n == 0 || n > PRED_MAX_ARGS || !buf || cap == 0) return NULL;
    int w = snprintf(buf, cap, "%s%s(", PRED_PREFIX, name);
    if (w < 0 || (size_t)w >= cap) return NULL;
    size_t o = (size_t)w;
    for (size_t k = 0; k < n; ++k) {
        if (!pred_arg_text_ok(args[k])) return NULL;
        w = snprintf(buf + o, cap - o, "%s%s", args[k], (k + 1 == n) ? ")]" : ",");
        if (w < 0 || o + (size_t)w >= cap) return NULL;
        o += (size_t)w;
    }
    return buf;
}

/* 第 k 个参数值（k 从 1 开始）；没有就 NULL。 */
static inline Token *pred_arg(AI *ai, const Token *p, size_t k) {
    if (!ai || !p || k == 0 || k > PRED_MAX_ARGS) return NULL;
    char rn[24];
    uint8_t rid = rel_id_of(ai, pred_arg_relname(k, rn, sizeof rn), 0);
    if (!rid || rid < REL_DYN_BASE) return NULL;
    uint8_t dyn = (uint8_t)(rid - REL_DYN_BASE + 1);
    for (size_t i = 0; i < p->count; ++i) {
        const AssocEntry *e = &token_assocs_ro(p)[i];
        if (e->dyn != dyn || !e->main) continue;
        return e->main;
    }
    return NULL;
}

/* 参数个数 = 实例名里写了几个参数；同时检查边：
 * `ok` 只有在"每个 参数K 边都在"时才为 1（名字说有、边上没有 = 结构坏了，如实报）。 */
static inline size_t pred_arity(AI *ai, const Token *p, int *out_ok) {
    if (out_ok) *out_ok = 0;
    if (!ai || !p || !p->name) return 0;
    char plain[64];
    if (!pred_plain_name(p->name, plain, sizeof plain)) return 0;
    const char *lp = strchr(p->name, '(');
    if (!lp) return 0;
    size_t n = 1;                              /* 逗号数 + 1 */
    for (const char *q = lp; *q; ++q) if (*q == ',') n++;
    if (n > PRED_MAX_ARGS) return n;
    int ok = 1;
    for (size_t k = 1; k <= n; ++k) if (!pred_arg(ai, p, k)) { ok = 0; break; }
    if (out_ok) *out_ok = ok;
    return n;
}

/* 按名字+参数找实例（**O(1)**：实例名就是键） */
static inline Token *pred_find(AI *ai, const char *name, char *const *args, size_t n) {
    if (!ai || !name || !args || n == 0 || n > PRED_MAX_ARGS) return NULL;
    char full[PRED_NAME_CAP];
    if (!pred_full_name(name, args, n, full, sizeof full)) return NULL;
    Token *p = pool_find(ai->pool, full);
    if (!p) return NULL;
    int ok = 0;
    if (pred_arity(ai, p, &ok) != n || !ok) return NULL;    /* 名字对、边不全 ⇒ 不算找到 */
    return p;
}

/* 造（或取回）一个谓词实例；参数为 0 或 > PRED_MAX_ARGS、或参数里有 `( ) ,` 时返回 NULL。 */
static inline Token *pred_make(AI *ai, const char *name, char *const *args, size_t n) {
    if (!ai || !name || !name[0] || !args || n == 0 || n > PRED_MAX_ARGS) return NULL;
    if (pred_is_arg_rel(name) || pred_is_pred_name(name)) return NULL;   /* 别嵌套自身 */
    char full[PRED_NAME_CAP];
    if (!pred_full_name(name, args, n, full, sizeof full)) return NULL;
    Token *p = pool_token_intern(ai->pool, full);
    if (!p) return NULL;
    for (size_t k = 0; k < n; ++k) {
        Token *v = pool_token_intern(ai->pool, args[k]);
        if (!v || v == p) return NULL;
        char rn[24];
        uint8_t rid = rel_id_of(ai, pred_arg_relname(k + 1, rn, sizeof rn), 1);
        if (!rid) return NULL;
        if (ai_write_rel(ai, p, v, rid, 0, AI_WSRC_RELATION) != 0) return NULL;
    }
    return p;
}

/* 列出**不同的谓词名**（剥掉实例参数），返回不同名字的个数 */
static inline size_t pred_list_names(AI *ai, char out[][64], size_t cap) {
    if (!ai || !out || cap == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *p = ai->pool->tokens[i];
        if (!p || p->is_garbage || !p->name || !pred_is_pred_name(p->name)) continue;
        char plain[64];
        if (!pred_plain_name(p->name, plain, sizeof plain)) continue;
        int dup = 0;
        for (size_t k = 0; k < n; ++k) if (!strcmp(out[k], plain)) { dup = 1; break; }
        if (!dup && n < cap) snprintf(out[n++], 64, "%s", plain);
    }
    return n;
}
/* 列出某个谓词名的**全部实例**节点（返回写进 out 的个数） */
static inline size_t pred_instances(AI *ai, const char *name, Token **out, size_t cap) {
    if (!ai || !name || !out || cap == 0) return 0;
    size_t n = 0;
    char pfx[PRED_NAME_CAP];
    int w = snprintf(pfx, sizeof pfx, "%s%s(", PRED_PREFIX, name);
    if (w < 0 || (size_t)w >= sizeof pfx) return 0;
    for (size_t i = 0; i < ai->pool->count && n < cap; ++i) {
        Token *p = ai->pool->tokens[i];
        if (!p || p->is_garbage || !p->name) continue;
        if (strncmp(p->name, pfx, strlen(pfx)) == 0) out[n++] = p;
    }
    return n;
}

#endif /* PRED_H */
