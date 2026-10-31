/* claim.h — **待证**（阶段二 WP4 的第一刀：不轻信）（§6.636）
 *
 * 需求（用户）：**不会随随便便就信论文和用户说的话**。
 *
 * 做法（不扩图、不改存档格式）：
 *   · 带"来源前缀"的话（`听说/据说/有人说/网上/书上/论文…`）**不写进事实层**，
 *     只在图里留一个**待证标记** `[待证:<来源>:A_R_B]`（内部命名空间，随图存盘、可数、可删）；
 *   · 待证**不参与推理**（它只是标记节点，没有 `A --R--> B` 那条边）；
 *   · `::claim` 列出来；`::claim accept N` = 用户确认 ⇒ 才写进事实层（并落教学留痕）；
 *     `::claim reject N` = 丢掉；
 *   · 同一条待证被**第二个不同来源**再说一次 ⇒ 计数加一（**仍然不自动升格**：
 *     两条都是"听说"不算交叉验证；这条在文档里写明）。
 *
 * 为什么用标记节点而不是新字段：存档格式刚抬到 v15（§6.630），这一版不想再动格式；
 * 标记节点跟着图走，重启后照样在（`::claim` 能列出来）。
 */
#ifndef CLAIM_H
#define CLAIM_H

#include "agent.h"

#define CLAIM_PREFIX MICRO_CLAIM_TOKEN_PREFIX
#define CLAIM_MAX_SRC 20

typedef struct {
    Token *node;
    char   src[24];
    char   a[40];
    char   r[24];
    char   b[40];
} Claim;

static inline int claim_is_marker(const char *n) {
    return n && strncmp(n, CLAIM_PREFIX, strlen(CLAIM_PREFIX)) == 0;
}

/* 造标记名：`[待证:来源:A_R_B]` */
static inline const char *claim_marker_name(const char *src, const char *a, const char *r,
                                           const char *b, char *buf, size_t cap) {
    if (!src || !a || !r || !b || !buf) return NULL;
    int w = snprintf(buf, cap, "%s%s:%s_%s_%s]", CLAIM_PREFIX, src, a, r, b);
    if (w < 0 || (size_t)w >= cap) return NULL;
    return buf;
}

/* 解析标记名；失败返回 0。 */
static inline int claim_parse(const char *name, char *src, size_t scap, char *a, size_t acap,
                              char *r, size_t rcap, char *b, size_t bcap) {
    if (!claim_is_marker(name)) return 0;
    const char *p = name + strlen(CLAIM_PREFIX);
    const char *c1 = strchr(p, ':');
    if (!c1) return 0;
    if ((size_t)(c1 - p) >= scap) return 0;
    memcpy(src, p, (size_t)(c1 - p)); src[c1 - p] = 0;
    p = c1 + 1;
    const char *u1 = strchr(p, '_');
    if (!u1) return 0;
    if ((size_t)(u1 - p) >= acap) return 0;
    memcpy(a, p, (size_t)(u1 - p)); a[u1 - p] = 0;
    p = u1 + 1;
    const char *u2 = strrchr(p, '_');
    if (!u2 || u2 == p) return 0;
    if ((size_t)(u2 - p) >= rcap) return 0;
    memcpy(r, p, (size_t)(u2 - p)); r[u2 - p] = 0;
    p = u2 + 1;
    const char *end = strrchr(p, ']');
    if (end && (size_t)(end - p) >= bcap) return 0;
    snprintf(b, bcap, "%.*s", (int)((end ? (size_t)(end - p) : strlen(p))), p);
    return src[0] && a[0] && r[0] && b[0];
}

/* 加一条待证（已存在就返回已有的那个节点，不重复造） */
static inline Token *claim_add(AI *ai, const char *src, const char *a, const char *r,
                               const char *b) {
    if (!ai || !src || !a || !r || !b) return NULL;
    char nm[160];
    if (!claim_marker_name(src, a, r, b, nm, sizeof nm)) return NULL;
    return pool_token_intern(ai->pool, nm);
}

/* 列全部待证（按池里顺序） */
static inline size_t claim_list(AI *ai, Claim *out, size_t cap) {
    if (!ai || !out || cap == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count && n < cap; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage || !t->name || !claim_is_marker(t->name)) continue;
        out[n].node = t;
        if (!claim_parse(t->name, out[n].src, sizeof out[n].src, out[n].a, sizeof out[n].a,
                         out[n].r, sizeof out[n].r, out[n].b, sizeof out[n].b)) continue;
        n++;
    }
    return n;
}

/* 第 idx 条（0 基） */
static inline int claim_at(AI *ai, size_t idx, Claim *out) {
    Claim all[64];
    size_t n = claim_list(ai, all, 64);
    if (idx >= n) return 0;
    *out = all[idx];
    return 1;
}

/* 丢掉一条待证（标垃圾；不是知识，没有边要清） */
static inline int claim_reject(AI *ai, Token *node) {
    if (!ai || !node) return 0;
    node->is_garbage = 1;
    (void)ai;
    return 1;
}

/* 确认一条待证 ⇒ **这时才写进事实层**（并落教学留痕：是用户确认的） */
static inline int claim_accept(AI *ai, Token *node, char *out, size_t cap) {
    if (!ai || !node || !node->name || node->is_garbage) return 0;   /* 已经确认/丢掉的不再处理 */
    char src[24], a[40], r[24], b[40];
    if (!claim_parse(node->name, src, sizeof src, a, sizeof a, r, sizeof r, b, sizeof b)) return 0;
    uint8_t rid = rel_id_of(ai, r, 0);
    if (!rid) rid = rel_id_of(ai, r, 1);
    Token *A = pool_token_intern(ai->pool, a);
    Token *B = pool_token_intern(ai->pool, b);
    if (!rid || !A || !B || A == B) return 0;
    if (!rel_direct(A, rid, B) && ai_write_rel(ai, A, B, rid, 0, AI_WSRC_RELATION) != 0) return 0;
    agent_teach_mark(ai, A, rid, B, 0);
    ai->n_rel_learned++;
    claim_reject(ai, node);
    if (out && cap) snprintf(out, cap, "%s %s %s", a, r, b);
    return 1;
}

#endif /* CLAIM_H */
