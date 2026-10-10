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

/* 原语**命名空间**的前缀：只写一份。
 * 校验器靠它把「打错的原语名」和「普通数据 token」分开 —— 别的 `[xxx:yyy]`
 * （`[符号:好]`、`[时刻:12]`、`[关系:之前]`…）都是运行时的**数据**，不许当成原语名报错。 */
#define PRIM_PREFIX       "[原语:"
#define PRIM_PREFIX_LEN   (sizeof(PRIM_PREFIX) - 1)

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

/* **原语表：名字只写一份；栈效应同一张表里写。**
 * 以前名字写了两处（这里的 if 链 + pool_init_prims 的清单），加一条原语要改两处 ——
 * 「同一份清单出现在两处」正是设计原则要清掉的东西。现在一张 name→kind 表：
 * 识别走它、intern 也走它；新增一条原语 = 表里加一行 + 写它的执行分支。
 *
 * 每一行还带**栈效应**（干跑校验器 `token_check_program()` 按它算）：
 *   need  至少要几个操作数才**真的做事**（栈上不够 = 操作数不够）
 *   pop   做那一手时从栈上拿走几个（不够就"有多少拿多少"—— 代码里就是 `interp_pop` 返回 NULL）
 *   push  至少放回几个（净效果 = push − pop）
 *   flags PF_*（见下）
 *
 * ⚠ **这张表按 `interp_exec_prim` 的真实行为抄，不按名字猜**（§6.58）：
 *   `Interp::stmt_start` 全仓从没被赋值过 ⇒ **恒为 0**。所以
 *     · `[原语:分割]` 的真实行为是**清空整栈**（不是"退回本语句开头"）；
 *     · `[原语:包裹]/[原语:嵌套]/[原语:捆绑]` 的真实行为是**把整栈折成一个**
 *       （栈上不够 need 时它们 `break`，**什么都不做**）。
 *   名字暗示"按语句分段"，实际没有分段这个概念。**行为不改**（改了会动一堆看护），
 *   校验器照真实行为判 —— 否则会认出假错误，"认错比不认更糟"。
 * 另：`绑定/解绑/认为/抽关系/套关系` 那几条在 `!hv` 时不推结果 —— 那是**池子能力**
 * 决定的（正常池子每个 Token 都有 hv），不算"看数据"，所以表里按"推"记；
 * 真正看数据的是 `展开`（推 0~2）与 `检索`（中没中），它们标 PF_INEXACT。 */
typedef enum {
    PF_NONE     = 0,
    PF_ARG      = 1u << 0,   /* 吃掉**下一个词**当参数（序列驱动：对象/创建/运行/插词为） */
    PF_OPEN     = 1u << 1,   /* 如果：开一层块 */
    PF_MID      = 1u << 2,   /* 否则：块中间 */
    PF_CLOSE    = 1u << 3,   /* 结束：关一层块 */
    PF_CLEAR    = 1u << 4,   /* 整栈折叠/清空（真实行为，见上；pop/push 栏对它无意义） */
    PF_INEXACT  = 1u << 5,   /* 推几个**看数据** ⇒ 从那一步起栈深不确定（校验器不再查操作数） */
} PrimFlags;

typedef struct {
    const char *name;
    PrimKind    kind;
    signed char need;        /* 至少几个操作数 */
    signed char pop;         /* 拿走几个 */
    signed char push;        /* 放回几个 */
    unsigned char flags;     /* PF_* */
} PrimDef;

static const PrimDef PRIMS[] = {
    /*                          name                  kind                  need pop push flags */
    { PRIM_OBJECT,       PK_OBJECT,        0, 0, 1, PF_ARG },   /* 要的是"下一个词"，不是栈上操作数 */
    { PRIM_GOOD,         PK_GOOD,          1, 0, 0, PF_NONE },     /* 只看栈顶（不改栈） */
    { PRIM_BAD,          PK_BAD,           1, 0, 0, PF_NONE },
    { PRIM_SPLIT,        PK_SPLIT,         0, 0, 0, PF_CLEAR },    /* 真实行为：清空整栈 */
    { PRIM_REQUEST,      PK_REQUEST,       0, 0, 0, PF_NONE },     /* 栈空就拿原语自己兜底 */
    { PRIM_EXECUTE,      PK_EXECUTE,       0, 0, 0, PF_NONE },
    { PRIM_REFUSE,       PK_REFUSE,        0, 0, 0, PF_NONE },
    { PRIM_WRAP,         PK_WRAP,          1, 0, 0, PF_CLEAR },    /* 真实行为：整栈折成一个 */
    { PRIM_PRE_ASSOC,    PK_PRE_ASSOC,     1, 1, 0, PF_NONE },
    { PRIM_CONNECT,      PK_CONNECT,       2, 2, 0, PF_NONE },
    { PRIM_DISCONNECT,   PK_DISCONNECT,    2, 2, 0, PF_NONE },
    { PRIM_CREATE,       PK_CREATE,        0, 0, 1, PF_ARG },
    { PRIM_RUN,          PK_RUN,           0, 0, 0, PF_ARG },     /* 参数是"跑哪个名字"，不进栈 */
    { PRIM_IF,           PK_IF,            1, 1, 0, PF_OPEN },
    { PRIM_ELSE,         PK_ELSE,          0, 0, 0, PF_MID },
    { PRIM_END,          PK_END,           0, 0, 0, PF_CLOSE },
    { PRIM_ACTIVATE,     PK_ACTIVATE,      1, 0, 0, PF_NONE },     /* 只看栈顶 */
    { PRIM_NEST,         PK_NEST,          2, 0, 0, PF_CLEAR },    /* 栈上不够 2 个：什么也不做 */
    { PRIM_EXPAND,       PK_EXPAND,        1, 1, 1, PF_INEXACT },  /* 推 0~2 个 → 栈深不确定 */
    { PRIM_PERSPECTIVE,  PK_PERSPECTIVE,   1, 1, 1, PF_NONE },     /* pop 之后又 push 回去 */
    { PRIM_BELIEVE,      PK_BELIEVE,       1, 1, 1, PF_NONE },
    { PRIM_QUOTE,        PK_QUOTE,         1, 1, 1, PF_NONE },
    { PRIM_TIME_NOW,     PK_TIME_NOW,      0, 0, 1, PF_NONE },
    { PRIM_TIME_BEFORE,  PK_TIME_BEFORE,   2, 2, 0, PF_NONE },
    { PRIM_TIME_AFTER,   PK_TIME_AFTER,    2, 2, 0, PF_NONE },
    { PRIM_TIME_CHAIN,   PK_TIME_CHAIN,    2, 0, 0, PF_NONE },     /* 整栈成链，不改栈 */
    { PRIM_BIND,         PK_BIND,          2, 2, 1, PF_NONE },
    { PRIM_UNBIND,       PK_UNBIND,        2, 2, 1, PF_NONE },
    { PRIM_BUNDLE,       PK_BUNDLE,        1, 0, 0, PF_CLEAR },    /* 真实行为：整栈折成一个 */
    { PRIM_QUERY,        PK_QUERY,         1, 1, 1, PF_INEXACT },  /* 没中就只推 0 个 */
    { PRIM_EXTRACT_REL,  PK_EXTRACT_REL,   2, 2, 1, PF_NONE },
    { PRIM_APPLY_REL,    PK_APPLY_REL,     2, 2, 1, PF_NONE },
    { PRIM_PRE_ASSOC_NOT, PK_PRE_ASSOC_NOT, 1, 1, 0, PF_NONE },    /* 反前提（[原语:前关联不]） */
    { PRIM_PREMISE_TIE,  PK_PREMISE_TIE,   3, 3, 0, PF_NONE },     /* Token 行为入口（见文件头注释） */

    { PRIM_INSERT,       PK_INSERT,        2, 2, 0, PF_NONE },
    { PRIM_INSERT_NAMED, PK_INSERT_NAMED,  2, 2, 0, PF_ARG },      /* 多一个名字当参数 */
    { PRIM_COLLECT_TEMP, PK_COLLECT_TEMP,  0, 0, 0, PF_NONE },
    { NULL, PK_NONE, 0, 0, 0, PF_NONE }
};
/* 表里的条数（不算结尾那条空行）—— 别的清单别再数一遍 */
#define PRIM_N ((sizeof PRIMS / sizeof PRIMS[0]) - 1)

/* 符号（不是原语，但要 intern 进池子，切词器和 ::asm 才认得出） */
static const char *const PRIM_SYMS[] = {
    SYM_GOOD, SYM_BAD, SYM_WRAP, SYM_REFUSED, NULL
};

static inline size_t prim_count(void) {
    size_t n = 0;
    while (PRIMS[n].name) n++;
    return n;
}

/* 按 kind 找表里那一行（栈效应、flags 都从这儿取 —— 只有这一张表） */
static inline const PrimDef *prim_def_of(PrimKind k) {
    for (size_t i = 0; PRIMS[i].name; ++i)
        if (PRIMS[i].kind == k) return &PRIMS[i];
    return NULL;
}
static inline const PrimDef *prim_def_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; PRIMS[i].name; ++i)
        if (strcmp(name, PRIMS[i].name) == 0) return &PRIMS[i];
    return NULL;
}
/* 这个原语在**序列驱动**下吃不吃下一个词当参数（`interp_step` 与校验器同一判据）。
 * 注意 `focus_step` / `lang_on_visit` 那两条路没有"下一个词"，参数是从图上联想来的
 * （它们用的是对象/创建/运行三条 + 图上取参数），所以不走这个函数。 */
static inline int prim_eats_arg(PrimKind k) {
    const PrimDef *d = prim_def_of(k);
    return d && (d->flags & PF_ARG);
}

/* 把一行的栈效应渲染成人话（`::prims` 用）—— 措辞与校验器**同一张表**，
 * 名字与真实行为不一致的地方（分割/包裹/嵌套/捆绑）在这儿就写明白，别让人按名字猜。 */
static inline const char *prim_effect_text(PrimKind k) {
    static char buf[192];
    const PrimDef *d = prim_def_of(k);
    if (!d) { snprintf(buf, sizeof buf, "（表里没有）"); return buf; }
    const char *note = "";
    if (d->flags & PF_ARG)          note = "，吃下一个词当参数";
    else if (d->flags & PF_CLEAR)   note = (d->kind == PK_SPLIT) ? "（真实行为：清空整栈）"
                                                                 : "（真实行为：整栈折成一个）";
    else if (d->flags & PF_OPEN)    note = "，开一层块";
    else if (d->flags & PF_MID)     note = "，块中";
    else if (d->flags & PF_CLOSE)   note = "，关一层块";
    else if (d->flags & PF_INEXACT) note = "，推几个看数据 ⇒ 栈深不确定";
    snprintf(buf, sizeof buf, "栈：要 %d 拿 %d 放 %d%s",
             (int)d->need, (int)d->pop, (int)d->push, note);
    return buf;
}

/* 按**名字**认原语。校验器走的就是这一条：它拿到的是一串词，还没有 Token
 * （干跑不许 intern —— 一 intern 就等于把打错的名字当场造成新 Token，
 * 那正是今天"错名字被静默吞掉"的机制本身）。 */
static inline PrimKind prim_kind_name(const char *name) {
    const PrimDef *d = prim_def_by_name(name);   /* 同一张表，不再抄一遍循环 */
    return d ? d->kind : PK_NONE;
}

static inline PrimKind prim_kind(const Token *t) {
    return prim_kind_name(t ? t->name : NULL);
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

/* ============================ 干跑校验（解析器/校验器） ============================
 *
 * 用户 2026-10-07 的线 A ④「语法约束/校验」：**执行之前**先干跑一遍，把
 * 「打错的原语名 / 括号不配对 / 操作数不够 / 如果-结束不配对 / 空程序」认出来。
 * 今天没有这一步：`::asm` 里写 `[原语:连结]`（错字）会被当成**普通数据词**压进栈、
 * 一声不响 —— 错名字被静默吞掉，正是"看起来跑了、其实没跑"那类坑。
 *
 * 三条硬规矩（都是本项目已经踩出来的）：
 *   ① **干跑不许 intern**：只按名字查表（`prim_def_by_name`）。一 intern 就等于把
 *      打错的名字当场造成一个新 Token —— 校验器自己把那件事做成了。
 *   ② **只按代码真实行为判**（§6.58）：栈效应来自上面那张表，名字与行为不一致的地方
 *      （分割=清空整栈、包裹/嵌套/捆绑=整栈折成一个）就按真实行为写，别按名字猜。
 *   ③ **不确定就不判**：`展开/检索` 推几个看数据 ⇒ 从那一步起栈深不确定，
 *      后面的操作数检查**跳过并记账**（认错比不认更糟）。
 *
 * 判据一共六条（tests/asmcheck.sh 一条一条钉着）：
 *   ① 未知原语名 ② 括号不配对 ③ 操作数不够 ④ 如果-结束不配对 ⑤ 空程序 ⑥ 参数缺失
 * 只认 `[原语:` 前缀的词当原语名 —— 别的 `[xxx:yyy]`（`[符号:好]`/`[时刻:12]`/`[关系:之前]`）
 * 都是运行时的**数据**，不许当原语报错。 */

#define TOKEN_CHECK_MAX_ERR  16      /* 最多记几条明细（多的只计数） */
#define TOKEN_CHECK_MSG_CAP  192
#define TOKEN_CHECK_BLOCK_MAX 16     /* 如果-结束 的嵌套上限（再深只报"太深"） */

typedef struct {
    size_t      at_i;                        /* 第几个词（0 起；(size_t)-1 = 不属于某个词） */
    const char *at;                          /* 出问题的那个词（指调用方的词表，不拷贝） */
    char        msg[TOKEN_CHECK_MSG_CAP];
} TokenCheckErr;

typedef struct {
    size_t        n_tok;                     /* 词数 */
    int           n_err;                     /* 错误条数（0 = 通过） */
    size_t        n_show;                    /* 记了明细几条（≤ TOKEN_CHECK_MAX_ERR） */
    TokenCheckErr err[TOKEN_CHECK_MAX_ERR];
    int           depth;                     /* 干跑末态栈深（下界）；-1 = 不确定 */
    size_t        uncertain_at;              /* 从第几个词起栈深不确定（(size_t)-1 = 一直确定） */
    size_t        n_skip;                    /* 因为不确定而跳过的操作数检查次数 */
    size_t        blocks_left;               /* 收尾时还开着的块数 */
} TokenCheck;

static inline void token_check_add(TokenCheck *c, size_t at_i, const char *at, const char *msg) {
    if (!c) return;
    if ((size_t)c->n_err < TOKEN_CHECK_MAX_ERR) {
        c->err[c->n_err].at_i = at_i;
        c->err[c->n_err].at   = at;
        snprintf(c->err[c->n_err].msg, TOKEN_CHECK_MSG_CAP, "%s", msg ? msg : "");
        c->n_err++;
        c->n_show = (size_t)c->n_err;
    } else {
        c->n_err++;                          /* 只计数（汇总行会说"还有 N 条"） */
    }
}

/* `[`/`]` 在**一个词里**必须成对（`[原语:对象]` 这种整体词才是完整的原语） */
static inline void token_check_brackets(TokenCheck *c, size_t i, const char *w) {
    int bal = 0;
    for (const char *q = w; *q; ++q) {
        if (*q == '[') bal++;
        else if (*q == ']' && --bal < 0) break;
    }
    if (bal != 0) {
        char msg[TOKEN_CHECK_MSG_CAP];
        snprintf(msg, sizeof msg, "括号不配对：`%s`（`[` 与 `]` 要成对）", w);
        token_check_add(c, i, w, msg);
    }
}

static inline int token_check_program(const char *const *names, size_t n, TokenCheck *out) {
    if (out) {
        memset(out, 0, sizeof *out);
        out->depth = 0;
        out->uncertain_at = (size_t)-1;
        out->n_tok = n;
    }
    if (!out) return 0;
    if (n == 0) { token_check_add(out, (size_t)-1, "", "空程序：一个词都没有"); return out->n_err; }

    int    d = 0;                             /* 已知栈深（≥0）；-1 = 不确定 */
    size_t open_pos[TOKEN_CHECK_BLOCK_MAX];   /* 还开着的 如果 在第几个词 */
    size_t n_open = 0;
    int    eats_next = 0;                     /* 上一个词是 PF_ARG：这个词是它的参数 */

    for (size_t i = 0; i < n; ++i) {
        const char *w = names[i] ? names[i] : "";
        token_check_brackets(out, i, w);

        int is_param = eats_next;
        eats_next = 0;
        if (is_param) continue;               /* 参数词由上一个原语吃掉，不单独进栈 */

        const PrimDef *pd = NULL;
        if (strncmp(w, PRIM_PREFIX, PRIM_PREFIX_LEN) == 0 ||
            strncmp(w, "[原语", 7) == 0) {    /* 认得的前缀写错（少冒号）也算原语名 */
            pd = prim_def_by_name(w);
            if (!pd) {
                char msg[TOKEN_CHECK_MSG_CAP];
                snprintf(msg, sizeof msg,
                         "未知原语名：`%s`（表里没有这种写法；干跑**不**intern，"
                         "不会当场造一个 Token 把错字吞掉）", w);
                token_check_add(out, i, w, msg);
                continue;                     /* 认不出就不猜它的栈效应 */
            }
        }
        if (!pd) { if (d >= 0) d++; continue; }   /* 普通数据词：进栈 */

        if (pd->flags & PF_ARG) {             /* 对象/创建/运行/插词为：吃掉下一个词 */
            if (i + 1 >= n) {
                char msg[TOKEN_CHECK_MSG_CAP];
                snprintf(msg, sizeof msg,
                         "参数缺失：`%s` 后面没有词了（真实行为是拿它自己当参数）", w);
                token_check_add(out, i, w, msg);
            } else {
                eats_next = 1;
            }
        }

        /* 块配对：如果 / 否则 / 结束 */
        if (pd->flags & PF_OPEN) {
            if (n_open < TOKEN_CHECK_BLOCK_MAX) open_pos[n_open] = i;
            n_open++;
        } else if (pd->flags & PF_MID) {
            if (n_open == 0) token_check_add(out, i, w, "否则 没有对应的 如果");
        } else if (pd->flags & PF_CLOSE) {
            if (n_open == 0) token_check_add(out, i, w, "结束 没有对应的 如果");
            else n_open--;
        }

        if (d < 0) { out->n_skip++; continue; }   /* 栈深不确定：只查名字/配对/括号 */

        if (d < pd->need) {
            char msg[TOKEN_CHECK_MSG_CAP];
            snprintf(msg, sizeof msg, "操作数不够：`%s` 要 %d 个操作数，栈上只有 %d 个",
                     w, (int)pd->need, d);
            token_check_add(out, i, w, msg);
            /* 真实行为：不够就"有多少拿多少"（`interp_pop` 空栈返回 NULL），推的部分不发生 */
            d = (d > pd->pop) ? d - pd->pop : 0;
            continue;
        }
        if (pd->flags & PF_CLEAR) {
            /* 真实行为（stmt_start 恒 0）：分割 = 清空整栈；包裹/嵌套/捆绑 = 整栈折成一个 */
            d = (pd->kind == PK_SPLIT) ? 0 : 1;
        } else {
            d = d - pd->pop + pd->push;
            if (pd->flags & PF_INEXACT) {
                d = -1;                       /* 从那一步起栈深不确定 */
                if (out->uncertain_at == (size_t)-1) out->uncertain_at = i;
            }
        }
    }

    out->depth = d;
    out->blocks_left = n_open;
    if (n_open > 0) {
        char msg[TOKEN_CHECK_MSG_CAP];
        size_t first = (n_open <= TOKEN_CHECK_BLOCK_MAX) ? open_pos[0] : 0;
        snprintf(msg, sizeof msg,
                 "如果-结束不配对：还开着 %zu 层（第 %zu 个词那个 `%s` 没等到 `%s`）",
                 n_open, first + 1, PRIM_IF, PRIM_END);
        token_check_add(out, first, names[first] ? names[first] : "", msg);
    }
    return out->n_err;
}

/* 校验结果的呈现（`::asm check on` 走这一条；只有这一处拼） */
static inline void token_check_print(const TokenCheck *c) {
    if (!c) return;
    if (c->n_err == 0) {
        printf("[校验] 通过：%zu 个词", c->n_tok);
        if (c->depth >= 0) printf("，干跑末态栈深 %d", c->depth);
        else               printf("，干跑末态栈深不确定（`展开`/`检索` 推几个看数据）");
        if (c->n_skip)     printf("，跳过 %zu 次操作数检查", c->n_skip);
        printf("\n");
        return;
    }
    printf("[校验] 没过：%d 条问题（干跑**没执行**，图一个字都没动）\n", c->n_err);
    for (size_t i = 0; i < c->n_show; ++i) {
        if (c->err[i].at_i == (size_t)-1) printf("  %zu) %s\n", i + 1, c->err[i].msg);
        else printf("  %zu) 第 %zu 个词：%s\n", i + 1, c->err[i].at_i + 1, c->err[i].msg);
    }
    if ((size_t)c->n_err > c->n_show)
        printf("  …还有 %d 条（只列前 %d 条）\n",
               c->n_err - (int)c->n_show, TOKEN_CHECK_MAX_ERR);
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
    if (prim_eats_arg(k)) {      /* 判据来自原语表（PF_ARG），不再另抄一份 kind 清单 */
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