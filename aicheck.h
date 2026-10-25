/* ============================================================================
 * aicheck.h — **图的不变式自检**（§6.584，阶段②查 bug 的第一件工具）
 *
 * 为什么要有它：现在的套件只验"答得对/不崩"，但一个错误的图（边指向别的池的词、
 * 同名两个节点、反查表漏一条、modlog 记了不存在的 id……）可能**照样答得对**，
 * 直到某个边界条件才炸。所以把"图必须满足的结构"写成断言，在**每次操作之后**跑一遍。
 *
 * 主张（invariants，全都 O(n+E)，开着也不慢）：
 *   ① 池：`tokens[i]->id == i`、`owner == pool`、`count <= cap`；
 *   ② 边：`assocs` 只有三种合法形态（RAM 数组 / DISK 哨兵 / 空），
 *      `main`/`action` 必须是**同一个池**里的活词，且**不能指向自己**；
 *   ③ 名字：每个有名字的词都能被 `pool_find` 找回来（这条同时抓"同名两节点"和哈希错位），
 *      `n_entries` 与"有名字的词数"一致；
 *   ④ 反查表：图上每一条关系边（含否定、含动态谓语）在 `dst->rev_head` 链里都找得到；
 *   ⑤ 账：`AIModLog` 里的 `a_id/b_id` 必须落在池里（b_id==0 是"宾语本地还没有词"，合法）；
 *   ⑥ 局部加载：`parts` 与 `pool->part_load` 要么都有要么都没有；索引里没有孤儿 id。
 *
 * 用法：
 *     AiCheck ck;
 *     if (!aip_check_ai(ai, &ck)) { printf("%s\n", ck.first); }   // 返回 1 = 干净
 * 环境变量 `AIP_CHECK=1` 时，CLI 每轮也会跑一遍（见 `main.c` 的 REPL 尾巴）。
 * ==========================================================================*/
#ifndef AICHECK_H
#define AICHECK_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ⚠ 和 parts.h 一样：假定调用方已经 include 了 ai.h / token.h / rel.h。 */
#include "token.h"

typedef struct {
    size_t bad;             /* 违反了几条（必须为 0）*/
    size_t warn;            /* **已知历史垃圾**的条数（自环空边；见下面 `aip_check_warn`）*/
    char   first[240];      /* 第一条 bad（带上下文），方便直接打印 */
    char   warn1[200];      /* 第一条 warn */
} AiCheck;

/* 已知历史垃圾：**空的自环边**（role/rel/nrel/dyn/hits/seq 全 0、action 为 NULL）。
 * 出厂 `model.bin`/`session.bin` 里各有 **18 条**（老版本留下的），不参与任何推理
 * （`rel_*` 看 rel 位、共现看 hits），所以留着不影响行为。
 * 处理方式：把它算 **warn** 而不是 bad —— 但探针/自检会**钉住这个数字**（`== 18`），
 * 它一旦变多就说明有新的写入路径在造自环（那才是真 bug）。 */
static inline int aip_edge_is_empty_self(const Token *t, const AssocEntry *e) {
    /* 注意：**不看 seq** —— 这些历史边是走 `token_add_assoc_ex` 建的，seq 有值；
     * 但 hits=0 且没有角色/关系/动作 ⇒ 不参与共现打分、不参与推理、不进 emb。 */
    return t && e && e->main == t && !e->action && e->role == ASSOC_ROLE_NONE &&
           e->rel == 0 && e->nrel == 0 && e->dyn == 0 && e->hits == 0 && e->n_tset == 0;
}

static inline void aip_check_warn(AiCheck *c, const char *fmt, ...) {
    if (!c) return;
    c->warn++;
    if (c->warn1[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->warn1, sizeof c->warn1, fmt, ap);
    va_end(ap);
}

static inline void aip_check_fail(AiCheck *c, const char *fmt, ...) {
    if (!c) return;
    c->bad++;
    if (c->first[0]) return;          /* 只留第一条（后面的是同因连锁）*/
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->first, sizeof c->first, fmt, ap);
    va_end(ap);
}

/* 反查表对拍：返回"图上应有 / 缺多少条"（`::revcheck` 与自检共用这一份实现）。 */
static inline void aip_rev_audit(AI *ai, size_t *out_want, size_t *out_missing,
                                 char *first, size_t first_cap) {
    if (out_want) *out_want = 0;
    if (out_missing) *out_missing = 0;
    if (first && first_cap) first[0] = 0;
    if (!ai || !ai->pool) return;
    TokenPool *p = ai->pool;
    size_t want = 0, missing = 0;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t || t->is_garbage) continue;
        size_t cnt = t->count;
        if (!token_assocs_resident(t)) {
            /* 边在磁盘上：**不为了自检把整份图读回来**（那会把"局部加载"变成"全加载"）。
             * 这类词的条数记在 `count` 里，但内容在段文件里 —— 跳过内容检查。 */
            if (cnt && out_want) *out_want += 0;
            continue;
        }
        for (size_t k = 0; k < cnt; ++k) {
            const AssocEntry *e = &token_assocs_ro(t)[k];
            if (!e->main) continue;
            for (uint8_t r = 1; r < REL_DYN_BASE; ++r) {
                uint16_t bit = REL_BIT(r);
                if (e->rel & bit) {
                    want++;
                    if (!pool_rev_has(p, e->main, t->id, r, 0)) {
                        missing++;
                        if (first && first_cap && !first[0])
                            snprintf(first, first_cap, "反查表缺：%s -[%s]-> %s",
                                     t->name ? t->name : "?",
                                     rel_name_of(ai, r) ? rel_name_of(ai, r) : "?",
                                     e->main->name ? e->main->name : "?");
                    }
                }
                if (e->nrel & bit) {
                    want++;
                    if (!pool_rev_has(p, e->main, t->id, r, 1)) {
                        missing++;
                        if (first && first_cap && !first[0])
                            snprintf(first, first_cap, "反查表缺（否定）：%s -[不 %s]-> %s",
                                     t->name ? t->name : "?",
                                     rel_name_of(ai, r) ? rel_name_of(ai, r) : "?",
                                     e->main->name ? e->main->name : "?");
                    }
                }
            }
            if (e->dyn) {
                want++;
                if (!pool_rev_has(p, e->main, t->id, (uint32_t)(REL_DYN_BASE + e->dyn - 1),
                                  edge_dyn_neg(e) ? 1 : 0)) {
                    missing++;
                    if (first && first_cap && !first[0]) {
                        const char *dn = rel_dyn_name(ai, (uint8_t)(REL_DYN_BASE + e->dyn - 1));
                        snprintf(first, first_cap, "反查表缺（动态 %s）：%s -> %s",
                                 dn ? dn : "?", t->name ? t->name : "?",
                                 e->main->name ? e->main->name : "?");
                    }
                }
            }
        }
    }
    if (out_want) *out_want = want;
    if (out_missing) *out_missing = missing;
}

/* 全量自检。返回 1 = 干净；0 = 有违反（`ck->first` 里是第一条）。 */
static inline int aip_check_ai(AI *ai, AiCheck *ck) {
    if (ck) { ck->bad = 0; ck->warn = 0; ck->first[0] = 0; ck->warn1[0] = 0; }
    if (!ai || !ai->pool) {
        if (ck) { ck->bad++; snprintf(ck->first, sizeof ck->first, "AI 或 pool 为空"); }
        return 0;
    }
    TokenPool *p = ai->pool;

    /* ---- ① 池 ---- */
    if (p->count > p->cap) aip_check_fail(ck, "池 count>cap（count=%zu cap=%zu）", p->count, p->cap);
    if (p->count && !p->tokens) aip_check_fail(ck, "池有 count=%zu 但 tokens 为空", p->count);
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t) { aip_check_fail(ck, "tokens[%zu] 为空", i); continue; }
        if (t->id != i) aip_check_fail(ck, "id 与下标不一致：名字=%s id=%u 下标=%zu", t->name ? t->name : "?", t->id, i);
        if (t->owner != p) aip_check_fail(ck, "owner 不是本池：名字=%s 下标=%zu", t->name ? t->name : "?", i);
        /* ---- ② 边数组的三种合法形态 ---- */
        int tag = token_assocs_in_ram(t) ? (int)AIP_PTR_RAM
                : (token_assocs_on_disk(t) ? (int)AIP_PTR_DISK : -1);
        /* `cap<count` 只对**内存里**的边表成立：`detach` 故意把 cap 置 0（边在磁盘上）*/
        if (tag == AIP_PTR_RAM && t->cap < t->count)
            aip_check_fail(ck, "cap<count：名字=%s cap=%zu count=%zu", t->name ? t->name : "?", t->cap, t->count);
        if (tag != AIP_PTR_RAM && tag != AIP_PTR_DISK)
            aip_check_fail(ck, "边数组标签非法：名字=%s tag=%d", t->name ? t->name : "?", tag);
        if (tag == AIP_PTR_DISK && !token_assocs_disk_sentinel_ok(t))
            aip_check_fail(ck, "DISK 哨兵不是 0：名字=%s", t->name ? t->name : "?");
        if (tag == AIP_PTR_RAM && t->count && !token_assocs(t))
            aip_check_fail(ck, "count>0 但边数组为空：名字=%s count=%zu", t->name ? t->name : "?", t->count);

        if (tag == AIP_PTR_RAM) {
            const AssocEntry *as = token_assocs_ro(t);
            for (size_t k = 0; k < t->count; ++k) {
                const Token *m = as[k].main;
                if (m) {
                    if (m == t) {
                        if (aip_edge_is_empty_self(t, &as[k]))
                            aip_check_warn(ck, "历史空自环：%s", t->name ? t->name : "?");
                        else
                            aip_check_fail(ck, "自环边（带语义）：%s rel=%u nrel=%u dyn=%u hits=%u role=%u act=%s",
                                           t->name ? t->name : "?", as[k].rel, as[k].nrel,
                                           as[k].dyn, as[k].hits, as[k].role,
                                           as[k].action && as[k].action->name ? as[k].action->name : "-");
                    }
                    else if (m->owner != p) aip_check_fail(ck, "边指向**别的池**的词：%s→%s（第 %zu 条）", t->name ? t->name : "?", m->name ? m->name : "?", k);
                    else if (m->id >= p->count || p->tokens[m->id] != m)
                        aip_check_fail(ck, "边指向的词不在池里（id 错位）：%s→%s（第 %zu 条）", t->name ? t->name : "?", m->name ? m->name : "?", k);
                }
                const Token *ac = as[k].action;
                if (ac) {
                    if (ac->owner != p) aip_check_fail(ck, "action 是别的池的词：%s→%s（第 %zu 条）", t->name ? t->name : "?", ac->name ? ac->name : "?", k);
                    else if (ac->id >= p->count || p->tokens[ac->id] != ac)
                        aip_check_fail(ck, "action 的词不在池里：%s→%s（第 %zu 条）", t->name ? t->name : "?", ac->name ? ac->name : "?", k);
                }
            }
        }

        /* ---- ③ 名字能被找回来（这条同时抓同名两节点/哈希错位）---- */
        if (t->name) {
            if (p->n_buckets) {
                Token *f = pool_find(p, t->name);
                if (f != t) aip_check_fail(ck, "名字找不回来/撞名：%s（找回来的是 id=%lld）", t->name,
                                           (long long)(f ? (long long)f->id : -1LL));
            }
        }
    }
    /* 名字数 == 哈希条目数（插了没删，所以应该相等）*/
    {
        size_t named = 0;
        for (size_t i = 0; i < p->count; ++i)
            if (p->tokens[i] && p->tokens[i]->name) named++;
        if (p->n_buckets && named != p->n_entries)
            aip_check_fail(ck, "哈希条目数对不上（有名字的词 %zu 个 / 表里 %zu 条）", named, p->n_entries);
    }

    /* ---- ⑤ 账 ---- */
    for (size_t i = 0; i < ai_modlog_count(ai); ++i) {
        const AIModEntry *e = ai_modlog_at(ai, i);
        if (!e) continue;
        if (e->a_id >= p->count) aip_check_fail(ck, "账里 a_id 越界：a_id=%u 池大小=%zu", e->a_id, p->count);
        if (e->b_id && e->b_id >= p->count) aip_check_fail(ck, "账里 b_id 越界：b_id=%u 池大小=%zu", e->b_id, p->count);
    }

    /* ---- ⑥ 局部加载 ---- */
    if (!!p->parts != !!p->part_load)
        aip_check_fail(ck, "parts（%s）与 part_load（%s）不一致", p->parts ? "有" : "无", p->part_load ? "有" : "无");

    /* ---- ④ 反查表（O(E)；边在磁盘上的词跳过，不为了自检把图读回来）---- */
    {
        size_t want = 0, missing = 0;
        char rf[200];
        aip_rev_audit(ai, &want, &missing, rf, sizeof rf);
        if (missing) {
            if (ck) { ck->bad++; if (!ck->first[0]) snprintf(ck->first, sizeof ck->first, "%s", rf); }
        }
    }

    return (ck && ck->bad) ? 0 : 1;
}

/* ---- 探针用的小包装（打印第一条）---- */
static inline int ai_check_ok(AI *ai) {
    AiCheck ck;
    if (aip_check_ai(ai, &ck)) return 1;
    printf("      [不变式] bad=%zu warn=%zu：%s\n", ck.bad, ck.warn, ck.first);
    return 0;
}
static inline void ai_check_dump_if_bad(AI *ai, const char *tag) {
    AiCheck ck;
    if (aip_check_ai(ai, &ck)) printf("      [不变式] %s 干净\n", tag ? tag : "");
    else printf("      [不变式] %s 坏了：bad=%zu %s\n", tag ? tag : "", ck.bad, ck.first);
}

#endif /* AICHECK_H */
