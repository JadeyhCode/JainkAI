/* rule.h — **规则即数据 + 有界后向链 + 证明**（阶段二 WP2；§6.635）
 *
 * 目标：让"会根据已有条件去证明新的东西"落地，而且**不扩图**。
 *
 * 表示（全部复用现有节点/边）：
 *   · 规则节点 `[规则:R<N>]`（内核内部命名空间，随图存盘）；
 *   · `[规则:R<N>] --[结论]--> 谓词模式`，`--[前提K]--> 谓词模式`（`结论`/`前提K` 是动态关系）；
 *   · 谓词模式就是 pred.h 的实例节点，只是参数可以是**变量名**（字面 `$x`），例如
 *     `[谓词:祖先($x,$z)]`；
 *   · 事实就是**没有变量**的实例节点（`[谓词:父(甲,乙)]`）。
 *
 * 求解：**后向链**（goal 驱动）—— 目标先看是不是事实，再看有没有规则的"结论"与它匹配；
 * 匹配 = 把规则头里的 `$变量` 绑定到目标参数；然后逐个证明前提（变量已经绑定就用绑定值）。
 *
 * 三条硬边界（写死，避免"看起来能证明"）：
 *   1. **只有 Horn 规则**：前提全为正文字，没有否定、没有函数符号、没有等式/不等式；
 *   2. **有界**：深度（默认 8）、步数（默认 32）、循环检测（同一目标在栈上重复 ⇒ 该分支失败）；
 *   3. 只回答三种结局：**证明了 / 证不出来（未知）/ 结构坏了（规则或实例不自洽）**——
 *      **绝不把"证不出来"说成"假"**（开放世界，和整个项目一致）。
 */
#ifndef RULE_H
#define RULE_H

#include "pred.h"

#define RULE_PREFIX      MICRO_RULE_TOKEN_PREFIX
#define RULE_MAX_PREMISE 4
#define RULE_MAX_VARS    16
#define RULE_MAX_DEPTH    8
#define RULE_MAX_STEPS   32

static inline int rule_is_rule_name(const char *n) {
    return n && strncmp(n, RULE_PREFIX, strlen(RULE_PREFIX)) == 0;
}

/* ---- 原子/文本 ---------------------------------------------------------- */

/* `名字(参数1,参数2)` —— 参数可以是 `$x` */
static inline const char *rule_atom_text(const char *name, char *const *args, size_t n,
                                        char *buf, size_t cap) {
    return pred_full_name(name, args, n, buf, cap);
}

/* 解析 `名字(参数1,参数2)`；name/args 指进 s（会被临时改写的副本），返回 1 = 成功。 */
static inline int rule_parse_atom(char *s, char *name, size_t ncap,
                                  char *args[PRED_MAX_ARGS], size_t *an) {
    if (!s || !name || !args || !an) return 0;
    while (*s == ' ' || *s == '\t') s++;
    char *lp = strchr(s, '(');
    char *rp = strrchr(s, ')');
    if (!lp || !rp || rp < lp) return 0;
    size_t nl = (size_t)(lp - s);
    if (nl == 0 || nl >= ncap) return 0;
    memcpy(name, s, nl); name[nl] = 0;
    *an = 0;
    char *p = lp + 1;
    while (p < rp) {
        while (p < rp && (*p == ' ' || *p == '\t')) p++;
        if (p >= rp) break;
        char *st = p;
        while (p < rp && *p != ',') p++;
        char *en = p;                              /* 指向 ',' 或 ')' */
        while (en > st && (en[-1] == ' ' || en[-1] == '\t')) en--;
        if (en == st || *an >= PRED_MAX_ARGS) return 0;
        /* ⚠ **先算好下一个起点再截断**：先 `*en = 0` 会把 ',' 也写成 0，
         * 于是 `p++` 的判断失效、第二个参数变成空串（第一版 CLI 就是这么坏的）。 */
        char *next = (p < rp && *p == ',') ? p + 1 : NULL;
        *en = 0;
        args[(*an)++] = st;
        if (!next) break;
        p = next;
    }
    return *an > 0;
}

/* ---- 造规则 ------------------------------------------------------------ */

/* 造一条规则：head + 若干 premise（都按"名字 + 参数串"给）。返回规则节点，失败 NULL。 */
static inline Token *rule_add(AI *ai, const char *hname, char *const *hargs, size_t hn,
                              const char **pnames,
                              char *(*pargs)[PRED_MAX_ARGS], const size_t *pns,
                              size_t np) {
    if (!ai || !hname || !hargs || hn == 0 || np == 0 || np > RULE_MAX_PREMISE) return NULL;
    if (!pnames || !pargs || !pns) return NULL;
    /* 规则节点名：`[规则:R<N>]`，N 取现有最大值 + 1（存盘重载后能续上） */
    int maxid = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage || !t->name || !rule_is_rule_name(t->name)) continue;
        /* 名字是 `[规则:R<N>]` —— 跳过前缀和那个 'R' 再取号（第一版忘了跳 R ⇒ 永远返回 R1，
         * 两条规则挤在同一个节点上、前提集被合并，`prove` 就永远证不出来）。 */
        int v = atoi(t->name + strlen(RULE_PREFIX) + 1);
        if (v > maxid) maxid = v;
    }
    char tb[PRED_NAME_CAP];
    /* ① **先把结论/前提的谓词节点都建好**（失败就在这一步回去，别留下一条空规则）*/
    if (!rule_atom_text(hname, hargs, hn, tb, sizeof tb)) return NULL;
    Token *hp = pred_make(ai, hname, hargs, hn);
    if (!hp) return NULL;
    Token *pp[RULE_MAX_PREMISE];
    for (size_t k = 0; k < np; ++k) {
        if (!pnames[k] || !pargs[k] || pns[k] == 0) return NULL;
        pp[k] = pred_make(ai, pnames[k], pargs[k], pns[k]);
        if (!pp[k]) return NULL;
    }
    /* ② 再建规则节点、挂边 */
    char rn[48];
    snprintf(rn, sizeof rn, RULE_PREFIX "R%d]", maxid + 1);
    Token *r = pool_token_intern(ai->pool, rn);
    if (!r) return NULL;
    uint8_t rid_head = rel_id_of(ai, "结论", 1);
    if (!rid_head || ai_write_rel(ai, r, hp, rid_head, 0, AI_WSRC_RELATION) != 0) return NULL;
    for (size_t k = 0; k < np; ++k) {
        char prn[24];
        snprintf(prn, sizeof prn, "前提%zu", k + 1);
        uint8_t rid = rel_id_of(ai, prn, 1);
        if (!rid || ai_write_rel(ai, r, pp[k], rid, 0, AI_WSRC_RELATION) != 0) return NULL;
    }
    return r;
}

/* ---- 读规则 ------------------------------------------------------------ */

/* 规则的结论 / 第 k 个前提（k 从 1 开始）；没有返回 NULL。 */
static inline Token *rule_part(AI *ai, const Token *r, const char *relname, size_t k) {
    if (!ai || !r || !relname) return NULL;
    char rn[24];
    if (k) snprintf(rn, sizeof rn, "%s%zu", relname, k);
    else   snprintf(rn, sizeof rn, "%s", relname);
    uint8_t rid = rel_id_of(ai, rn, 0);
    if (!rid || rid < REL_DYN_BASE) return NULL;
    uint8_t dyn = (uint8_t)(rid - REL_DYN_BASE + 1);
    for (size_t i = 0; i < r->count; ++i) {
        const AssocEntry *e = &token_assocs_ro(r)[i];
        if (e->dyn == dyn && e->main) return e->main;
    }
    return NULL;
}
static inline Token *rule_head(AI *ai, const Token *r) { return rule_part(ai, r, "结论", 0); }
static inline size_t rule_npremise(AI *ai, const Token *r) {
    size_t n = 0;
    for (size_t k = 1; k <= RULE_MAX_PREMISE; ++k) if (rule_part(ai, r, "前提", k)) n = k;
    return n;
}
static inline size_t rule_list(AI *ai, Token **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count && n < cap; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage || !t->name || !rule_is_rule_name(t->name)) continue;
        out[n++] = t;
    }
    return n;
}

/* ---- 求解（后向链） ---------------------------------------------------- */

typedef struct {
    char name[40];
    char args[PRED_MAX_ARGS][40];
    size_t n;
} RuleAtom;

static inline int rule_atom_to_node(AI *ai, const RuleAtom *at, Token **out) {
    if (!ai || !at || !out || at->n == 0 || at->n > PRED_MAX_ARGS) return 0;
    char *a[PRED_MAX_ARGS];
    for (size_t i = 0; i < at->n; ++i) a[i] = (char *)at->args[i];
    Token *p = pred_find(ai, at->name, a, at->n);
    if (!p) return 0;
    *out = p;
    return 1;
}
static inline int rule_node_to_atom(AI *ai, const Token *p, RuleAtom *out) {
    if (!ai || !p || !out || !p->name) return 0;
    char plain[64];
    if (!pred_plain_name(p->name, plain, sizeof plain)) return 0;
    snprintf(out->name, sizeof out->name, "%.39s", plain);
    int ok = 0;
    size_t n = pred_arity(ai, p, &ok);
    if (!ok || n == 0 || n > PRED_MAX_ARGS) return 0;
    out->n = n;
    for (size_t k = 1; k <= n; ++k) {
        Token *v = pred_arg(ai, p, k);
        if (!v || !v->name) return 0;
        snprintf(out->args[k - 1], sizeof out->args[0], "%.39s", v->name);
    }
    return 1;
}

typedef struct { char var[24]; char val[40]; } RuleBind;
typedef struct {
    RuleBind b[RULE_MAX_VARS];
    size_t   n;
} RuleSubst;

static inline const char *rule_lookup(const RuleSubst *s, const char *var) {
    if (!s || !var || var[0] != '$') return NULL;
    for (size_t i = 0; i < s->n; ++i) if (!strcmp(s->b[i].var, var)) return s->b[i].val;
    return NULL;
}
static inline int rule_bind(RuleSubst *s, const char *var, const char *val) {
    if (!s || !var || !val) return 0;
    const char *have = rule_lookup(s, var);
    if (have) return strcmp(have, val) == 0;
    if (s->n >= RULE_MAX_VARS) return 0;
    snprintf(s->b[s->n].var, sizeof s->b[s->n].var, "%.23s", var);
    snprintf(s->b[s->n].val, sizeof s->b[s->n].val, "%.39s", val);
    s->n++;
    return 1;
}
/* 把原子按当前绑定实例化（变量换成值）；还有没绑的变量 ⇒ 返回 0（不能当事实查） */
static inline int rule_ground(const RuleSubst *s, const RuleAtom *at, RuleAtom *out) {
    if (!s || !at || !out) return 0;
    *out = *at;
    for (size_t i = 0; i < out->n; ++i) {
        if (out->args[i][0] != '$') continue;
        const char *v = rule_lookup(s, out->args[i]);
        if (!v) return 0;
        snprintf(out->args[i], sizeof out->args[i], "%.39s", v);
    }
    return 1;
}

typedef struct {
    char  text[RULE_MAX_STEPS * 96];
    size_t used;
    int   depth_limited;    /* 有分支是因为深度/循环被截断的 ⇒ 结论只能算"未知" */
} RuleTrace;

static inline void rule_trace_add(RuleTrace *t, const char *line) {
    if (!t || !line) return;
    /* §6.653 同一条"缺口"会被递归里每一层各记一次 ⇒ 和**上一条**一样就跳过（去重） */
    if ((unsigned char)line[0] == 0xE7 && (unsigned char)line[1] == 0xBC &&
        (unsigned char)line[2] == 0xBA && t->used > 0) {   /* UTF-8 「缺」= E7 BC BA */
        size_t l = strlen(line), start = (t->used > l + 1) ? t->used - l - 1 : 0;
        if (!strncmp(t->text + start, line, l)) return;
    }
    size_t l = strlen(line);
    if (t->used + l + 2 >= sizeof t->text) return;
    memcpy(t->text + t->used, line, l);
    t->used += l;
    t->text[t->used++] = '\n';
    t->text[t->used] = 0;
}

/* ---- 求解内核：goal 驱动 + 合取回溯 ------------------------------------
 * 为什么需要"枚举事实"这一层：`祖先($x,$z) :- 父($x,$y), 祖先($y,$z)` 里前提 1 的 `$y`
 * **还没绑定** —— 只查"具体事实"永远失败（第一版就是这么错的：trace 里只有事实那一行）。
 * 所以：带变量的前提要去**枚举同名同元数的实例**逐条绑定（标准的 join）。
 * 仍有边界：如果某条前提**整体**带未绑定变量、而同名实例一条都没有，就按"未知"失败
 * （不做"目标驱动的枚举推导"，那要另一套机制）。 */
/* 这个实例里有没有变量参数（`$x`）—— **有就不是事实，是模式**。
 * 为什么必须区分：规则头和前提本身也是 `[谓词:父($x,$y)]` 这种节点，它们和图里的事实
 * 混在同一个池子里。第一版把它们也当事实 ⇒ `父` 对**任意参数**都"成立"（`rule_unify`
 * 把 `$x` 绑成了字面的 `$x`），`祖先(甲,丙)` 于是推出无穷长的假链、最后失败。
 */
static inline int rule_instance_has_var(AI *ai, const Token *p) {
    int ok = 0;
    size_t n = pred_arity(ai, p, &ok);
    if (!ok) return 0;
    for (size_t k = 1; k <= n; ++k) {
        Token *v = pred_arg(ai, p, k);
        if (v && v->name && v->name[0] == '$') return 1;
    }
    return 0;
}

static inline int rule_unify(AI *ai, const RuleAtom *at, const Token *p, RuleSubst *s) {
    if (rule_instance_has_var(ai, p)) return 0;      /* 模式节点不是事实 */
    int ok = 0;
    size_t n = pred_arity(ai, p, &ok);
    if (!ok || n != at->n) return 0;
    RuleSubst t = *s;
    for (size_t k = 0; k < n; ++k) {
        Token *v = pred_arg(ai, p, k + 1);
        if (!v || !v->name) return 0;
        if (at->args[k][0] == '$') { if (!rule_bind(&t, at->args[k], v->name)) return 0; }
        else if (strcmp(at->args[k], v->name) != 0) return 0;
    }
    *s = t;
    return 1;
}

static inline int rule_solve(AI *ai, const RuleAtom *goal, RuleSubst *s, int depth,
                             RuleTrace *tr, char stack[][64], size_t sn);

/* 第 k 条前提起，能不能全部满足（成功时 s 里带着绑定） */
static inline int rule_body(AI *ai, const Token *rule, size_t k, RuleSubst *s, int depth,
                            RuleTrace *tr, char stack[][64], size_t sn) {
    size_t np = rule_npremise(ai, rule);
    if (k > np) return 1;
    Token *pp = rule_part(ai, rule, "前提", k);
    RuleAtom pa;
    if (!pp || !rule_node_to_atom(ai, pp, &pa)) return 0;
    /* §6.654 **否定前提（NAF）**：前提名以「不」开头 ⇒ 要求"正面那条**推不出来**"才算成立。
     * 边界（写死）：只在**目标已具体**（变量都绑上）时可用；正面那条一旦触到深度/循环截断，
     * 否定**不算成立**（否则深度一截断就会凭空"证明"出否定 → 假结论）。 */
    int is_neg = ((unsigned char)pa.name[0] == 0xE4 && (unsigned char)pa.name[1] == 0xB8 &&
                  (unsigned char)pa.name[2] == 0x8D);          /* UTF-8 「不」= E4 B8 8D */
    if (is_neg) {
        char base[40];
        snprintf(base, sizeof base, "%.39s", pa.name + 3);
        RuleAtom pos = pa;
        snprintf(pos.name, sizeof pos.name, "%s", base);
        RuleAtom pg;
        if (!rule_ground(s, &pos, &pg)) {
            if (tr) rule_trace_add(tr, "否定前提里有没绑定的变量 ⇒ 这条规则用不了（不猜）");
            return 0;
        }
        RuleSubst fresh2;
        memset(&fresh2, 0, sizeof fresh2);
        RuleTrace sub;
        memset(&sub, 0, sizeof sub);
        int pr = rule_solve(ai, &pg, &fresh2, depth - 1, &sub, stack, sn);
        if (pr) return 0;                        /* 正面成立 ⇒ 否定前提失败 */
        if (sub.depth_limited) {                 /* 截断过 ⇒ 不敢说"没有" */
            if (tr) rule_trace_add(tr, "否定前提撞到深度/循环截断 ⇒ 不算成立（不猜）");
            return 0;
        }
        char nline[200];
        snprintf(nline, sizeof nline, "否定前提成立：%.20s(%.20s) 推不出来", pos.name, pg.args[0]);
        if (tr) rule_trace_add(tr, nline);
        return rule_body(ai, rule, k + 1, s, depth, tr, stack, sn);
    }
    RuleAtom g;
    if (rule_ground(s, &pa, &g)) {
        /* 这条前提已经具体了：当子目标证（事实或规则）。
         * ⚠ **子目标要用空绑定**：不同规则用同名变量（`$x`）很常见，把父层的绑定带进去会让
         * 子目标匹配头时"$x 必须等于甲"从而失败（第一版就是这么错的：内层连 body 都不进）。 */
        RuleSubst fresh;
        memset(&fresh, 0, sizeof fresh);
        RuleSubst s2 = *s;
        return rule_solve(ai, &g, &fresh, depth - 1, tr, stack, sn) &&
               rule_body(ai, rule, k + 1, &s2, depth, tr, stack, sn);
    }
    /* 还没绑全：枚举同名同元数的实例，逐条绑定后继续 */
    Token *inst[256];
    size_t ni = pred_instances(ai, pa.name, inst, 256);
    for (size_t i = 0; i < ni; ++i) {
        RuleSubst s2 = *s;
        if (!rule_unify(ai, &pa, inst[i], &s2)) continue;
        char st2[16][64];
        memcpy(st2, stack, sizeof st2);
        if (rule_body(ai, rule, k + 1, &s2, depth, tr, st2, sn)) {
            /* 把"用了哪条事实"记进证明 —— 否则链里只有规则名，读的人不知道凭什么 */
            char line[160];
            snprintf(line, sizeof line, "选取事实：%.60s", inst[i]->name ? inst[i]->name : "?");
            rule_trace_add(tr, line);
            *s = s2;
            return 1;
        }
    }
    return 0;
}

static inline int rule_solve(AI *ai, const RuleAtom *goal, RuleSubst *s, int depth,
                             RuleTrace *tr, char stack[][64], size_t sn) {
    if (!ai || !goal || depth <= 0) { if (tr) tr->depth_limited = 1; return 0; }
    char key[64];
    snprintf(key, sizeof key, "%.20s(%.18s%.18s)", goal->name,
             goal->args[0], goal->n > 1 ? goal->args[1] : "");
    for (size_t i = 0; i < sn && i < 16; ++i)
        if (!strcmp(stack[i], key)) { if (tr) tr->depth_limited = 1; return 0; }
    RuleAtom g;
    if (rule_ground(s, goal, &g)) {
        Token *p = NULL;
        if (rule_atom_to_node(ai, &g, &p)) {
            char line[160];
            snprintf(line, sizeof line, "事实：%.20s(%.30s%.1s%.30s)", g.name,
                     g.args[0], g.n > 1 ? "," : "", g.n > 1 ? g.args[1] : "");
            rule_trace_add(tr, line);
            return 1;
        }
    }
    Token *rules[64];
    size_t nr = rule_list(ai, rules, 64);
    for (size_t i = 0; i < nr; ++i) {
        Token *head = rule_head(ai, rules[i]);
        RuleAtom ha;
        if (!head || !rule_node_to_atom(ai, head, &ha)) continue;
        if (strcmp(ha.name, goal->name) != 0 || ha.n != goal->n) continue;
        RuleSubst s2 = *s;
        int unify_ok = 1;
        for (size_t k = 0; k < ha.n && unify_ok; ++k) {
            if (ha.args[k][0] == '$') unify_ok = rule_bind(&s2, ha.args[k], goal->args[k]);
            else unify_ok = !strcmp(ha.args[k], goal->args[k]);
        }
        if (!unify_ok) continue;
        char st2[16][64];
        memcpy(st2, stack, sizeof st2);
        if (sn < 16) snprintf(st2[sn], sizeof st2[0], "%s", key);
        RuleSubst s3 = s2;
        if (!rule_body(ai, rules[i], 1, &s3, depth - 1, tr, st2, sn + 1)) {
            /* §6.653 **缺口清单**：规则匹配上了、但前提证不出来 —— 记一句，给 `::prove` 用。
             * 只记"匹配上的规则"（没匹配的规则不是缺口，是无关）。 */
            RuleTrace *tr2 = tr;
            if (tr2) {
                char gline[200];
                snprintf(gline, sizeof gline, "缺口：规则 %.24s（%s）的前提证不出来",
                         rules[i]->name, goal->name);
                rule_trace_add(tr2, gline);
            }
            continue;
        }
        char line[200];
        snprintf(line, sizeof line, "规则 %.24s：%.20s(%.24s%.1s%.24s) ⇐ %zu 条前提",
                 rules[i]->name, ha.name, ha.args[0], ha.n > 1 ? "," : "",
                 ha.n > 1 ? ha.args[1] : "", rule_npremise(ai, rules[i]));
        rule_trace_add(tr, line);
        *s = s3;
        return 1;
    }
    return 0;
}

/* 对外：证明一个**具体**目标（参数都是词，不含变量）。
 * 返回 1 = 证明了（tr 里有步骤）、0 = 证不出来（未知）、-1 = 参数/结构不对。 */
static inline int rule_prove(AI *ai, const char *name, char *const *args, size_t n,
                            char *out, size_t cap) {
    if (!ai || !name || !args || n == 0 || n > PRED_MAX_ARGS) return -1;
    RuleAtom goal;
    memset(&goal, 0, sizeof goal);
    snprintf(goal.name, sizeof goal.name, "%.39s", name);
    goal.n = n;
    for (size_t i = 0; i < n; ++i) {
        if (!args[i] || !args[i][0] || args[i][0] == '$') return -1;   /* 目标必须具体 */
        snprintf(goal.args[i], sizeof goal.args[0], "%.39s", args[i]);
    }
    RuleTrace tr;
    memset(&tr, 0, sizeof tr);
    RuleSubst s;
    memset(&s, 0, sizeof s);
    char stack[16][64];
    memset(stack, 0, sizeof stack);
    int r = rule_solve(ai, &goal, &s, RULE_MAX_DEPTH, &tr, stack, 0);
    if (out && cap) snprintf(out, cap, "%.*s", (int)(cap - 1), tr.text);
    if (r == 1) return 1;
    return tr.depth_limited ? 0 : 0;    /* 未知就是未知（不把"证不出来"说成假） */
}

/* §6.649 **规则体检**：只看三件事（都是"写了但推不出东西"的典型）：
 *   ① **安全条件**：规则头里的每个 `$变量` 必须在前提里也出现（Datalog 的 safety）——
 *      否则"证明"会拿一个没绑定的变量去当值；
 *   ② **重复规则**：头和前提集合完全一样的两条规则（纯冗余）。
 * ⚠ 自环（`环($x) :- 环($x)`）**没有**做成本项检查：第一版写了、但查不出来（节点比较没命中），
 *   而且求解器的**栈检测**本来就能拦（返回未知、不挂死，见 §6.635 的探针）—— 如实不承诺。
 * 返回 0 = 全部合格；>0 = 问题条数。问题描述写进 out（可空）。 */
static inline int rule_check(AI *ai, char *out, size_t cap) {
    if (!ai) return 0;
    Token *rs[128];
    size_t nr = rule_list(ai, rs, 128);
    int bad = 0;
    size_t o = 0;
#define RC_SAY(...) do { if (out && o < cap) { int w_ = snprintf(out + o, cap - o, __VA_ARGS__);                         if (w_ > 0) o += (size_t)w_; } } while (0)
    for (size_t i = 0; i < nr; ++i) {
        Token *h = rule_head(ai, rs[i]);
        RuleAtom ha;
        if (!h || !rule_node_to_atom(ai, h, &ha)) continue;
        /* ① 安全条件：头的变量 ⊆ 前提的变量（先把前提里的变量收成一个表，再逐个查） */
        char pv[32][40]; size_t npv = 0;
        size_t np = rule_npremise(ai, rs[i]);
        for (size_t k = 1; k <= np; ++k) {
            Token *pp = rule_part(ai, rs[i], "前提", k);
            RuleAtom pa;
            if (!pp || !rule_node_to_atom(ai, pp, &pa)) continue;
            for (size_t m = 0; m < pa.n && npv < 32; ++m)
                if (pa.args[m][0] == '$') snprintf(pv[npv++], sizeof pv[0], "%s", pa.args[m]);
        }
        for (size_t k = 0; k < ha.n; ++k) {
            if (ha.args[k][0] != '$') continue;
            int found = 0;
            for (size_t m = 0; m < npv && !found; ++m)
                if (!strcmp(pv[m], ha.args[k])) found = 1;
            if (!found) {
                bad++;
                RC_SAY("%s：头里的 %s **不在前提里**（不安全：推的时候没有值可绑）\n",
                       rs[i]->name, ha.args[k]);
            }
        }
        /* ③ 重复规则（头 + 前提集合完全一样） */
        for (size_t j = i + 1; j < nr; ++j) {
            Token *h2 = rule_head(ai, rs[j]);
            RuleAtom hb;
            if (!h2 || !rule_node_to_atom(ai, h2, &hb)) continue;
            if (strcmp(hb.name, ha.name) || hb.n != ha.n) continue;
            int same = 1;
            for (size_t k = 0; k < ha.n && same; ++k)
                if (strcmp(hb.args[k], ha.args[k])) same = 0;
            if (!same) continue;
            size_t np2 = rule_npremise(ai, rs[j]);
            if (np2 != np) continue;
            for (size_t k = 1; k <= np && same; ++k) {
                Token *p1 = rule_part(ai, rs[i], "前提", k), *p2 = rule_part(ai, rs[j], "前提", k);
                if (!p1 || !p2 || strcmp(p1->name ? p1->name : "", p2->name ? p2->name : "")) same = 0;
            }
            if (same) {
                bad++;
                RC_SAY("%s 与 %s：**完全相同**的规则（纯冗余）\n", rs[i]->name, rs[j]->name);
                break;
            }
        }
    }
    if (out && cap) out[o < cap ? o : cap - 1] = 0;
#undef RC_SAY
    return bad;
}

/* §6.655 **前向链（一次、有界）**：把每条规则的前提在**现有事实**上枚举组合，能全部满足就把结论
 * 写成事实（并留 `[推:…]` 标记 —— 推出来的和教的要分得开）。
 *
 * 边界（写死）：
 *   · **只跑一遍**（不迭代到不动点），所以"链式"要跑两次 `::rule apply`；
 *   · 派生上限 200 条；超过就停并回报"截断"；
 *   · 只支持**肯定前提**（否定前提在前提枚举里没有"事实表"可查，这一轮不做）；
 *   · 已经成立的事实不重复写（`rel_direct` 先查）。
 */
typedef struct { size_t derived, truncated, checked; } RuleForward;

/* 把结论落成事实（关系边或谓词实例 + `[推:…]` 留痕）。返回 1 = 新写了一条。 */
static inline int rule_fwd_derive(AI *ai, const RuleAtom *hg) {
    if (!ai || !hg || hg->n < 2) return 0;
    for (size_t k = 0; k < hg->n; ++k)
        if (hg->args[k][0] == '$') return 0;      /* 没绑全 ⇒ 不落（不猜） */
    uint8_t rid = rel_id_of(ai, hg->name, 0);
    const char *a = hg->args[0];
    const char *b = hg->args[1];
    if (rid) {
        Token *A = pool_token_intern(ai->pool, a), *B = pool_token_intern(ai->pool, b);
        if (!A || !B || A == B || rel_direct(A, rid, B)) return 0;
        if (ai_write_rel(ai, A, B, rid, 0, AI_WSRC_RELATION) != 0) return 0;
    } else {
        char *ga[PRED_MAX_ARGS];
        for (size_t z = 0; z < hg->n; ++z) ga[z] = (char *)hg->args[z];
        if (pred_find(ai, hg->name, ga, hg->n)) return 0;
        if (!pred_make(ai, hg->name, ga, hg->n)) return 0;
    }
    char mk[160];
    snprintf(mk, sizeof mk, "%s%s_%s_%s]", MICRO_TAUGHT_DERIVED_PREFIX, a,
             rid ? (rel_name_of(ai, rid) ? rel_name_of(ai, rid) : "?") : hg->name, b);
    (void)pool_token_intern(ai->pool, mk);
    return 1;
}

/* 前提的**递归 join**：第 k 条起，能不能在现有事实里绑全（§6.656：链式规则要靠这个）。 */
static inline void rule_fwd_body(AI *ai, const Token *rule, size_t k, const RuleSubst *s,
                                 const RuleAtom *head, RuleForward *st, size_t max_derive) {
    if (!ai || !rule || !s || !head || !st) return;
    if (st->truncated) return;
    size_t np = rule_npremise(ai, rule);
    if (k > np) {
        RuleAtom hg;
        if (rule_ground(s, head, &hg)) {
            if (rule_fwd_derive(ai, &hg)) st->derived++;
            if (st->derived >= max_derive) st->truncated = 1;
        }
        return;
    }
    Token *pp = rule_part(ai, rule, "前提", k);
    RuleAtom pa;
    if (!pp || !rule_node_to_atom(ai, pp, &pa)) return;
    if ((unsigned char)pa.name[0] == 0xE4 && (unsigned char)pa.name[1] == 0xB8 &&
        (unsigned char)pa.name[2] == 0x8D) return;          /* 否定前提：前向不接 */
    RuleAtom g;
    if (rule_ground(s, &pa, &g)) {
        Token *node = NULL;
        st->checked++;
        if (rule_atom_to_node(ai, &g, &node))
            rule_fwd_body(ai, rule, k + 1, s, head, st, max_derive);
        return;
    }
    Token *inst[256];
    size_t ni = pred_instances(ai, pa.name, inst, 256);
    for (size_t i = 0; i < ni && !st->truncated; ++i) {
        if (rule_instance_has_var(ai, inst[i])) continue;
        RuleSubst s2 = *s;
        st->checked++;
        if (!rule_unify(ai, &pa, inst[i], &s2)) continue;
        rule_fwd_body(ai, rule, k + 1, &s2, head, st, max_derive);
    }
}

static inline int rule_apply_once(AI *ai, size_t max_derive, RuleForward *st) {
    if (!ai) return 0;
    RuleForward tot;
    memset(&tot, 0, sizeof tot);
    Token *rules[64];
    size_t nr = rule_list(ai, rules, 64);
    for (size_t i = 0; i < nr && !tot.truncated; ++i) {
        Token *h = rule_head(ai, rules[i]);
        RuleAtom ha;
        if (!h || !rule_node_to_atom(ai, h, &ha)) continue;
        if ((unsigned char)ha.name[0] == 0xE4 && (unsigned char)ha.name[1] == 0xB8 &&
            (unsigned char)ha.name[2] == 0x8D) continue;     /* 否定结论：不做 */
        if (rule_npremise(ai, rules[i]) == 0) continue;
        RuleSubst s0;
        memset(&s0, 0, sizeof s0);
        rule_fwd_body(ai, rules[i], 1, &s0, &ha, &tot, max_derive);
    }
    if (st) *st = tot;
    return (int)tot.derived;
}

/* §6.656 **前向链的不动点**（有界）：反复 `rule_apply_once` 直到"这一遍 0 条"或到遍数上限。
 * 边界：遍数 ≤5、总派生 ≤200（两个上限都可被调用方收紧）；命中任一上限 ⇒ `truncated=1`。
 * 这条把 §6.655 的"只跑一遍"补上 —— 链式规则（`祖先 :- 父, 祖先`）现在能自己链下去。 */
static inline int rule_apply_fixpoint(AI *ai, size_t max_derive, int max_pass, RuleForward *st) {
    if (!ai) return 0;
    RuleForward tot;
    memset(&tot, 0, sizeof tot);
    if (max_pass < 1) max_pass = 1;
    if (max_pass > 5) max_pass = 5;
    for (int p = 0; p < max_pass; ++p) {
        RuleForward one;
        memset(&one, 0, sizeof one);
        size_t left = (max_derive > tot.derived) ? (max_derive - tot.derived) : 0;
        int got = rule_apply_once(ai, left, &one);
        tot.derived += (size_t)got;
        tot.checked += one.checked;
        if (one.truncated) tot.truncated = 1;
        if (got == 0) break;                 /* 不动点：这一遍一条都没有 */
        if (tot.derived >= max_derive) { tot.truncated = (p + 1 < max_pass); break; }
    }
    if (st) *st = tot;
    return (int)tot.derived;
}

#endif /* RULE_H */
