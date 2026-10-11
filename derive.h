/* derive.h — 推导（Derivation）：让「链」成为第一等公民
 *
 * 这一层的立场：**输出只是思考结果的投影**。
 * 一条回答的价值不在它那句话写得漂不漂亮，而在它背后的链
 *   ① 能不能被看懂（每一步是事实还是规则、用了哪条规则）
 *   ② 能不能被**独立验证**（每一步都拿求解器重新推一遍）
 *   ③ 有没有缺口（有缺口就**不许下结论**）
 * 文字是最后一步才产生的，而且可以换风格说同一件事（::style）。
 *
 * 结构：
 *   Derivation = 一串 DerStep + 整体真值 + 缺口数 + 是否用了推测
 * 步骤种类：
 *   GOAL     目标（要回答什么）
 *   FACT     事实（图里直接有的边，带极性）
 *   RULE     规则应用（传递/继承/对称/逆关系）
 *   DEFAULT  默认推测（类比，非单调）
 *   GAP      缺口（这一步没有依据 —— 有它就说明链不完整）
 *   CONCLUDE 结论
 *
 * 验证器 der_verify 做的事是「**独立重推**」：不信任构造时写下的东西，
 * 拿 rel_direct / rel_solve_go 把每一步重新验一遍。验证不过 → gaps+1，
 * 而 renderer 只会在 gaps==0 时才敢下断言。
 */
#ifndef DERIVE_H
#define DERIVE_H

/* 注意：Derivation / DerStep / DerKind 的**类型定义在 ai.h 里**（AI 结构要用到
 * last_der 字段），这里只放函数。所以 derive.h 必须在 ai.h 之后包含。 */

static inline void der_reset(Derivation *d) {
    if (!d) return;
    d->n = 0; d->truth = 0; d->gaps = 0; d->guessed = 0; d->conf = 0;
    d->has_claim = 0; d->source[0] = 0;
}
static inline DerStep *der_add(Derivation *d, DerKind k) {
    if (!d || d->n >= DER_MAX_STEP) return NULL;
    DerStep *s = &d->step[d->n++];
    memset(s, 0, sizeof *s);
    s->kind = k;
    return s;
}
/* 记一条**非逻辑来源**：这一轮的话是从哪来的（记忆/骨架/降级）。
 * 它不参与真值 —— 检索到一句像的话不等于推导出了结论。 */
static inline void der_source(Derivation *d, const char *note) {
    if (!d) return;
    der_reset(d);
    d->has_claim = 0;
    cot_copy(d->source, sizeof d->source, note);
    DerStep *s = der_add(d, DER_SOURCE);
    if (s) cot_copy(s->note, sizeof s->note, note);
}
static inline DerStep *der_goal(Derivation *d, Token *a, uint8_t rid, Token *b) {
    DerStep *s = der_add(d, DER_GOAL);
    if (s) { s->subj = a; s->obj = b; s->rid = rid; }
    return s;
}
static inline DerStep *der_fact(Derivation *d, Token *a, uint8_t rid, Token *b,
                                int neg, int truth) {
    DerStep *s = der_add(d, DER_FACT);
    if (s) { s->subj = a; s->obj = b; s->rid = rid; s->neg = (uint8_t)neg; s->truth = truth; }
    return s;
}
static inline DerStep *der_rule(Derivation *d, uint8_t rule, Token *a, uint8_t rid,
                                Token *b, int truth) {
    DerStep *s = der_add(d, DER_RULE);
    if (s) { s->rule = rule; s->subj = a; s->obj = b; s->rid = rid; s->truth = truth; }
    return s;
}
static inline DerStep *der_default(Derivation *d, Token *a, Token *src, uint8_t rid,
                                   Token *b, int sim, int truth) {
    DerStep *s = der_add(d, DER_DEFAULT);
    if (s) { s->subj = a; s->src = src; s->obj = b; s->rid = rid;
             s->sim = sim; s->truth = truth; }
    return s;
}
static inline DerStep *der_gap(Derivation *d, const char *why) {
    DerStep *s = der_add(d, DER_GAP);
    if (s && why) { cot_copy(s->note, sizeof s->note, why); }
    return s;
}
#define DER_VERDICT_UNSURE "不 确定"   /* 判不出来时的那句措辞（唯一出处） */

static inline void der_conclude(Derivation *d, int truth, int conf) {
    DerStep *s = der_add(d, DER_CONCLUDE);
    if (s) s->truth = truth;
    if (d) { d->truth = truth; d->conf = conf; }
}

/* 把求解器给出的 Proof 变成 Derivation。
 * Proof 是一串「走到哪个词、用了哪条规则」，这里翻译成带规则的步骤：
 * 每一跳是一条 FACT（直接边）或 RULE（传递/继承/对称/逆关系）或 DEFAULT（类比）。 */
static inline void der_from_proof(AI *ai, Derivation *d, const Token *a, uint8_t rid,
                                  const Token *b, const Proof *pf) {
    (void)ai;
    der_reset(d);
    der_goal(d, (Token *)a, rid, (Token *)b);
    if (!pf) return;
    for (size_t i = 1; i < pf->n; ++i) {
        const ProofStep *ps = &pf->step[i];
        const ProofStep *prev = &pf->step[i - 1];
        uint8_t via = ps->via;
        int neg = ps->neg ? 1 : 0;
        if (via == 5) {
            der_default(d, (Token *)prev->tok, ps->tok, rid, (Token *)b,
                        ps->sim, ps->neg ? -1 : +1);
        } else if (via == 10 || via == 11) {
            /* 前提这一跳：`A -[前提]-> P`。rid 记 0（它不是关系边，是标记），
             * 渲染时按 rule==10 特判成「前提」。 */
            DerStep *r = der_add(d, DER_RULE);
            if (r) {
                r->subj = (Token *)prev->tok; r->obj = (Token *)ps->tok;
                r->rid = 0; r->neg = 0; r->rule = via;
                r->truth = +1;
            }
        } else if (via == 7 || via == 8) {
            DerStep *r = der_add(d, DER_RULE);
            if (r) {
                r->subj = (Token *)prev->tok; r->obj = (Token *)ps->tok;
                r->rid = ps->rel ? ps->rel : rid; r->neg = ps->neg;
                r->rule = via;                     /* 7 独占 8 闭合世界，复用 rule 字段 */
                r->truth = ps->neg ? -1 : +1;
            }
        } else if (via == 6 || ps->hyp) {
            /* 靠假设成立的一步。via 若是规则号（继承/传递），一并记下来，
             * 这样链上能看出「这一步是假设，而且它是被继承规则用上的」。 */
            DerStep *h = der_add(d, DER_HYP);
            if (h) {
                h->subj = (Token *)prev->tok; h->obj = (Token *)ps->tok;
                h->rid = ps->rel ? ps->rel : rid; h->neg = ps->neg;
                h->rule = (via >= 1 && via <= 4) ? via : 0;
                h->truth = ps->neg ? -1 : +1;
            }
        } else if (via == 0) {
            if (ps->known) {
                /* 这一跳的边是**推出来沉淀的** —— 链上要能看出来，
                 * 否则「我记得的」会被当成「我知道的」。 */
                DerStep *k = der_add(d, DER_KNOWN);
                if (k) {
                    k->subj = (Token *)prev->tok;
                    k->obj = (Token *)ps->tok;
                    k->rid = ps->rel ? ps->rel : rid;
                    k->neg = (uint8_t)(neg ? 1 : 0);
                    k->truth = neg ? -1 : +1;
                }
            } else {
                der_fact(d, (Token *)prev->tok, ps->rel ? ps->rel : rid,
                         (Token *)ps->tok, neg, neg ? -1 : +1);
            }
        } else {
            der_rule(d, via, (Token *)prev->tok, ps->rel ? ps->rel : rid,
                     (Token *)ps->tok, neg ? -1 : +1);
        }
    }
}

/* ---------------------------------------------------------------- 验证器 */

/* 独立重推每一步。**不信任构造时写下的东西** —— 这是「可以正确思考」的保证。
 * 返回缺口数（0 = 链条站得住）。 */
static inline int der_verify(AI *ai, Derivation *d) {
    if (!ai || !d) return 1;
    d->gaps = 0; d->guessed = 0;
    /* 被问的关系（GOAL 上记着），规则合法性要对着它判 */
    uint8_t goal_rid = 0;
    for (size_t i = 0; i < d->n; ++i)
        if (d->step[i].kind == DER_GOAL) { goal_rid = d->step[i].rid; break; }
    const RelDef *grd = rel_by_id(goal_rid);

    for (size_t i = 0; i < d->n; ++i) {
        DerStep *s = &d->step[i];
        s->verified = 0;
        switch (s->kind) {
        case DER_FACT: {
            /* 独立复查：这条边**真的**在图里，极性也对 */
            int ok = s->neg ? rel_direct_neg(s->subj, s->rid, s->obj)
                            : rel_direct(s->subj, s->rid, s->obj);
            s->verified = ok;
            if (!ok) {
                s->kind = DER_GAP;
                cot_copy(s->note, sizeof s->note, "这一步在知识里查不到");
                d->gaps++;
            }
            break;
        }
        case DER_KNOWN: {
            /* 独立复查：这条缓存**还在**（或者已经被观测升级成了普通事实）。
             * 不在了就是缺口 —— 它曾经被推出来过，现在推不出来了。 */
            int still_derived = 0;
            int ok = s->neg ? rel_direct_neg_as(s->subj, s->rid, s->obj, &still_derived)
                            : rel_direct_as(s->subj, s->rid, s->obj, &still_derived);
            if (ok && !still_derived) {
                s->kind = DER_FACT;      /* 观测把它升级了：内容没变，来源变了 */
                s->verified = 1;
                break;
            }
            s->verified = ok;
            if (!ok) {
                s->kind = DER_GAP;
                cot_copy(s->note, sizeof s->note, "这条是以前推出来记住的，现在图里没有了");
                d->gaps++;
            }
            break;
        }
        case DER_RULE: {
            /* 两件事：
             * ① 这一跳的边本身要在图里（链的每一环都得是真的）；
             * ② 这条关系必须**声明**支持所用的规则（不能拿传递去套「有」）。
             *
             * **例外**：「独占」和「闭合世界」这两条规则推出来的否定，
             * 本来就没有对应的边（那正是结论本身）—— 它们的前提要**另外复核**：
             * 独占要求找得到反例类且两者互不包含；闭合世界要求这个世界假设真的开着。 */
            /* 每一步按**它自己那条规则的前置**来验，不能一律要求「正向边存在」：
             *   传递/继承 → 正向边 a R c 必须在图里
             *   对称      → 边在**反向**（b R a），`鸟 怕 猫` 才是 `猫 怕 鸟` 的依据
             *   逆关系    → 反向 + 逆关系（b inv(R) a）
             *   独占/闭合世界 → 没有对应的边（结论本身就是那个否定），另查前提
             * 这一条改了三回，每次都是「验证器正确地拒绝了一条它看不懂的真链」。 */
            int hop_ok = 0;
            if (s->rule == 7) {
                hop_ok = rel_excl_witness(ai, s->subj, s->rid, s->obj, 0) != NULL;
            } else if (s->rule == 8) {
                hop_ok = rel_cw_ok(ai, s->subj);
            } else if (s->rule == 9) {
                /* 「缺前提」的结论也没有对应的边（结论就是那个否定）：
                 * 独立复核「确实存在一个前提 P：Y 需要 P 且 X 没有 P」。 */
                hop_ok = rel_need_witness(ai, s->subj, s->obj, 0) != NULL;
            } else if (s->rule == 10 || s->rule == 11) {
                /* R10 前提这一跳：① 边必须是**前提链接**；② 前提**现在成立**
                 * （在注意窗口里）—— 前提不成立，这条链就不该被接受。 */
                const AssocEntry *pl = token_premise_link(s->subj, s->obj);
                int ppol = edge_premise_polarity(pl);
                /* 规则号要和边的极性一致（10 正前提 / 11 反前提），再查条件成不成立 */
                hop_ok = ppol != 0 &&
                         ((s->rule == 11) == (ppol < 0)) &&
                         ai_premise_cond_ok(ai, s->obj, ppol);
            } else if (s->rule == 3) {
                hop_ok = s->neg ? rel_direct_neg(s->obj, s->rid, s->subj)
                                : rel_direct(s->obj, s->rid, s->subj);
            } else if (s->rule == 4) {
                const RelDef *hrd = rel_by_id(s->rid);
                hop_ok = hrd && hrd->inv &&
                         (s->neg ? rel_direct_neg(s->obj, hrd->inv, s->subj)
                                 : rel_direct(s->obj, hrd->inv, s->subj));
            } else {
                hop_ok = s->neg ? rel_direct_neg(s->subj, s->rid, s->obj)
                                : rel_direct(s->subj, s->rid, s->obj);
            }
            int legal = 0;
            if (grd) {
                legal = (s->rule == 1 && grd->trans) || (s->rule == 2 && grd->inherit) ||
                        (s->rule == 3 && grd->sym)   || (s->rule == 4 && grd->inv) ||
                        (s->rule == 7 && rel_is_excl(ai, s->rid)) || (s->rule == 8) ||
                        (s->rule == 9 && s->rid == REL_CAN) ||
                        (s->rule == 10 || s->rule == 11);  /* 前提这一跳与关系无关（第二跳才带关系） */
            }
            s->verified = hop_ok && legal;
            if (!s->verified) {
                s->kind = DER_GAP;
                cot_copy(s->note, sizeof s->note,
                         !hop_ok ? "这一跳的边在图里不存在" : "这条关系不支持该规则");
                d->gaps++;
            }
            break;
        }
        case DER_DEFAULT: {
            /* 推测要有真凭据：① 知识库里确实推不出来；② 最近邻够近；
             * ③ 那个最近邻确实对这件事有态度（验证器不信构造者写下的东西） */
            Proof pf;
            rel_solve(ai, s->subj, s->rid, s->obj, 0, &pf);
            int known = (pf.status != 0);
            int src_knows = s->src &&
                            (rel_direct(s->src, s->rid, s->obj) ||
                             rel_direct_neg(s->src, s->rid, s->obj));
            int ok = !known && s->sim >= REL_GUESS_SIM && src_knows;
            s->verified = ok;
            d->guessed = 1;
            if (!ok) {
                s->kind = DER_GAP;
                cot_copy(s->note, sizeof s->note,
                         known ? "其实知识库里有，不该走推测"
                               : (src_knows ? "最近邻不够近" : "依据的那一步站不住"));
                d->gaps++;
            }
            break;
        }
        case DER_GAP:
            d->gaps++;
            break;
        case DER_HYP: {
            /* 靠假设成立 —— 去**覆盖层**里核对。这是反事实推理的安全阀：
             * 覆盖层里没有这条，说明是伪造的假设 → 缺口。
             * 注意：假设**不进图**，所以这里绝不能退回去查图（那等于把假设当真）。 */
            const HypoFact *hf = hypo_find(ai ? &ai->hypo : NULL, ai ? ai->view_who : NULL,
                                           s->subj, s->rid, s->obj, NULL);
            int ok = hf && (hf->neg == s->neg);
            /* 如果这一步还标了规则，规则也得合法（假设不能绕过规则检查） */
            int legal = 1;
            if (ok && s->rule && grd) {
                legal = (s->rule == 1 && grd->trans) || (s->rule == 2 && grd->inherit) ||
                        (s->rule == 3 && grd->sym)   || (s->rule == 4 && grd->inv) ||
                        (s->rule == 7 && rel_is_excl(ai, s->rid)) || (s->rule == 8) ||
                        (s->rule == 9 && s->rid == REL_CAN);
            }
            ok = ok && legal;
            s->verified = ok;
            if (!ok) cot_copy(s->note, sizeof s->note,
                              legal ? "这一步声称靠假设，可假设里没有它"
                                    : "假设不能绕过规则合法性");
            if (!ok) {
                s->kind = DER_GAP;
                cot_copy(s->note, sizeof s->note, "这一步声称靠假设，可假设里没有它");
                d->gaps++;
            }
            break;
        }
        case DER_SOURCE:            /* 非逻辑路径：不做断言，也就无所谓真假 */
            s->verified = 1;
            break;
        default:
            s->verified = 1;
            break;
        }
    }
    /* 连通性：链的相邻两环必须首尾相接（断开就说明拼链拼错了） */
    for (size_t i = 2; i < d->n; ++i) {
        const DerStep *p = &d->step[i - 1];
        DerStep *c = &d->step[i];
        if (p->kind == DER_CONCLUDE || c->kind == DER_CONCLUDE) continue;
        if (c->kind == DER_SOURCE || c->kind == DER_GAP) continue;
        if (p->kind == DER_SOURCE || p->kind == DER_GAP) continue;
        if (p->obj && c->subj && p->obj != c->subj) {
            c->kind = DER_GAP;
            cot_copy(c->note, sizeof c->note, "链在这里断了（上一环的终点≠这一环的起点）");
            d->gaps++;
        }
    }
    /* 结论必须能独立重推出来 —— 这是最硬的一道 */
    if (d->has_claim && d->gaps == 0) {
        DerStep *g = NULL, *c = NULL;
        for (size_t i = 0; i < d->n; ++i) {
            if (d->step[i].kind == DER_GOAL) g = &d->step[i];
            else if (d->step[i].kind == DER_CONCLUDE) c = &d->step[i];
        }
        if (g && c) {
            Proof pf;
            rel_solve(ai, g->subj, g->rid, g->obj, 0, &pf);
            if (pf.status == 0) rel_solve(ai, g->subj, g->rid, g->obj, 1, &pf);
            if (pf.status == 0) {
                der_gap(d, "结论重推不出来");
                d->gaps++;
            }
        }
    }
    return d->gaps;
}

/* ---------------------------------------------------------------- 渲染 */

/* 风格：语言是**最后一步**的事，同一份链可以这么说也可以那么说。 */
/* 风格常量 DER_STYLE_* 定义在 ai.h（AI 结构拿它当字段默认值）。
 * 这里不能再定义一遍 —— 宏和同名枚举会撞。 */

static inline const char *der_style_name(int st) {
    switch (st) {
        case DER_STYLE_TERSE:  return "只给结论";
        case DER_STYLE_PLAIN:  return "结论 + 推导链";
        case DER_STYLE_CHAIN:  return "只给推导链";
        case DER_STYLE_SYMBOL: return "符号化";
        default: return "?";
    }
}

/* ---------------------------------------------------------------- 规则表
 *
 * 「有哪些推导规则、叫什么、有多硬」是**声明**，不该散在 switch 里。
 * 以前这两件事写在两个地方（这里的 der_rule_name、think.h 的 think_evidence_class），
 * 加一条规则要改两处 —— 那正是「滥用硬编码」的典型。
 * 现在它们都是这张表的一列：
 *   name  链上读作什么
 *   ev    证据强度档（见下面 DER_EV_*；think.h 的 THINK_EV_* 直接别名它们）
 * 独占/闭合世界/假设/类比是**非单调**的（可被具体事实推翻），
 * 传递/继承/对称/逆/缺前提是健全的（但它们也可能被观测形成的例外挡住 ——
 * 那是求解器里的「具体优先于一般」，见 rel_solve_go）。 */
#define DER_EV_FACT    4
#define DER_EV_RULE    3
#define DER_EV_DEFAULT 2
#define DER_EV_HYP     1
#define DER_EV_GUESS   0

typedef struct {
    uint8_t     id;
    const char *name;
    uint8_t     ev;
} RelRuleDef;

static const RelRuleDef REL_RULES[] = {
    { 1, "传递",     DER_EV_RULE },
    { 2, "继承",     DER_EV_RULE },
    { 3, "对称",     DER_EV_RULE },
    { 4, "逆关系",   DER_EV_RULE },
    { 5, "类比推测", DER_EV_GUESS },   /* via==5 的近似步 */
    { 6, "假设",     DER_EV_HYP },     /* via==6 的假设步 */
    { 7, "独占",     DER_EV_DEFAULT }, /* 函数式：一个主语只能有一个宾语 */
    { 8, "闭合世界", DER_EV_DEFAULT }, /* 显式打开的假设，不是真理 */
    { 9, "缺前提",   DER_EV_RULE },    /* 做这件事的前提它没有 ⟹ 它做不到 */
    { 10, "前提",    DER_EV_RULE },    /* R10：经前提（前提成立才走那条连结） */
    { 11, "前提不",  DER_EV_RULE },    /* R10b：反前提（前提**不**在窗口里才走） */
    { 0, NULL, 0 }
};

static inline const RelRuleDef *rel_rule_by_id(uint8_t id) {
    for (size_t i = 0; REL_RULES[i].name; ++i)
        if (REL_RULES[i].id == id) return &REL_RULES[i];
    return NULL;
}

/* **来源监控**：这条链的"底"是哪来的 —— 基准知识 / 观测 / 推论 / 假设 / 规则。
 * 看第一个能落地的步子（事实或缓存）在图上那条边的 role；都没有就说"规则推的"。 */
/* 链的底那条边**有多可信**（逐条可信度；规则/假设没有边 → 给 50 当"中性"）。 */
static inline int der_base_credibility(const AI *ai, const Derivation *d) {
    if (!d) return 50;
    for (size_t i = 0; i < d->n; ++i) {
        const DerStep *s = &d->step[i];
        if (s->kind == DER_FACT || s->kind == DER_KNOWN) {
            if (s->subj && s->obj) {
                long ei = micro_edge_index(s->subj, s->obj);
                if (ei >= 0) return ai_credibility(ai, &s->subj->assocs[ei]);
            }
            return 50;
        }
        if (s->kind == DER_HYP) return 40;
    }
    return 50;
}

static inline const char *der_base_source(const Derivation *d) {
    if (!d) return "?";
    for (size_t i = 0; i < d->n; ++i) {
        const DerStep *s = &d->step[i];
        if (s->kind == DER_HYP) return "假设";
        if (s->kind == DER_FACT || s->kind == DER_KNOWN) {
            if (s->subj && s->obj) {
                long ei = micro_edge_index(s->subj, s->obj);
                if (ei >= 0) return assoc_role_name(s->subj->assocs[ei].role);
            }
            return (s->kind == DER_KNOWN) ? "推论" : "观测";
        }
    }
    return "规则";
}

static inline const char *der_rule_name(uint8_t r) {
    const RelRuleDef *d = rel_rule_by_id(r);
    return d ? d->name : "";
}
/* 这一条规则的证据强度档（认不出来的规则当作「规则」级） */
static inline uint8_t der_rule_ev(uint8_t r) {
    const RelRuleDef *d = rel_rule_by_id(r);
    return d ? d->ev : DER_EV_RULE;
}

/* 把链渲染成一行（**带关系标签和规则名**），这是「逻辑链」的可读形式。 */
static inline void der_chain_text(const AI *ai, const Derivation *d,
                                  char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!d) return;
    size_t off = 0;
    int first = 1;
    for (size_t i = 0; i < d->n; ++i) {
        const DerStep *s = &d->step[i];
        /* 只渲染**链的一环**：GOAL 是问题、CONCLUDE 是结论、SOURCE/GAP 是说明，
         * 都不该混进链文本里（第一版把 GOAL 也渲进去了，读起来像
         * 「猫 有 毛 ? ； 猫 -[属于]-> 动物」，很怪）。 */
        if (s->kind != DER_FACT && s->kind != DER_KNOWN && s->kind != DER_RULE &&
            s->kind != DER_DEFAULT && s->kind != DER_HYP) continue;
        const char *an = (s->subj && s->subj->name) ? s->subj->name : "?";
        const char *bn = (s->obj && s->obj->name) ? s->obj->name : "?";
        const char *rn = rel_name_of(ai, s->rid);
        if (!rn) rn = "?";
        int w;
        (void)rn;
        if (s->kind == DER_DEFAULT) {
            /* 推测那一步读作「<依据词> ≈(n% 像)≈> <宾语>」：
             * 第一段是依据（src），第二段才是被推测出来的结论。 */
            const char *sn = (s->src && s->src->name) ? s->src->name : an;
            w = snprintf(out + off, cap - off, "%s%s ≈（%d%% 像）≈> %s",
                         first ? "" : " ； ", sn, s->sim, bn);
        } else {
            const char *rule = (s->kind == DER_RULE) ? der_rule_name(s->rule)
                             : (s->kind == DER_HYP ? "假设"
                             : (s->kind == DER_KNOWN ? "记" : ""));
            char lbl[48];
            if (s->kind == DER_RULE && (s->rule == 10 || s->rule == 11))
                snprintf(lbl, sizeof lbl, "%s", der_rule_name(s->rule));  /* 前提/前提不 */
            else if (rule[0]) snprintf(lbl, sizeof lbl, "%s%s·%s", s->neg ? "不 " : "", rn, rule);
            else         snprintf(lbl, sizeof lbl, "%s%s", s->neg ? "不 " : "", rn);
            w = snprintf(out + off, cap - off, "%s%s -[%s]-> %s",
                         first ? "" : " ； ", an, lbl, bn);
        }
        if (w < 0 || (size_t)w >= cap - off) { off = cap - 1; break; }
        off += (size_t)w;
        first = 0;
    }
    out[off < cap ? off : cap - 1] = 0;
}

/* 符号化渲染：给「逻辑链」一个与自然语言无关的样子。
 * 这不只是好看 —— 它证明链条本身是独立于措辞存在的。 */
static inline void der_symbol_text(const AI *ai, const Derivation *d,
                                   char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!d) return;
    size_t off = 0;
    int w = snprintf(out, cap, "?");
    off = (w > 0) ? (size_t)w : 0;
    if (d->n == 0) return;
    const DerStep *g = NULL, *c = NULL, *f1 = NULL, *f2 = NULL;
    for (size_t i = 0; i < d->n; ++i) {
        const DerStep *s = &d->step[i];
        /* 取前两个**链环**（事实或规则都算）当前提 —— 传递链的两环都是 RULE，
         * 早先只认 FACT/DEFAULT，于是传递链的符号化成了「⇐ 直接事实」。 */
        if (s->kind == DER_GOAL && !g) g = s;
        else if (s->kind == DER_CONCLUDE) c = s;
        else if (s->kind == DER_FACT || s->kind == DER_RULE ||
                 s->kind == DER_DEFAULT) {
            if (!f1) f1 = s;
            else if (!f2) f2 = s;
        }
    }
    (void)c;
    if (!g) return;
    const char *an = (g->subj && g->subj->name) ? g->subj->name : "?";
    const char *bn = (g->obj && g->obj->name) ? g->obj->name : "?";
    const char *rn = rel_name_of(ai, g->rid);
    if (!rn) rn = "?";
    off = 0;
    w = snprintf(out + off, cap - off, "%s %s %s", an, rn, bn);
    if (w < 0 || (size_t)w >= cap - off) { out[cap-1] = 0; return; }
    off += (size_t)w;
    if (f1 && f2) {
        const char *a1 = (f1->subj && f1->subj->name) ? f1->subj->name : "?";
        const char *b1 = (f1->obj && f1->obj->name) ? f1->obj->name : "?";
        const char *a2 = (f2->subj && f2->subj->name) ? f2->subj->name : "?";
        const char *b2 = (f2->obj && f2->obj->name) ? f2->obj->name : "?";
        /* 每一环用它**自己**的关系名：继承链的两环关系不同
         * （猫 属于 动物 ∧ 动物 有 毛），早先第一环错用了目标关系名，
         * 打出来成了「猫 有 动物」。 */
        const char *rn1 = rel_name_of(ai, f1->rid ? f1->rid : g->rid);
        if (!rn1) rn1 = rn;
        const char *rn2 = rel_name_of(ai, f2->rid ? f2->rid : g->rid);
        if (!rn2) rn2 = rn;
        w = snprintf(out + off, cap - off, "  ⇐  %s %s %s  ∧  %s %s %s",
                     a1, rn1, b1, a2, rn2, b2);
        if (w < 0 || (size_t)w >= cap - off) { out[cap-1] = 0; return; }
        off += (size_t)w;
    } else if (f1) {
        w = snprintf(out + off, cap - off, "  ⇐  直接事实");
        if (w > 0 && (size_t)w < cap - off) off += (size_t)w;
    }
    out[off < cap ? off : cap - 1] = 0;
}

/* 把链渲染成**回复**（这才是「语言」所在的地方）。
 * 注意：gaps > 0 时**绝不下断言** —— 只会说「推不出来」。 */
static inline void der_say(const AI *ai, Derivation *d, int style,
                           char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!d) { cot_copy(out, cap, "……"); return; }
    if (!d->has_claim) {
        /* 没有逻辑结论 —— 老实说这句话是从哪来的，别包装成推导结果。 */
        snprintf(out, cap, "%s", d->source[0] ? d->source : "（没有逻辑结论）");
        return;
    }
    char chain[320];
    chain[0] = 0;
    if (style == DER_STYLE_SYMBOL) der_symbol_text(ai, d, chain, sizeof chain);
    else der_chain_text(ai, d, chain, sizeof chain);

    /* 有缺口：不许下断言。这是整层最要紧的一条规矩。 */
    if (d->gaps > 0) {
        if (style == DER_STYLE_TERSE) snprintf(out, cap, "推不出来");
        else if (style == DER_STYLE_CHAIN) snprintf(out, cap, "%s", chain);
        else snprintf(out, cap, "推不出来（链上缺 %d 步，不能下结论）", d->gaps);
        return;
    }
    int has_hyp = 0;
    for (size_t i = 0; i < d->n; ++i)
        if (d->step[i].kind == DER_HYP) { has_hyp = 1; break; }
    /* 判定词只此一处（下面 agent 层要靠它认"这条是没答上来"，别在两处各写一份字符串） */
    const char *verdict = (d->truth > 0) ? "对" : (d->truth < 0 ? "不 对" : DER_VERDICT_UNSURE);
    switch (style) {
    case DER_STYLE_TERSE:
        snprintf(out, cap, "%s", verdict);
        break;
    case DER_STYLE_CHAIN:
        snprintf(out, cap, "%s", chain);
        break;
    case DER_STYLE_SYMBOL:
        snprintf(out, cap, "%s ： %s", verdict, chain);
        break;
    case DER_STYLE_PLAIN:
    default:
        if (d->guessed)
            snprintf(out, cap, "大概 吧 ， %s （%s）", verdict, chain);
        else if (has_hyp)
            /* 反事实：必须写明是**在假设下**的结论，不能说得像真的一样 */
            snprintf(out, cap, "在 假设 下 ： %s ， %s", verdict, chain);
        else
            snprintf(out, cap, "%s ， %s", verdict, chain);
        break;
    }
}

/* 结构化打印：把链按步摊开，每步一行，带验证结果。
 * 这是「链是产物」这句话最直接的体现 —— 文字只是最后一行。 */
static inline void der_dump(const AI *ai, const Derivation *d, int style, FILE *out) {
    if (!out) return;
    if (!d || d->n == 0) { fprintf(out, "（还没有推导）\n"); return; }
    fprintf(out, "[推导] %s  真值=%s  缺口=%d  推测=%s  把握=%d\n",
            d->has_claim ? "有逻辑结论" : "无逻辑结论（只有来源）",
            d->truth > 0 ? "真" : (d->truth < 0 ? "假" : "未定"),
            d->gaps, d->guessed ? "是" : "否", d->conf);
    for (size_t i = 0; i < d->n; ++i) {
        const DerStep *s = &d->step[i];
        const char *an = (s->subj && s->subj->name) ? s->subj->name : "-";
        const char *bn = (s->obj && s->obj->name) ? s->obj->name : "-";
        const char *rn = rel_name_of(ai, s->rid);
        const char *mark = s->verified ? "✓" : "✗";
        switch (s->kind) {
        case DER_GOAL:
            fprintf(out, "  %zu. 目标      %s %s %s ?\n", i + 1, an, rn ? rn : "?", bn);
            break;
        case DER_FACT:
            fprintf(out, "  %zu. 事实 %s   %s -[%s%s]-> %s\n", i + 1, mark,
                    an, s->neg ? "不 " : "", rn ? rn : "?", bn);
            break;
        case DER_RULE:
            if (s->rule == 10 || s->rule == 11)
                fprintf(out, "  %zu. 前提 %s   %s -[%s]-> %s   （R10：条件成立才走这条连结）\n",
                        i + 1, mark, an, der_rule_name(s->rule), bn);
            else
                fprintf(out, "  %zu. 规则 %s   %s -[%s%s]-> %s   （%s）\n", i + 1, mark,
                        an, s->neg ? "不 " : "", rn ? rn : "?", bn, der_rule_name(s->rule));
            break;
        case DER_DEFAULT:
            fprintf(out, "  %zu. 推测 %s   %s ≈（%d%% 像）≈> %s\n", i + 1, mark,
                    s->src && s->src->name ? s->src->name : "?", s->sim, bn);
            break;
        case DER_HYP:
            fprintf(out, "  %zu. 假设 %s   %s -[%s%s]-> %s   （来自假设层，不在知识库里%s）\n",
                    i + 1, mark, an, s->neg ? "不 " : "", rn ? rn : "?", bn,
                    s->rule ? "；被继承/传递规则用上" : "");
            break;
        case DER_SOURCE:
            fprintf(out, "  %zu. 来源      %s\n", i + 1, s->note);
            break;
        case DER_GAP:
            fprintf(out, "  %zu. **缺口**  %s\n", i + 1, s->note);
            break;
        case DER_CONCLUDE:
            fprintf(out, "  %zu. 结论      %s\n", i + 1,
                    s->truth > 0 ? "真" : (s->truth < 0 ? "假" : "未定"));
            break;
        default:
            break;
        }
    }
    char line[400];
    der_say(ai, (Derivation *)d, style, line, sizeof line);
    fprintf(out, "  → 这么说（风格 %s）：%s\n", der_style_name(style), line);
}

#endif /* DERIVE_H */
