/* token_lang.h — 原语 + 栈机 + 语法 + VSA 操作
 * 依赖：token.h
 */
#ifndef TOKEN_LANG_H
#define TOKEN_LANG_H

#include "token.h"

#define PRIM_OBJECT       "[原语:对象]"
#define PRIM_GOOD         "[原语:好]"
#define PRIM_BAD          "[原语:坏]"
#define PRIM_SPLIT        "[原语:分割]"
#define PRIM_REQUEST      "[原语:请求]"
#define PRIM_EXECUTE      "[原语:执行]"
#define PRIM_REFUSE       "[原语:拒绝]"
#define PRIM_WRAP         "[原语:包裹]"
#define PRIM_PRE_ASSOC    "[原语:前关联]"
#define PRIM_CONNECT      "[原语:连接]"
#define PRIM_DISCONNECT   "[原语:断开]"
#define PRIM_CREATE       "[原语:创建]"
#define PRIM_RUN          "[原语:运行]"
#define PRIM_IF           "[原语:如果]"
#define PRIM_ELSE         "[原语:否则]"
#define PRIM_END          "[原语:结束]"
#define PRIM_ACTIVATE     "[原语:激活]"
#define PRIM_NEST         "[原语:嵌套]"
#define PRIM_EXPAND       "[原语:展开]"
#define PRIM_PERSPECTIVE  "[原语:视角]"
#define PRIM_BELIEVE      "[原语:认为]"
#define PRIM_QUOTE        "[原语:引用]"
#define PRIM_TIME_NOW     "[原语:现在]"
#define PRIM_TIME_BEFORE  "[原语:之前]"
#define PRIM_TIME_AFTER   "[原语:之后]"
#define PRIM_TIME_CHAIN   "[原语:时间链]"
#define PRIM_BIND         "[原语:绑定]"
#define PRIM_UNBIND       "[原语:解绑]"
#define PRIM_BUNDLE       "[原语:捆绑]"
#define PRIM_QUERY        "[原语:检索]"
#define PRIM_EXTRACT_REL  "[原语:抽关系]"
#define PRIM_APPLY_REL    "[原语:套关系]"
/* ---- Token 行为（图操作）的原语入口：把「行为」写进语言层，用**已有的栈机**组合 ----
 *   [原语:对象] A [原语:对象] B [原语:插词]            在 A、B 之间插一个临时 Token（经它中转）
 *   [原语:对象] A [原语:对象] B [原语:插词为] <名字>    同上，但中转 Token 用这个名字
 *   [原语:对象] A [原语:对象] P [原语:前提接线] Y?      见下（前提接线要三个操作数）
 *   [原语:对象] A [原语:对象] P [原语:对象] Y [原语:前提接线]  让 A 经前提 P 也能到 Y
 *   [原语:回收临时]                                     回收没人引用的草稿
 *
 * **前提**（淘汰「语境」后的表示）：`[原语:前关联] P` 之后紧跟的 `[原语:连接]` 不再往边上
 * 写「必需语境」字段，而是建出 `A —[前提]→ P → B`：P 是**普通 token**（联想照走、
 * 能被正常联想到），`[前提]` 是挂在动作槽上的**行为 token**，逻辑用
 * `token_premise_between(A,B)` 就能问出「这两个之间的前提是什么」。
 * 注意：原语层**不碰图** —— 它只把「行为编号 + 操作数」交给宿主注入的回调，
 * 由持有 AI 的那一层过 token_op_run（安全内核 + 预算 + 审计）。见 docs/token行为-图操作与临时token.md。 */
#define PRIM_PREMISE_TIE  "[原语:前提接线]"
#define PRIM_PRE_ASSOC_NOT "[原语:前关联不]"   /* 反前提：条件是 P **不**在注意窗口里 */
#define PRIM_INSERT       "[原语:插词]"
#define PRIM_INSERT_NAMED "[原语:插词为]"
#define PRIM_COLLECT_TEMP "[原语:回收临时]"

#define SYM_GOOD          "[符号:好]"
#define SYM_BAD           "[符号:坏]"
#define SYM_WRAP          "[符号:包裹]"
#define SYM_REFUSED       "[符号:拒绝]"

typedef enum {
    PK_NONE = 0,
    PK_OBJECT, PK_GOOD, PK_BAD, PK_SPLIT,
    PK_REQUEST, PK_EXECUTE, PK_REFUSE, PK_WRAP,
    PK_PRE_ASSOC, PK_CONNECT, PK_DISCONNECT,
    PK_CREATE, PK_RUN, PK_IF, PK_ELSE, PK_END, PK_ACTIVATE,
    PK_NEST, PK_EXPAND,
    PK_PERSPECTIVE, PK_BELIEVE, PK_QUOTE,
    PK_TIME_NOW, PK_TIME_BEFORE, PK_TIME_AFTER, PK_TIME_CHAIN,
    PK_BIND, PK_UNBIND, PK_BUNDLE, PK_QUERY,
    PK_EXTRACT_REL, PK_APPLY_REL,
    /* Token 行为的原语入口（它们不改图，只把请求交给宿主回调） */
    PK_PREMISE_TIE, PK_INSERT, PK_INSERT_NAMED, PK_COLLECT_TEMP, PK_PRE_ASSOC_NOT,
} PrimKind;

/* 「Token 行为」在原语层的**编号**：按下标引用 micro.h 的 TOKEN_OPS[]。
 * 原语层看不到 micro.h（分层的），所以这里只带编号 ——
 * 「编号 ↔ 行为名」的对应由 tools/optest.c 钉住（改了 TOKEN_OPS 顺序那里当场变红）。 */
typedef enum {
    IPOP_PREMISE_CONNECT     = 0,   /* 加前提（[原语:前关联] P + [原语:连接] A B） */
    IPOP_PREMISE_CONNECT_NOT = 1,   /* 加反前提（[原语:前关联不] P + [原语:连接] A B） */
    IPOP_PREMISE_TIE         = 2,   /* 前提接线 */
    IPOP_INSERT              = 3,   /* 中间插词 */
    IPOP_COLLECT_TEMP        = 4,   /* 回收临时 */
    IPOP_N
} InterpOpKind;

/* 宿主回调：把一条「Token 行为」落地。返回应用条数（<0 = 没跑成）。 */
typedef int (*InterpOpFn)(void *user, InterpOpKind op, char *const *args, size_t n);

typedef struct {
    Token    **stack;
    size_t     n, cap;
    size_t     stmt_start;
    Token     *pending_from, *pending_to;
    int        pending;
    Token     *pending_sub;
    int        pending_not;      /* 待连接的条件是「反前提」吗（[原语:前关联不]） */
    TokenPool *pool;
    Token     *sym_good, *sym_bad, *sym_wrap, *sym_refused;
    int        skip_depth;
    int        last_if_true;
    Token    **ctx;
    size_t     n_ctx;
    float      energy;
    /* Token 行为的宿主（原语层靠它把请求交出去；NULL = 没人接，原语不生效） */
    InterpOpFn op_fn;
    void      *op_user;
} Interp;

typedef Token *(*FocusSelector)(Token *focus, Token *const *ctx,
                                size_t n_ctx, float energy, void *user);

typedef struct {
    Interp        base;
    Token        *focus;
    size_t        steps;
    Token       **trace;
    size_t        trace_n, trace_cap;
    FocusSelector selector;
    void         *selector_user;
} FocusRun;

static inline void token_run_names(TokenPool *p, const char *const *names, size_t n);
static inline void token_run_names_ctx(TokenPool *p, const char *const *names, size_t n,
                                        InterpOpFn op_fn, void *op_user);

/* ============================ 原语识别 ============================ */

/* **原语表：名字只写一份。**
 * 以前名字写了两处（这里的 if 链 + pool_init_prims 的清单），加一条原语要改两处 ——
 * 「同一份清单出现在两处」正是设计原则要清掉的东西。现在一张 name→kind 表：
 * 识别走它、intern 也走它；新增一条原语 = 表里加一行 + 写它的执行分支。 */
typedef struct { const char *name; PrimKind kind; } PrimDef;

static const PrimDef PRIMS[] = {
    { PRIM_OBJECT,       PK_OBJECT },
    { PRIM_GOOD,         PK_GOOD },
    { PRIM_BAD,          PK_BAD },
    { PRIM_SPLIT,        PK_SPLIT },
    { PRIM_REQUEST,      PK_REQUEST },
    { PRIM_EXECUTE,      PK_EXECUTE },
    { PRIM_REFUSE,       PK_REFUSE },
    { PRIM_WRAP,         PK_WRAP },
    { PRIM_PRE_ASSOC,    PK_PRE_ASSOC },
    { PRIM_CONNECT,      PK_CONNECT },
    { PRIM_DISCONNECT,   PK_DISCONNECT },
    { PRIM_CREATE,       PK_CREATE },
    { PRIM_RUN,          PK_RUN },
    { PRIM_IF,           PK_IF },
    { PRIM_ELSE,         PK_ELSE },
    { PRIM_END,          PK_END },
    { PRIM_ACTIVATE,     PK_ACTIVATE },
    { PRIM_NEST,         PK_NEST },
    { PRIM_EXPAND,       PK_EXPAND },
    { PRIM_PERSPECTIVE,  PK_PERSPECTIVE },
    { PRIM_BELIEVE,      PK_BELIEVE },
    { PRIM_QUOTE,        PK_QUOTE },
    { PRIM_TIME_NOW,     PK_TIME_NOW },
    { PRIM_TIME_BEFORE,  PK_TIME_BEFORE },
    { PRIM_TIME_AFTER,   PK_TIME_AFTER },
    { PRIM_TIME_CHAIN,   PK_TIME_CHAIN },
    { PRIM_BIND,         PK_BIND },
    { PRIM_UNBIND,       PK_UNBIND },
    { PRIM_BUNDLE,       PK_BUNDLE },
    { PRIM_QUERY,        PK_QUERY },
    { PRIM_EXTRACT_REL,  PK_EXTRACT_REL },
    { PRIM_APPLY_REL,    PK_APPLY_REL },
    { PRIM_PRE_ASSOC_NOT, PK_PRE_ASSOC_NOT }, /* 反前提（[原语:前关联不]） */
    { PRIM_PREMISE_TIE,  PK_PREMISE_TIE },   /* Token 行为入口（见文件头注释） */

    { PRIM_INSERT,       PK_INSERT },
    { PRIM_INSERT_NAMED, PK_INSERT_NAMED },
    { PRIM_COLLECT_TEMP, PK_COLLECT_TEMP },
    { NULL, PK_NONE }
};

/* 符号（不是原语，但要 intern 进池子，切词器和 ::asm 才认得出） */
static const char *const PRIM_SYMS[] = {
    SYM_GOOD, SYM_BAD, SYM_WRAP, SYM_REFUSED, NULL
};

static inline size_t prim_count(void) {
    size_t n = 0;
    while (PRIMS[n].name) n++;
    return n;
}

static inline PrimKind prim_kind(const Token *t) {
    if (!t || !t->name) return PK_NONE;
    for (size_t i = 0; PRIMS[i].name; ++i)
        if (strcmp(t->name, PRIMS[i].name) == 0) return PRIMS[i].kind;
    return PK_NONE;
}

static inline void pool_init_prims(TokenPool *p) {
    /* 名字**只有一份**：原语表 + 符号表（以前这里是第二份手抄清单） */
    for (size_t i = 0; PRIMS[i].name; ++i) pool_token_intern(p, PRIMS[i].name);
    for (size_t i = 0; PRIM_SYMS[i]; ++i)   pool_token_intern(p, PRIM_SYMS[i]);
}

/* ============================ 栈操作 ============================ */

static inline int interp_push(Interp *ip, Token *t) {
    if (!t) return -1;
    if (ip->n == ip->cap) {
        size_t ncap = ip->cap ? ip->cap * 2 : 16;
        Token **ns = (Token **)realloc(ip->stack, ncap * sizeof *ns);
        if (!ns) return -1;
        ip->stack = ns; ip->cap = ncap;
    }
    ip->stack[ip->n++] = t;
    return 0;
}
static inline Token *interp_top(Interp *ip)    { return ip->n ? ip->stack[ip->n - 1] : NULL; }
static inline Token *interp_second(Interp *ip) { return ip->n >= 2 ? ip->stack[ip->n - 2] : NULL; }
static inline Token *interp_pop(Interp *ip)    { return ip->n ? ip->stack[--ip->n] : NULL; }
static inline Token *interp_sym(Interp *ip, Token **slot, const char *name) {
    if (!*slot) *slot = pool_token_intern(ip->pool, name);
    return *slot;
}

/* ============================ 原语执行 ============================ */

/* 把一条「Token 行为」交给宿主（原语层**不碰图**）。
 * 没有宿主（op_fn == NULL）就什么也不做 —— 语言层不该自己改结构，
 * 结构改动只有 micro_step 一个入口。 */
static inline void interp_ask_op(Interp *ip, InterpOpKind op, Token **operands, size_t n) {
    if (!ip || !ip->op_fn) return;
    char buf[4][64];
    char *args[4] = { NULL, NULL, NULL, NULL };
    for (size_t i = 0; i < n && i < 4; ++i) {
        snprintf(buf[i], sizeof buf[i], "%s",
                 operands[i] && operands[i]->name ? operands[i]->name : "");
        args[i] = buf[i];
    }
    (void)ip->op_fn(ip->op_user, op, args, n);
}

static inline void interp_exec_prim(Interp *ip, Token *prim, Token *param) {
    switch (prim_kind(prim)) {
    case PK_OBJECT:
    case PK_CREATE:
        if (param) interp_push(ip, param);
        break;

    case PK_GOOD: {
        Token *x = interp_top(ip);
        if (x) token_add_assoc_unique(x, interp_sym(ip, &ip->sym_good, SYM_GOOD));
        break;
    }
    case PK_BAD: {
        Token *x = interp_top(ip);
        if (x) token_add_assoc_unique(x, interp_sym(ip, &ip->sym_bad, SYM_BAD));
        break;
    }
    case PK_SPLIT:
        ip->n = ip->stmt_start;
        ip->pending = 0;
        ip->pending_from = ip->pending_to = NULL;
        ip->pending_sub = NULL;
        ip->pending_not = 0;
        pool_pop_perspective(ip->pool);
        break;

    case PK_REQUEST: {
        Token *from = interp_top(ip);
        Token *to   = interp_second(ip);
        if (!from) from = prim;
        if (!to)   to   = from;
        ip->pending_from = from; ip->pending_to = to; ip->pending = 1;
        break;
    }
    case PK_EXECUTE:
        if (ip->pending && ip->pending_from && ip->pending_to)
            token_add_assoc_unique(ip->pending_to, ip->pending_from);
        ip->pending = 0;
        break;
    case PK_REFUSE:
        if (ip->pending && ip->pending_from)
            token_add_assoc_unique(ip->pending_from,
                                   interp_sym(ip, &ip->sym_refused, SYM_REFUSED));
        ip->pending = 0;
        break;

    case PK_WRAP: {
        size_t cnt = ip->n - ip->stmt_start;
        if (cnt == 0) break;
        Token *w = interp_sym(ip, &ip->sym_wrap, SYM_WRAP);
        Token *c = pool_token(ip->pool, NULL, NULL);
        if (!c) break;
        for (size_t i = ip->stmt_start; i < ip->n; ++i)
            token_add_assoc_unique(c, ip->stack[i]);
        token_add_assoc_unique(c, w);
        ip->n = ip->stmt_start;
        interp_push(ip, c);
        break;
    }
    case PK_PRE_ASSOC:
        ip->pending_sub = interp_pop(ip);
        ip->pending_not = 0;
        break;
    case PK_PRE_ASSOC_NOT:
        ip->pending_sub = interp_pop(ip);
        ip->pending_not = 1;
        break;
    case PK_CONNECT: {
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        if (a && b) {
            if (ip->pending_sub) {
                /* **前提**（淘汰「语境」后的表示）：不往边上写字段，而是建
                 * `A —[前提]→ P → B`（P 是普通 token，逻辑查得到它是前提）。
                 * 语言层照样不碰图：交给宿主走安全内核。 */
                Token *ops[3] = { a, b, ip->pending_sub };
                interp_ask_op(ip, ip->pending_not ? IPOP_PREMISE_CONNECT_NOT
                                                  : IPOP_PREMISE_CONNECT, ops, 3);
            } else {
                token_add_assoc_ex(a, b, NULL);
            }
        }
        ip->pending_sub = NULL;
        break;
    }
    case PK_DISCONNECT: {
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        if (a && b) token_remove_assoc(a, b);
        ip->pending_sub = NULL;
        ip->pending_not = 0;
        break;
    }

    /* ---- Token 行为：只组装请求，不改图（见文件头那段注释）---- */
    case PK_INSERT: {                       /* [原语:插词]：pop B, A → A、B 之间插临时 Token */
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        Token *ops[2] = { a, b };
        interp_ask_op(ip, IPOP_INSERT, ops, 2);
        break;
    }
    case PK_INSERT_NAMED: {                 /* [原语:插词为] <名字>：param 是中转 Token 的名字 */
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        Token *ops[3] = { a, b, param };
        interp_ask_op(ip, IPOP_INSERT, ops, 3);
        break;
    }
    case PK_PREMISE_TIE: {                  /* [原语:前提接线]：pop Y, P, A */
        Token *y = interp_pop(ip);
        Token *p = interp_pop(ip);
        Token *a = interp_pop(ip);
        Token *ops[3] = { a, p, y };
        interp_ask_op(ip, IPOP_PREMISE_TIE, ops, 3);
        break;
    }
    case PK_COLLECT_TEMP:                   /* [原语:回收临时]：不要操作数 */
        interp_ask_op(ip, IPOP_COLLECT_TEMP, NULL, 0);
        break;
    case PK_RUN:
        if (param && param->name) {
            const char *name = param->name;
            token_run_names(ip->pool, &name, 1);
        }
        break;

    case PK_IF: {
        Token *cond = interp_pop(ip);
        int truthy = cond && !(cond->name && strcmp(cond->name, SYM_BAD) == 0);
        ip->last_if_true = truthy;
        if (!truthy) ip->skip_depth = 1;
        break;
    }
    case PK_ELSE:
        if (ip->skip_depth == 0 && ip->last_if_true) ip->skip_depth = 1;
        break;
    case PK_END:
        if (ip->skip_depth == 1)     ip->skip_depth = 0;
        else if (ip->skip_depth > 1) ip->skip_depth--;
        break;

    case PK_ACTIVATE: {
        Token *x = interp_top(ip);
        if (x && ip->ctx) {
            ActivateCtx act;
            activate_ctx_init(&act, 0.7f, 0.05f, 16);
            token_activate_from(x, ip->ctx, ip->n_ctx, ip->energy, &act);
            activate_ctx_free(&act);
        }
        break;
    }
    case PK_NEST: {
        size_t cnt = ip->n - ip->stmt_start;
        if (cnt < 2) break;
        Token *acc = ip->stack[ip->n - 1];
        for (size_t i = ip->n - 1; i > ip->stmt_start; --i) {
            Token *left = ip->stack[i - 1];
            if (!left || !acc || !left->hv || !acc->hv) break;
            Token *r = pool_token(ip->pool, NULL, NULL);
            if (!r || !r->hv) break;
            vsa_bind(left->hv, acc->hv, r->hv);
            token_add_assoc_unique(r, left);
            token_add_assoc_unique(r, acc);
            acc = r;
        }
        ip->n = ip->stmt_start;
        interp_push(ip, acc);
        break;
    }
    case PK_EXPAND: {
        Token *nested = interp_pop(ip);
        if (!nested || !nested->hv) break;
        Token *left = NULL, *rest = NULL;
        for (size_t i = 0; i < nested->count; ++i) {
            if (!left) left = nested->assocs[i].main;
            else if (!rest) { rest = nested->assocs[i].main; break; }
        }
        if (left) interp_push(ip, left);
        if (rest) interp_push(ip, rest);
        break;
    }
    case PK_PERSPECTIVE:
    case PK_QUOTE: {
        Token *who = interp_pop(ip);
        if (who) {
            pool_push_perspective(ip->pool, who);
            interp_push(ip, who);
        }
        break;
    }
    case PK_BELIEVE: {
        Token *content = interp_pop(ip);
        if (!content || !content->hv) break;
        Token *holder = pool_current_perspective(ip->pool);
        if (!holder || !holder->hv) break;
        Token *r = pool_token(ip->pool, NULL, NULL);
        if (!r || !r->hv) break;
        vsa_bind_perspective(ip->pool, content->hv, r->hv);
        token_add_assoc_unique(r, holder);
        token_add_assoc_unique(r, content);
        r->perspective_of = holder;
        interp_push(ip, r);
        break;
    }
    case PK_TIME_NOW: {
        char name[64];
        snprintf(name, sizeof name, "[时刻:%llu]",
                 (unsigned long long)ip->pool->current_tick);
        Token *now = pool_token_intern(ip->pool, name);
        interp_push(ip, now);
        break;
    }
    case PK_TIME_BEFORE: {
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        if (a && b) {
            Token *rel = pool_token_intern(ip->pool, "[关系:之前]");
            token_add_assoc_unique(a, rel);
            token_add_assoc_unique(a, b);
            if (!a->next_in_time) a->next_in_time = b;
        }
        break;
    }
    case PK_TIME_AFTER: {
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        if (a && b) {
            Token *rel = pool_token_intern(ip->pool, "[关系:之后]");
            token_add_assoc_unique(a, rel);
            token_add_assoc_unique(a, b);
            if (!b->next_in_time) b->next_in_time = a;
        }
        break;
    }
    case PK_TIME_CHAIN: {
        size_t cnt = ip->n - ip->stmt_start;
        if (cnt < 2) break;
        for (size_t i = ip->stmt_start; i + 1 < ip->n; ++i) {
            Token *a = ip->stack[i];
            Token *b = ip->stack[i + 1];
            if (a && b) {
                if (!a->next_in_time) a->next_in_time = b;
                Token *rel = pool_token_intern(ip->pool, "[关系:之后]");
                token_add_assoc_unique(a, rel);
                token_add_assoc_unique(a, b);
            }
        }
        break;
    }
    case PK_BIND: {
        Token *b = interp_pop(ip);
        Token *a = interp_pop(ip);
        if (!a || !b || !a->hv || !b->hv) break;
        Token *r = pool_token(ip->pool, NULL, NULL);
        if (!r || !r->hv) break;
        vsa_bind(a->hv, b->hv, r->hv);
        token_add_assoc_unique(r, a);
        token_add_assoc_unique(r, b);
        interp_push(ip, r);
        break;
    }
    case PK_UNBIND: {
        Token *role  = interp_pop(ip);
        Token *bound = interp_pop(ip);
        if (!role || !bound || !role->hv || !bound->hv) break;
        Token *r = pool_token(ip->pool, NULL, NULL);
        if (!r || !r->hv) break;
        vsa_unbind(bound->hv, role->hv, r->hv);
        token_add_assoc_unique(r, role);
        token_add_assoc_unique(r, bound);
        interp_push(ip, r);
        break;
    }
    case PK_BUNDLE: {
        size_t cnt = ip->n - ip->stmt_start;
        if (cnt == 0) break;
        const int8_t **vecs = (const int8_t **)malloc(cnt * sizeof *vecs);
        if (!vecs) break;
        size_t m = 0;
        for (size_t i = ip->stmt_start; i < ip->n; ++i)
            if (ip->stack[i]->hv) vecs[m++] = ip->stack[i]->hv;
        Token *r = pool_token(ip->pool, NULL, NULL);
        if (!r || !r->hv) { free(vecs); break; }
        vsa_bundle(vecs, m, r->hv);
        for (size_t i = ip->stmt_start; i < ip->n; ++i)
            token_add_assoc_unique(r, ip->stack[i]);
        free(vecs);
        ip->n = ip->stmt_start;
        interp_push(ip, r);
        break;
    }
    case PK_QUERY: {
        Token *q = interp_pop(ip);
        if (!q || !q->hv) break;
        float sim = 0;
        Token *match = pool_query_vsa(ip->pool, q->hv, 0.15f, &sim);
        if (match) interp_push(ip, match);
        break;
    }
    case PK_EXTRACT_REL: {
        Token *pair = interp_pop(ip);
        Token *item = interp_pop(ip);
        if (!pair || !item || !pair->hv || !item->hv) break;
        Token *r = pool_token(ip->pool, NULL, NULL);
        if (!r || !r->hv) break;
        vsa_unbind(pair->hv, item->hv, r->hv);
        token_add_assoc_unique(r, pair);
        token_add_assoc_unique(r, item);
        interp_push(ip, r);
        break;
    }
    case PK_APPLY_REL: {
        Token *rel = interp_pop(ip);
        Token *item = interp_pop(ip);
        if (!rel || !item || !rel->hv || !item->hv) break;
        Token *r = pool_token(ip->pool, NULL, NULL);
        if (!r || !r->hv) break;
        vsa_bind(item->hv, rel->hv, r->hv);
        token_add_assoc_unique(r, rel);
        token_add_assoc_unique(r, item);
        interp_push(ip, r);
        break;
    }
    default:
        break;
    }
}

/* ============================ 序列驱动 ============================ */

static inline size_t interp_step(Interp *ip, Token **prog, size_t n, size_t pc) {
    Token *t = prog[pc];
    PrimKind k = prim_kind(t);
    if (ip->skip_depth > 0) {
        if (k == PK_IF) ip->skip_depth++;
        else if (k == PK_ELSE && ip->skip_depth == 1) ip->skip_depth = 0;
        else if (k == PK_END) ip->skip_depth--;
        return 1;
    }
    if (k == PK_OBJECT || k == PK_CREATE || k == PK_RUN || k == PK_INSERT_NAMED) {
        Token *param = (pc + 1 < n) ? prog[pc + 1] : t;   /* 下一个词是参数（对象/名字） */
        interp_exec_prim(ip, t, param);
        return 2;
    }
    if (k == PK_NONE) { interp_push(ip, t); return 1; }
    interp_exec_prim(ip, t, NULL);
    return 1;
}
static inline void token_run_env(Interp *ip, Token **prog, size_t n) {
    size_t pc = 0;
    while (pc < n) {
        size_t consumed = interp_step(ip, prog, n, pc);
        if (consumed == 0) consumed = 1;
        pc += consumed;
    }
}
static inline void token_run_ctx(TokenPool *p, Token **prog, size_t n,
                                 InterpOpFn op_fn, void *op_user) {
    Interp ip = {0};
    ip.pool = p;
    ip.op_fn = op_fn;          /* 宿主：把 Token 行为交出去落地（NULL = 原语不生效） */
    ip.op_user = op_user;
    token_run_env(&ip, prog, n);
    free(ip.stack);
}
static inline void token_run(TokenPool *p, Token **prog, size_t n) {
    token_run_ctx(p, prog, n, NULL, NULL);
}
static inline void token_run_names_ctx(TokenPool *p, const char *const *names, size_t n,
                                        InterpOpFn op_fn, void *op_user) {
    if (!p || !names || n == 0) return;
    Token **prog = (Token **)malloc(n * sizeof *prog);
    if (!prog) return;
    for (size_t i = 0; i < n; ++i) prog[i] = pool_token_intern(p, names[i]);
    token_run_ctx(p, prog, n, op_fn, op_user);
    free(prog);
}
/* 不带宿主的版本：Token 行为的原语**不生效**（语言层不自己改图） */
static inline void token_run_names(TokenPool *p, const char *const *names, size_t n) {
    token_run_names_ctx(p, names, n, NULL, NULL);
}

/* ============================ 图驱动 + 选择器 ============================ */

static inline Token *focus_selector_default(Token *f, Token *const *ctx,
                                            size_t n_ctx, float energy, void *user) {
    (void)user;
    return token_associate(f, ctx, n_ctx, energy);
}
static inline Token *focus_selector_top_hits(Token *f, Token *const *ctx,
                                             size_t n_ctx, float energy, void *user) {
    (void)user; (void)energy;
    if (!f || f->count == 0) return NULL;
    AssocEntry *best = NULL;
    for (size_t i = 0; i < f->count; ++i) {
        AssocEntry *e = &f->assocs[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        if (!best || e->hits > best->hits) best = e;
    }
    return best ? best->main : NULL;
}
static inline Token *focus_selector_random(Token *f, Token *const *ctx,
                                           size_t n_ctx, float energy, void *user) {
    uint32_t *rng = (uint32_t *)user;
    if (!rng) return NULL;
    return token_associate_weighted(f, ctx, n_ctx, energy, rng);
}
static inline int focus_step(FocusRun *fr, Token *const *ctx, size_t n_ctx, float energy) {
    Token *f = fr->focus;
    if (!f) return 1;
    if (fr->trace_n == fr->trace_cap) {
        size_t ncap = fr->trace_cap ? fr->trace_cap * 2 : 16;
        Token **nt = (Token **)realloc(fr->trace, ncap * sizeof *nt);
        if (nt) { fr->trace = nt; fr->trace_cap = ncap; }
    }
    if (fr->trace_n < fr->trace_cap) fr->trace[fr->trace_n++] = f;
    PrimKind k = prim_kind(f);
    if (k != PK_NONE) {
        Token *param = NULL;
        if (k == PK_OBJECT || k == PK_CREATE || k == PK_RUN)
            param = token_associate(f, ctx, n_ctx, energy);
        fr->base.ctx = (Token **)ctx;
        fr->base.n_ctx = n_ctx;
        fr->base.energy = energy;
        interp_exec_prim(&fr->base, f, param);
    }
    Token *next = fr->selector(f, ctx, n_ctx, energy, fr->selector_user);
    fr->focus = next;
    fr->steps++;
    return next ? 0 : 1;
}
static inline void token_run_focus(TokenPool *p, Token *start,
                                   Token *const *ctx, size_t n_ctx,
                                   float energy, size_t max_steps, FocusRun *out) {
    FocusRun local = {0};
    FocusRun *fr = out ? out : &local;
    if (fr->base.pool == NULL) fr->base.pool = p;
    if (!fr->selector) fr->selector = focus_selector_default;
    fr->focus = start;
    fr->steps = 0;
    while (fr->steps < max_steps)
        if (focus_step(fr, ctx, n_ctx, energy) != 0) break;
    if (!out) { free(fr->base.stack); free(fr->trace); }
}
static inline void focus_run_free(FocusRun *fr) {
    if (!fr) return;
    free(fr->base.stack); free(fr->trace);
    fr->base.stack = NULL; fr->trace = NULL;
    fr->base.n = fr->base.cap = 0;
    fr->trace_n = fr->trace_cap = 0;
}

/* ============================ 激活 → 原语（桥） ============================ */

typedef struct { FocusRun *fr; Token **ctx; size_t n_ctx; float energy; } LangActivate;

static inline void lang_on_visit(Token *t, size_t depth, float energy, void *user) {
    (void)depth;
    LangActivate *la = (LangActivate *)user;
    PrimKind k = prim_kind(t);
    if (k == PK_NONE) return;
    Token *param = NULL;
    if ((k == PK_OBJECT || k == PK_CREATE || k == PK_RUN) && la->ctx)
        param = token_associate(t, (Token *const *)la->ctx, la->n_ctx, energy);
    if (la->fr) {
        la->fr->base.ctx = la->ctx;
        la->fr->base.n_ctx = la->n_ctx;
        la->fr->base.energy = energy;
        interp_exec_prim(&la->fr->base, t, param);
    } else {
        Interp tmp = {0};
        tmp.pool = NULL; tmp.ctx = la->ctx;
        tmp.n_ctx = la->n_ctx; tmp.energy = energy;
        interp_exec_prim(&tmp, t, param);
        free(tmp.stack);
    }
}
static inline void token_activate_lang(TokenPool *p, Token *start,
                                       Token **ctx, size_t n_ctx,
                                       float energy, size_t max_depth, FocusRun *fr) {
    FocusRun local = {0};
    FocusRun *use = fr ? fr : &local;
    if (use->base.pool == NULL) use->base.pool = p;
    LangActivate la = { use, ctx, n_ctx, energy };
    ActivateCtx act;
    activate_ctx_init(&act, 0.7f, 0.05f, max_depth);
    act.on_visit = lang_on_visit;
    act.user = &la;
    token_activate_from(start, (Token *const *)ctx, n_ctx, energy, &act);
    activate_ctx_free(&act);
    if (!fr) free(local.base.stack);
}

#endif /* TOKEN_LANG_H */