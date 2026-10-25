/* model_io.h — 模型整体快照：存盘 / 读盘（model.bin）
 *
 * 为什么不能直接用 token.h 的 token_pool_dump：
 *   那份 v4 流只写「符号图」，读回来会丢：
 *     - 符号节点的 birth_tick / last_touch_tick / mem_state / is_garbage / next_in_time
 *     - AssocEntry 的 birth_tick / last_touch_tick / mem_state
 *     - TokenPool 的 current_tick 与视角栈
 *     - 以及整个 AI 层（rng / 内生状态 / 元认知 / 校准 / 表达自举 / 时间线 / 他者 /
 *       焦点环 / trace / last_output / last_skipped / 各种计数器）
 *   本文件把这些**全部**写进 model.bin，读回来的 AI 与训练完在内存里的那个逐位等价：
 *   存 → 读 → 再存，两次字节完全一致（tests/model_roundtrip.sh 看护）。
 *
 * 格式（全小端，自描述，无结构体 padding 依赖）：
 *   [FileHeader 40B]
 *   [TokenRec × n]                       每符号节点一条，长度可变
 *   [AssocRec × total_assocs]            全池所有边，按符号节点下标顺序
 *   [AIBlock]                            AI 层状态
 *
 * 依赖：token.h + ai.h 里的类型。被 ai.h 之后包含。
 */
#ifndef MODEL_IO_H
#define MODEL_IO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "saveio.h"      /* §6.583 唯一临时名 + 落盘 + 原子替换 */
#include <errno.h>
/* §6.124 **自足性**：本文件用了 `fsync`（POSIX，声明在 unistd.h），但一直没包含它 ——
 * 只在"包含链里恰好有 action.h"的 TU（main.c 那条）里编得过。
 * `tools/overlapprobe.c` 从 ai.h → frame_io.h → model_io.h 进来，不含 action.h，
 * 于是探针门（`-std=c11 -Werror`）当场红：`fsync` 隐式声明。
 * 干净 HEAD 上同样红（已验证）⇒ 不是某次改动引入的，是这里少一行。 */
#include <unistd.h>

#define MODEL_MAGIC   0x444D4941u  /* "AIMD" */
/* v8: 「他者模型」带上**他的信念表** —— 归属从标注变成持久状态。
 *     旧文件（v1..v7）照样能读：读进来的 other 没有信念，n_bel = 0。
 * v9: 边的 rel/nrel 从 1 字节扩成 2 字节 —— 核心关系多了第 9 个「需要」
 *     （缺前提推理 R9 用的），bit8 装不进一个字节。
 *     v8 及以前按 1 字节读进来即可（那些文件不可能有 bit8）。
 * v10: **边的语境字段（subs/subs_not）删掉**（用户 2026-10-07 拍板「历史遗留可以删」）。
 *     每条边少写 4+4 个字节的"计数 + 裸 id"：出厂模型 41138 条边实测全是 0 ⇒ 迁移无损，
 *     文件与内存各瘦一圈（内存里那 64 字节/条也从 AssocEntry 里删了）。
 *     v9 及以前照样读得回来：按 has_ctx 把那两段跳过去，读进来的边当**普通边**
 *     （旧文件里实测一条带语境的都没有；手工造过的那种会**少一层条件**，登记在案 —— 见 §6.50）。 */
#define MODEL_VERSION 14u  /* v14: 删掉 hv/hv_prev（VSA 绑定代数整套撤销，§6.299）；读 v13 时按旧布局丢弃 */  /* v13: 他者**改口历史**（BelHist，§6.112） */  /* v12: 边存**时刻集合**（tset/n_tset，§6.111） */  /* v11: **N1 思考对象**持久化（摘要：结论/缺口/复没复核，§6.110） */  /* v7: 边增加**否定**关系与**动态**关系字节（L2 完整逻辑链） */

/* ---------------------------------------------------------------- 字节流 */

typedef struct { uint8_t *buf; size_t len, cap; int err; } MWriter;
typedef struct { const uint8_t *buf; size_t len, pos; int err; } MReader;

static inline void mw_raw(MWriter *w, const void *src, size_t n) {
    if (n == 0) return;
    if (w->buf) {
        if (w->len + n > w->cap) { w->err = 1; return; }
        memcpy(w->buf + w->len, src, n);
    }
    w->len += n;
}
static inline void mw_u8 (MWriter *w, uint8_t v)  { mw_raw(w, &v, 1); }
static inline void mw_u16(MWriter *w, uint16_t v) { uint8_t b[2] = {
    (uint8_t)v, (uint8_t)(v >> 8) }; mw_raw(w, b, 2); }
static inline void mw_u32(MWriter *w, uint32_t v) { uint8_t b[4] = {
    (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; mw_raw(w, b, 4); }
static inline void mw_u64(MWriter *w, uint64_t v) { mw_u32(w, (uint32_t)v);
                                                    mw_u32(w, (uint32_t)(v >> 32)); }
static inline void mw_f32(MWriter *w, float v)    { uint32_t u; memcpy(&u, &v, 4); mw_u32(w, u); }
static inline void mw_str(MWriter *w, const char *s, uint32_t n) {
    mw_u32(w, n);
    if (n) mw_raw(w, s, n);
}

static inline int mr_raw(MReader *r, void *dst, size_t n) {
    if (r->pos + n > r->len) { r->err = 1; return -1; }
    if (dst) memcpy(dst, r->buf + r->pos, n);
    r->pos += n;
    return 0;
}
static inline uint8_t  mr_u8 (MReader *r) { uint8_t v = 0; mr_raw(r, &v, 1); return v; }
static inline uint16_t mr_u16(MReader *r) { uint8_t b[2] = {0,0}; mr_raw(r, b, 2);
                                            return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8)); }
static inline uint32_t mr_u32(MReader *r) { uint8_t b[4] = {0,0,0,0};
                                            mr_raw(r, b, 4);
                                            return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static inline uint64_t mr_u64(MReader *r) { uint64_t lo = mr_u32(r); uint64_t hi = mr_u32(r);
                                            return lo | (hi << 32); }
static inline float    mr_f32(MReader *r) { uint32_t u = mr_u32(r); float f; memcpy(&f, &u, 4); return f; }

/* ------------------------------------------------------- 通用 id 列表助手 */

static inline void mw_ids(MWriter *w, Token *const *arr, size_t n) {
    mw_u32(w, (uint32_t)n);
    for (size_t i = 0; i < n; ++i) mw_u32(w, arr[i] ? arr[i]->id : TOKEN_ID_NONE);
}

/* 读 id 列表进动态数组；max_hint 给容量上界（0 = 不设限，只受文件长度约束） */
static inline int mr_ids(MReader *r, TokenPool *p, Token ***arr, size_t *ncap, size_t *n_out,
                         size_t max_hint) {
    uint32_t n = mr_u32(r);
    if (r->err) return -1;
    if (max_hint && n > max_hint) return -1;
    if (n == 0) { *n_out = 0; return 0; }
    Token **buf = (Token **)realloc(*arr, (size_t)n * sizeof *buf);
    if (!buf) return -1;
    *arr = buf;
    *ncap = n;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = mr_u32(r);
        if (r->err) return -1;
        Token *t = (id == TOKEN_ID_NONE) ? NULL : pool_token_by_id(p, id);
        if (id != TOKEN_ID_NONE && !t) return -1;
        buf[i] = t;
    }
    *n_out = n;
    return 0;
}

/* 这条边会不会被存盘：两端（主语与宾语）、动作里**没有临时符号节点**。
 * 临时符号节点是草稿纸，不存盘；指向它的边也不存 —— 否则装载时会指向不存在的 id。
 * （v10：边的语境字段已删，所以这里只看 main/action 两端。） */
static inline int mio_edge_saved(const AssocEntry *e) {
    if (!e) return 0;
    /* §6.185 **没有名字的端点是"临时叠加"**（`ai_imagine` 用 `pool_token(pool,NULL,NULL)`
     * 造的概念节点）：它的边**不存盘**。§6.175 把这类边的写入改成了直写（不进受审通道），
     * 副作用是它们开始被存下来 —— 实测 100 轮多 10 条 `我 → (无名)` 的边（42026 → 42036）。
     * 存它等于把"临时叠加"写进知识，而且每 100 轮 +10 条会无界增长 ⇒ 这里一并跳过。 */
    if (!e->main || !e->main->name || !e->main->name[0]) return 0;
    if (token_is_temp(e->main) || token_is_temp(e->action)) return 0;
    return 1;
}

/* ------------------------------------------------------------ 存盘（两遍） */

/* 第一遍：算出 model.bin 需要多少字节。cap/out 为 0/NULL 时只量长度。 */
static inline size_t ai_snapshot(const AI *ai, uint8_t *out, size_t cap) {
    if (!ai || !ai->pool) return 0;
    const TokenPool *p = ai->pool;
    MWriter w = { out, 0, cap, 0 };

    /* ---- 头 ---- */
    /* **临时符号节点（[临:…]）不存盘**：它是草稿纸（会话内的中间节点），
     * 存它等于把草稿写成知识。跳过它**以及指向它的边**（不然装载时会指向不存在的 id）。
     * 名字前缀是唯一判据 —— 这条路**不需要为它动存档格式**（v10 里连语境字段都没了，判断更简单）。
     * 没有临时符号节点时，下面每个过滤都恒真 ⇒ 输出逐字节不变（roundtrip 钉着这一点）。 */
    /* §6.598 **临时符号节点也写一条记录**（否则 id 出现缺口 ⇒ 读盘那侧 `t->id != id`
     * 硬校验当场判损坏、整份存档**静默读不回来**；实测 `::op 加前提` 之后存盘就无法再载入）。
     * 它们落盘时强制标垃圾、边一条不写 ⇒ 草稿纸不会复活成知识；**头里的 n_tok 必须
     * 包含它们**（和下面那段写记录的循环逐条对齐），否则读盘会错位。 */
    size_t saved_tokens = 0;
    uint32_t total_assocs = 0;
    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        saved_tokens++;
        if (token_is_temp(t)) continue;
        for (size_t j = 0; j < t->count; ++j)
            if (!mio_edge_saved(&token_assocs_ro(t)[j])) continue;
            else total_assocs++;
    }
    w.len = 0;
    mw_u32(&w, MODEL_MAGIC);
    mw_u32(&w, MODEL_VERSION);
    mw_u32(&w, EMB_DIM);
    mw_u32(&w, (uint32_t)saved_tokens);
    mw_u32(&w, total_assocs);
    mw_u32(&w, (uint32_t)(p->perspective_cur ? p->perspective_cur->depth : 0));
    if (w.err) return 0;

    /* ---- 符号节点段 ---- */
    for (size_t i = 0; i < p->count; ++i) {
        const Token *t = p->tokens[i];
        /* §6.598 **临时符号节点也要占一个 id 空位**（别再 `continue` 跳过）。
         * 以前跳过它们 ⇒ 文件里的 id 出现缺口 ⇒ 读盘那侧有 `t->id != id` 的硬校验，
         * 整份存档**静默读不回来**。实测：`::op 加前提 猫 动物` 造出一个被引用的
         * `[临:前提]` 之后存盘，`ai_load_file` 直接返回 NULL（`bughunt` 的随机 `::op`
         * 动作把它照出来的）。现在：名字照写、**一条边都不写**、落盘时强制标垃圾
         * （草稿纸复活也不会变成知识；id 连续 ⇒ 存档可读）。 */
        const int is_temp = token_is_temp(t);
        uint32_t n_saved_edges = 0;
        if (!is_temp)
            for (size_t j = 0; j < t->count; ++j)
                if (mio_edge_saved(&token_assocs_ro(t)[j])) n_saved_edges++;
        uint32_t nlen = t->name ? (uint32_t)strlen(t->name) : 0;
        mw_u32(&w, t->id);
        mw_str(&w, t->name ? t->name : "", nlen);
        mw_u32(&w, t->misses);
        mw_u32(&w, t->recent_misses);
        mw_u32(&w, t->parent ? t->parent->id : TOKEN_ID_NONE);
        mw_u32(&w, t->next_in_time ? t->next_in_time->id : TOKEN_ID_NONE);
        mw_u32(&w, t->perspective_of ? t->perspective_of->id : TOKEN_ID_NONE);
        mw_u8 (&w, t->is_category);
        mw_u8 (&w, is_temp ? 1 : t->is_garbage);   /* §6.598 临时节点落盘即"垃圾" */
        mw_u8 (&w, t->mem_state);
        mw_u64(&w, t->birth_tick);
        mw_u64(&w, t->last_touch_tick);
        mw_u32(&w, n_saved_edges);

        for (size_t j = 0; j < t->count && !is_temp; ++j) {
            const AssocEntry *e = &token_assocs_ro(t)[j];
            if (!mio_edge_saved(e)) continue;
            /* v10：语境那两段（n_subs + 裸 id / n_not + 裸 id）不再写。 */
            mw_u32(&w, e->main ? e->main->id : TOKEN_ID_NONE);
            mw_u32(&w, e->action ? e->action->id : TOKEN_ID_NONE);            mw_u32(&w, e->hits);
            mw_u32(&w, e->seq);
            mw_u8 (&w, e->mem_state);
            mw_u64(&w, e->birth_tick);
            mw_u64(&w, e->last_touch_tick);
            mw_u8 (&w, e->role);
            mw_u8 (&w, e->topic);
            mw_u8 (&w, e->pattern);
            mw_u16(&w, e->rel);      /* v9：2 字节（bit8 = 需要） */
            mw_u16(&w, e->nrel);
            mw_u8 (&w, e->dyn);
            /* v12：边上的**时刻集合**（互不相同的 tick，≤4） */
            uint8_t nt_ok = (e->n_tset <= 4) ? e->n_tset : 0;   /* 同上：垃圾值当空集合 */
            mw_u8 (&w, nt_ok);
            for (uint8_t k = 0; k < 4; ++k) mw_u32(&w, k < nt_ok ? e->tset[k] : 0u);
        }
        if (w.err) return 0;
    }

    /* ---- 池层 ---- */
    mw_u64(&w, p->current_tick);
    mw_u32(&w, (uint32_t)p->n_entries);

    /* ---- AI 层 ---- */
    mw_u32(&w, ai->rng);
    mw_u32(&w, ai->gc_rng);
    mw_u64(&w, ai->turns);
    mw_u64(&w, (uint64_t)ai->n_learned);
    mw_f32(&w, ai->energy);
    mw_f32(&w, ai->decay);
    mw_f32(&w, ai->cutoff);
    mw_u32(&w, (uint32_t)ai->max_gen);
    mw_u32(&w, (uint32_t)ai->consolidate_every);
    mw_f32(&w, ai->decay_factor);

    /* gc / uncertain / setpoint */
    mw_u64(&w, ai->gc.gc_min_age);
    mw_u64(&w, ai->gc.gc_time_scale);
    mw_f32(&w, ai->gc.gc_upgrade_rate);
    mw_u32(&w, (uint32_t)ai->gc.gc_interval);
    mw_u8 (&w, ai->gc.gc_delete_state);
    mw_f32(&w, ai->uncertain.w_gap);
    mw_f32(&w, ai->uncertain.w_conflict);
    mw_f32(&w, ai->uncertain.w_activation);
    mw_f32(&w, ai->uncertain.w_effort);
    mw_f32(&w, ai->uncertain.w_novelty);
    mw_f32(&w, ai->uncertain.certain_threshold);
    mw_f32(&w, ai->uncertain.unknown_threshold);
    mw_f32(&w, ai->setpoint.setpoint_comfort);
    mw_f32(&w, ai->setpoint.setpoint_arousal);
    mw_f32(&w, ai->setpoint.regulation_rate);

    /* 内生状态 + 元认知 */
    mw_f32(&w, ai->inner.predict_accuracy);
    mw_f32(&w, ai->inner.surprise_level);
    mw_f32(&w, ai->inner.comfort);
    mw_f32(&w, ai->inner.discomfort);
    mw_f32(&w, ai->inner.arousal);
    mw_f32(&w, ai->inner.fatigue);
    mw_f32(&w, ai->meta.confidence);
    mw_f32(&w, ai->meta.familiarity);
    mw_f32(&w, ai->meta.fluency);
    mw_f32(&w, ai->meta.effort);
    mw_f32(&w, ai->meta.gap_feel);
    mw_f32(&w, ai->meta.total);

    /* 核心符号节点指针 */
    mw_u32(&w, ai->self        ? ai->self->id        : TOKEN_ID_NONE);
    mw_u32(&w, ai->good        ? ai->good->id        : TOKEN_ID_NONE);
    mw_u32(&w, ai->bad         ? ai->bad->id         : TOKEN_ID_NONE);
    mw_u32(&w, ai->unknowing   ? ai->unknowing->id   : TOKEN_ID_NONE);
    mw_u32(&w, ai->causal      ? ai->causal->id      : TOKEN_ID_NONE);
    mw_u32(&w, ai->social_good ? ai->social_good->id : TOKEN_ID_NONE);
    mw_u32(&w, ai->social_bad  ? ai->social_bad->id  : TOKEN_ID_NONE);
    mw_u32(&w, ai->past_self   ? ai->past_self->id   : TOKEN_ID_NONE);
    mw_u32(&w, ai->future_self ? ai->future_self->id : TOKEN_ID_NONE);

    /* 焦点环 + 当前焦点 */
    for (size_t i = 0; i < AI_MAX_FOCUS; ++i) {
        mw_u32(&w, ai->focus_ring[i] ? ai->focus_ring[i]->id : TOKEN_ID_NONE);
        mw_f32(&w, ai->focus_weight[i]);
    }
    mw_u32(&w, (uint32_t)ai->focus_n);
    mw_u32(&w, ai->focus ? ai->focus->id : TOKEN_ID_NONE);

    /* trace / last_output / last_skipped */
    mw_ids(&w, ai->trace, ai->trace_n);
    mw_ids(&w, ai->last_output, ai->last_output_n);
    mw_ids(&w, ai->last_skipped, ai->last_skipped_n);

    /* last_path */
    mw_u32(&w, (uint32_t)ai->last_path.n);
    for (size_t i = 0; i < ai->last_path.n; ++i)
        mw_u32(&w, ai->last_path.path[i] ? ai->last_path.path[i]->id : TOKEN_ID_NONE);

    /* 表达池 + 上次表达模式 */
    for (int i = 0; i < EXPR_COUNT; ++i)
        mw_u32(&w, ai->expr_root[i] ? ai->expr_root[i]->id : TOKEN_ID_NONE);
    mw_u32(&w, (uint32_t)(int32_t)ai->last_expr_mode);

    /* 时间线 */
    for (size_t i = 0; i < AI_MAX_HISTORY; ++i)
        mw_u32(&w, ai->timeline[i] ? ai->timeline[i]->id : TOKEN_ID_NONE);
    mw_u32(&w, (uint32_t)ai->timeline_n);
    mw_u32(&w, (uint32_t)ai->timeline_head);

    /* 他者模型 */
    mw_u32(&w, (uint32_t)ai->others_n);
    for (size_t i = 0; i < ai->others_n && i < AI_MAX_OTHERS; ++i) {
        mw_u32(&w, ai->others[i].who ? ai->others[i].who->id : TOKEN_ID_NONE);
        mw_u32(&w, ai->others[i].belief_of_self ? ai->others[i].belief_of_self->id
                                                : TOKEN_ID_NONE);
        mw_f32(&w, ai->others[i].trust);
        mw_u32(&w, (uint32_t)ai->others[i].n_interactions);
        /* v8：他的信念表。谁信什么必须存下来 —— 不存的话「归属」重载就没了，
         * 而它明明是**持久**知识（关系知识都存了，凭什么他的看法不存）。 */
        mw_u32(&w, (uint32_t)ai->others[i].n_bel_learned);
        mw_u32(&w, (uint32_t)ai->others[i].n_bel);
        for (size_t k = 0; k < ai->others[i].n_bel && k < AI_MAX_BELIEF; ++k) {
            const HypoFact *f = &ai->others[i].bel[k];
            mw_u32(&w, f->a ? f->a->id : TOKEN_ID_NONE);
            mw_u32(&w, f->b ? f->b->id : TOKEN_ID_NONE);
            mw_u8(&w, f->rid);
            mw_u8(&w, f->neg);
            /* v13：这条信念的**改口历史**（**定长** 20 字节 = total 4 + n 1 + 3×1 + 3×4） */
            {
                const BelHist *h = &ai->others[i].hist[k];
                uint8_t hn = (h->n <= AI_MAX_BELIEF_REV) ? h->n : 0;
                mw_u32(&w, h->total);
                mw_u8 (&w, hn);
                for (uint8_t q = 0; q < AI_MAX_BELIEF_REV; ++q)
                    mw_u8(&w, q < hn ? h->old_neg[q] : 0);
                for (uint8_t q = 0; q < AI_MAX_BELIEF_REV; ++q)
                    mw_u32(&w, q < hn ? h->tick[q] : 0);
            }
        }
    }

    /* 行为参数 / 计数器 */
    mw_f32(&w, ai->imagine_rate);
    mw_u32(&w, (uint32_t)ai->n_imaginations);
    mw_u32(&w, (uint32_t)ai->n_imagine_useful);
    mw_f32(&w, ai->inner_dialogue_gain);
    mw_u32(&w, (uint32_t)ai->n_inner_dialogues);
    mw_u32(&w, (uint32_t)ai->narrate_every);
    mw_u32(&w, (uint32_t)ai->n_narrations);
    mw_u32(&w, (uint32_t)ai->drift_sample_every);
    mw_u32(&w, (uint32_t)ai->n_drifts_detected);
    mw_f32(&w, ai->diag.prev_lexical_diversity);
    mw_u32(&w, (uint32_t)ai->diag.prev_perspective_n);
    mw_u32(&w, (uint32_t)ai->diag.prev_abstractions);
    mw_u32(&w, (uint32_t)ai->diag.prev_imaginations);
    mw_u32(&w, (uint32_t)ai->diag.prev_imagine_useful);
    mw_u64(&w, ai->diag.last_diag_tick);
    mw_u32(&w, (uint32_t)ai->n_diagnoses);
    mw_u32(&w, (uint32_t)ai->calib.n_confident);
    mw_u32(&w, (uint32_t)ai->calib.n_confident_wrong);
    mw_f32(&w, ai->calib.calibration_bias);
    mw_f32(&w, ai->last_confidence_at_output);
    mw_u32(&w, (uint32_t)ai->n_surprises);
    mw_u32(&w, (uint32_t)ai->n_abstractions);
    mw_u32(&w, (uint32_t)ai->n_sleeps);
    mw_u32(&w, (uint32_t)ai->n_gc_deleted_edges);
    mw_u32(&w, (uint32_t)ai->n_gc_deleted_tokens);
    mw_u32(&w, (uint32_t)ai->n_credit_events);
    mw_u32(&w, (uint32_t)ai->n_expr_learned);
    mw_u32(&w, (uint32_t)ai->n_counterfactual);
    mw_f32(&w, ai->last_confidence);
    mw_u32(&w, (uint32_t)(int32_t)ai->n_candidates);
    mw_u32(&w, (uint32_t)ai->sleep_every);
    mw_u32(&w, (uint32_t)ai->sleep_cursor);
    mw_u32(&w, (uint32_t)ai->sleep_target);
    mw_u32(&w, (uint32_t)(int32_t)ai->sleep_in_abstraction);

    /* 话题表：每个话题的「词 id 列表」 */
    for (uint8_t t = 0; t < AI_MAX_TOPICS; ++t) {
        uint32_t n = (uint32_t)ai->topics[t].n;
        if (n > 192u) n = 192u;
        mw_u32(&w, n);
        for (uint32_t k = 0; k < n; ++k)
            mw_u32(&w, ai->topics[t].seen[k] ? ai->topics[t].seen[k]->id : TOKEN_ID_NONE);
    }

    /* 整句检索库：输入句 id 序列 + 回复句 id 序列 + 话题 */
    mw_u32(&w, (uint32_t)ai->pairs_n);
    for (size_t i = 0; i < ai->pairs_n; ++i) {
        const AISentPair *p = &ai->pairs[i];
        mw_u32(&w, (uint32_t)p->in_n);
        for (size_t k = 0; k < p->in_n; ++k)
            mw_u32(&w, p->in[k] ? p->in[k]->id : TOKEN_ID_NONE);
        mw_u32(&w, (uint32_t)p->out_n);
        for (size_t k = 0; k < p->out_n; ++k)
            mw_u32(&w, p->out[k] ? p->out[k]->id : TOKEN_ID_NONE);
        mw_u8(&w, p->topic);
    }

    /* 动态关系名表（L2）：谓词表要能开放，就必须把「id→名字」存下来，
     * 否则存盘重载之后 id 对不上号。 */
    /* **v11：N1 思考对象**（§6.110）—— 存"摘要"（结论/缺口数/复没复核），不是整条推导链。
     * ⚠ 位置在动态关系表**之前**：读写两侧必须同位置（`mver` 门控保证旧档没有这一段）。 */
    mw_u32(&w, (uint32_t)ai->n1_gaps);
    mw_u32(&w, (uint32_t)ai->n1_verified);
    /* ⚠ `mw_str` **自带长度**（它内部先写 `mw_u32(n)`）—— 别再自己写一遍长度，
     * 否则读侧只读一次长度 ⇒ 整段错位（这一版就是这么栽的，见 §6.110 踩坑）。 */
    mw_str(&w, ai->n1_concl, (uint32_t)strlen(ai->n1_concl));
    mw_u32(&w, (uint32_t)ai->dynrel_n);
    for (size_t i = 0; i < ai->dynrel_n; ++i) {
        const char *nm = ai->dynrel_name[i] ? ai->dynrel_name[i] : "";
        /* 只用 mw_str：它内部**已经写长度前缀**了。外面再手写一个长度就会
         * 变成 [len][len][bytes]，读回来错位 → 名字变垃圾 → 动态关系重复登记、
         * 存盘重载后 hits 翻倍。这个 bug 是被 embed.sh 的
         * 「读盘前后语义邻居一致」抓出来的。 */
        mw_str(&w, nm, (uint32_t)strlen(nm));
    }

    if (w.err) return 0;
    return w.len;
}

/* 写文件。返回写入字节数，0 表示失败。 */
static inline size_t ai_save_file(const AI *ai, const char *path) {
    if (!ai || !path) return 0;
    size_t sz = ai_snapshot(ai, NULL, 0);
    if (sz == 0) return 0;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return 0;
    size_t got = ai_snapshot(ai, buf, sz);
    if (got != sz) { free(buf); return 0; }
    /* **原子写**（借 §6.88 那条经验）：先写同目录的临时文件 → fsync 落盘 → rename 原子替换。
     * 为什么非改不可：原来直接 `fopen(path,"wb")` 写目标 —— 中途崩/满盘/断电，
     * 模型文件就**截断**了，而模型就是它的全部记忆（写坏 = 记忆清零，比丢一次快照严重得多）。
     * rename 在同一个文件系统上是原子的：要么旧文件完好，要么新文件完整，没有中间态。
     *
     * §6.583 **临时名必须唯一**：以前是固定的 `path.tmp` ⇒ 两个进程同时存同一路径时，
     * 它们写的是**同一个**临时文件（互相截断/覆盖），rename 出来是一份混着两边的坏文件。
     * 现在带 `pid + 序号`（`saveio.h`），并发写同一路径的语义变成"最后 rename 的人赢"。 */
    char tmp[1024];
    if (aip_tmp_name(path, tmp, sizeof tmp) != 0) { free(buf); return 0; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); return 0; }
    size_t wrote = fwrite(buf, 1, sz, f);
    int bad = (wrote != sz);
    if (!bad && aip_sync_close(f) != 0) bad = 1;   /* fflush + fsync + fclose（先落盘再替换）*/
    else if (bad) fclose(f);
    if (bad) { remove(tmp); free(buf); return 0; }
    if (aip_commit_rename(tmp, path) != 0) { remove(tmp); free(buf); return 0; }
    free(buf);
    return sz;
}

/* 建目录（父目录用，已存在算成功） */
static inline int model_mkdir_p(const char *path) {
    if (!path || !*path) return -1;
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, 0777) == 0) return 0;
    return (errno == EEXIST) ? 0 : -1;
}

/* ---------------------------------------------------------------- 读盘 */

static inline Token *mio_tok(const TokenPool *p, uint32_t id) {
    if (id == TOKEN_ID_NONE) return NULL;
    return pool_token_by_id(p, id);
}

/* **把"学出来的可信度"的证据从图里重新数出来**（读盘之后调一次）。
 *
 * 为什么需要：`n_rel_learned / n_revised / n_cached` 这几个计数器**没进存档**
 * （存档格式不动是硬约束），于是"改口越多观测档越低"这件事一重启就归零 ——
 * 那等于假的学习。好在证据**本来就写在图里**：
 *   · 现场学到的关系事实 = 带关系位且 **role ≠ 基准** 的边（基准是装载时打的标）；
 *   · 改口次数           = `[修正:…]` 标记节点的个数（改口时经安全内核落下的）；
 *   · 现在还挂着的缓存   = `role == ASSOC_ROLE_DERIVED` 的边数。
 * 和 emb/idf/槽类一样：**是从最终的图现算的**，不是另存一份。
 * ~~局限：被撤销的缓存数（n_retracted）恢复不了~~ **已修（§6.78）**：撤销时会在图上留一条
 * `[撤销:A|R|B]` 标记节点（受审通道写），这里把它**数回来** —— 于是重载后推论档的后验不再偏乐观。
 * 旧档（没有标记的那些）仍然恢复不了：那是历史，写清楚就行。 */
static inline void ai_recount_evidence(AI *ai) {
    if (!ai || !ai->pool) return;
    size_t rel_learned = 0, revised = 0, cached = 0, revoked = 0;
    const size_t plen = strlen(MICRO_REVISE_TOKEN_PREFIX);
    const size_t vlen = strlen(MICRO_REVOKE_TOKEN_PREFIX);
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        if (t->name && strncmp(t->name, MICRO_REVISE_TOKEN_PREFIX, plen) == 0) revised++;
        if (t->name && strncmp(t->name, MICRO_REVOKE_TOKEN_PREFIX, vlen) == 0) revoked++;
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &token_assocs(t)[j];
            if (!e->main) continue;
            if (e->rel || e->nrel) {
                /* 基准（装载时打的标）不算"现场学到"；**推论缓存也不算**（它另有一本账） */
                if (e->role != ASSOC_ROLE_INNATE && e->role != ASSOC_ROLE_DERIVED)
                    rel_learned++;
            }
            if (e->role == ASSOC_ROLE_DERIVED) cached++;
        }
    }
    ai->n_rel_learned = rel_learned;
    ai->n_revised = revised;
    ai->n_cached = cached;
    ai->n_retracted = revoked;      /* 从图里数回来（§6.78） */
}

static inline AI *ai_load_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long flen = ftell(f);
    if (flen <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)flen);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    if (rd != (size_t)flen) { free(buf); return NULL; }

    /* 关键：这里不能用 ai_new()。ai_new() 会先建一个自己的池，并把该池的符号节点
     * 写进表达池的边（ai_init_expr_pools）。如果之后把 ai->pool 换成快照池，那些边
     * 就指向已经 free 掉的内存 —— 症状是几轮之后 token_add_assoc_ex 堆越界。
     * 所以这里只 calloc 一个空 AI 骨架，全部符号节点都从快照池里长出来。 */
    AI *ai = (AI *)calloc(1, sizeof *ai);
    if (!ai) { free(buf); return NULL; }
    TokenPool *np = NULL;   /* 提前声明：fail 路径按「是否已建好新池」决定释放 */

    MReader r = { buf, (size_t)flen, 0, 0 };
    if (mr_u32(&r) != MODEL_MAGIC) goto fail;
    uint32_t mver = mr_u32(&r);
    if (mver < 1u || mver > MODEL_VERSION) goto fail;
    int has_role    = (mver >= 2u);
    int has_topic   = (mver >= 3u);
    int has_pattern = (mver >= 4u);
    int has_rel     = (mver >= 6u);
    int has_nrel    = (mver >= 7u);
    int has_belief  = (mver >= 8u);
    int has_wide_rel = (mver >= 9u);   /* v9：rel/nrel 是 2 字节 */
    int has_ctx      = (mver < 10u);   /* v10 起：边上**没有**语境字段（旧文件要跳过去） */
    if (mr_u32(&r) != (uint32_t)EMB_DIM) goto fail;
    uint32_t n_tok   = mr_u32(&r);
    uint32_t n_assoc = mr_u32(&r);
    uint32_t depth   = mr_u32(&r);
    if (r.err || n_tok == 0 || n_tok > 20000000u) goto fail;
    uint64_t assoc_seen = 0;

    np = pool_new();
    if (!np) goto fail;

    /* 两遍读：
     *   第一遍只建符号节点骨架 + 把边当成「4 字节裸 id」堆在边上，不做任何指针解析；
     *   第二遍（池里 0..n-1 全都有名字之后）再把 id 换成符号节点*。
     * 必须在第二遍才解析，因为边可以引用文件里更靠后的符号节点（例如 id 0 的「我」
     * 很可能比引用它的符号节点先出现），第一遍就去解析会拿到 NULL 而误判成损坏。 */
    uint32_t *tok_parent = NULL, *tok_next = NULL, *tok_persp = NULL;

    tok_parent = (uint32_t *)calloc(n_tok, sizeof *tok_parent);
    tok_next   = (uint32_t *)calloc(n_tok, sizeof *tok_next);
    tok_persp  = (uint32_t *)calloc(n_tok, sizeof *tok_persp);
    if (!tok_parent || !tok_next || !tok_persp) goto fail;

    for (uint32_t i = 0; i < n_tok; ++i) {
        uint32_t id  = mr_u32(&r);
        uint32_t nl  = mr_u32(&r);
        if (r.err || nl > (1u << 20)) goto fail;
        char *name = NULL;
        if (nl) {
            name = (char *)malloc((size_t)nl + 1);
            if (!name) goto fail;
            if (mr_raw(&r, name, nl) != 0) { free(name); goto fail; }
            name[nl] = 0;
        }
        Token *t = pool_token(np, name, NULL);
        free(name);
        if (!t || t->id != id) goto fail;

        /* §6.299 v13 及更早：每个符号节点带两份 1024 字节向量（hv/hv_prev）。
         * 读旧档时照样把字节**吃掉**（读到临时缓冲再丢），这样旧 model/session 不用重训。 */
        if (mver <= 13u) {
            int8_t scratch[EMB_DIM];
            for (int k = 0; k < 2; ++k) {
                if (mr_u8(&r)) { if (mr_raw(&r, scratch, EMB_DIM) != 0) goto fail; }
                else           { if (mr_raw(&r, NULL, EMB_DIM) != 0) goto fail; }
            }
        }

        t->misses        = mr_u32(&r);
        t->recent_misses = mr_u32(&r);
        tok_parent[i] = mr_u32(&r);
        tok_next[i]   = mr_u32(&r);
        tok_persp[i]  = mr_u32(&r);
        t->is_category = mr_u8(&r);
        t->is_garbage  = mr_u8(&r);
        t->mem_state   = mr_u8(&r);
        t->birth_tick      = mr_u64(&r);
        t->last_touch_tick = mr_u64(&r);
        uint32_t na = mr_u32(&r);
        if (r.err || na > 20000000u) goto fail;
        assoc_seen += na;

        /* cap 不进文件：按 token_add_assoc_ex 的「count==cap 时先翻倍再写、
         * 起步 4」那条序列还原，这样加载后的内存态与训练出来的内存态连指纹都一致。
         * 注意：分配必须按 cap 而不是按 count —— 只分配 count 个元素、却把 cap
         * 报成更大的值，下一次追加就会直接踩到分配区外面。 */
        if (na == 0) {
            t->cap = 0;
        } else {
            size_t canon = 4;
            while (canon <= na) canon *= 2;
            t->cap = canon;
            /* 边直接落进 t->assocs。刻意不用 token_add_assoc_ex：它会把
             * mem_state/birth_tick 重置成默认值，而我们要的是快照里那份。 */
            t->assocs = (AssocEntry *)calloc(t->cap, sizeof *t->assocs);
            if (!t->assocs) goto fail;
        }
        t->count = na;      /* 边表已就位（na 可能为 0）；字段在第二遍填 */
        /* 第一遍只把每条边整体跳过：不解析 id，只在第二遍做。
         * 每条边的裸 id 数是 main + action；纯标量部分是
         * hits/seq/mem_state/birth/last_touch = 4+4+1+8+8 = 25 字节。
         * v9 及以前：**最前面还有语境两段**（n_subs + 裸 id / n_not + 裸 id），按 has_ctx 跳。 */
        for (uint32_t j = 0; j < na; ++j) {
            if (has_ctx) {
                uint8_t ns = mr_u8(&r);
                if (r.err || ns > TOKEN_CTX_MAX) goto fail;
                if (mr_raw(&r, NULL, 4u * ns) != 0) goto fail;
                uint8_t nn = mr_u8(&r);
                if (r.err || nn > TOKEN_CTX_MAX) goto fail;
                if (mr_raw(&r, NULL, 4u * nn) != 0) goto fail;
            }
            if (mr_raw(&r, NULL, 4u + 4u + 4u + 4u + 1u + 8u + 8u
                              + (has_role ? 1u : 0u) + (has_topic ? 1u : 0u)
                              + (has_pattern ? 1u : 0u)
                              + (has_rel  ? (has_wide_rel ? 2u : 1u) : 0u)
                              + (has_nrel ? (has_wide_rel ? 3u : 2u) : 0u)
                              + (mver >= 12u ? 17u : 0u)) != 0)   /* v12：n_tset(1) + 4 个 tick(16)，**定长** */
                goto fail;
        }
    }

    /* ---- 池层 ---- */
    uint64_t cur_tick = mr_u64(&r);
    (void)mr_u32(&r);   /* 原 n_entries，读盘后由 hash 重建 */
    if (r.err) goto fail;
    if (np->count != n_tok || assoc_seen != (uint64_t)n_assoc) goto fail;

    /* 现在池里 0..n-1 齐了，重扫一遍把所有裸 id 换成符号节点* */
    {
        MReader rr = { buf, (size_t)flen, 0, 0 };
        (void)mr_u32(&rr); (void)mr_u32(&rr); (void)mr_u32(&rr);
        (void)mr_u32(&rr); (void)mr_u32(&rr); (void)mr_u32(&rr);
        for (uint32_t i = 0; i < n_tok; ++i) {
            uint32_t id = mr_u32(&rr);
            uint32_t nl = mr_u32(&rr);
            if (rr.err || nl > (1u << 20)) goto fail;
            if (mr_raw(&rr, NULL, nl) != 0) goto fail;
            /* §6.299 第二趟重扫：v13 及更早这里还有两段向量要跳过（v14 起没有）。 */
            if (mver <= 13u) {
                if (mr_u8(&rr)) { if (mr_raw(&rr, NULL, EMB_DIM) != 0) goto fail; }
                if (mr_u8(&rr)) { if (mr_raw(&rr, NULL, EMB_DIM) != 0) goto fail; }
            }
            (void)mr_u32(&rr); (void)mr_u32(&rr);
            (void)mr_u32(&rr); (void)mr_u32(&rr); (void)mr_u32(&rr);
            (void)mr_u8(&rr); (void)mr_u8(&rr); (void)mr_u8(&rr);
            (void)mr_u64(&rr); (void)mr_u64(&rr);
            uint32_t na = mr_u32(&rr);
            if (rr.err || na > 20000000u) goto fail;
            Token *t = pool_token_by_id(np, id);
            if (!t || t->count != na) goto fail;
            for (uint32_t j = 0; j < na; ++j) {
                AssocEntry *e = &token_assocs(t)[j];
                if (has_ctx) {
                    /* 旧格式的语境字段：**跳过去**（字段在 v10 里已删）。
                     * 出厂模型实测全 0 ⇒ 无损；非空的旧文件会少一层条件（登记在案，§6.50）。 */
                    uint8_t ns = mr_u8(&rr);
                    if (rr.err || ns > TOKEN_CTX_MAX) goto fail;
                    if (mr_raw(&rr, NULL, 4u * ns) != 0) goto fail;
                    uint8_t nn = mr_u8(&rr);
                    if (rr.err || nn > TOKEN_CTX_MAX) goto fail;
                    if (mr_raw(&rr, NULL, 4u * nn) != 0) goto fail;
                }
                uint32_t main_id = mr_u32(&rr);
                uint32_t act_id  = mr_u32(&rr);
                e->hits       = mr_u32(&rr);
                e->seq        = mr_u32(&rr);
                e->mem_state  = mr_u8(&rr);
                e->birth_tick = mr_u64(&rr);
                e->last_touch_tick = mr_u64(&rr);
                if (has_role) e->role = mr_u8(&rr);
                else          e->role = ASSOC_ROLE_NONE;
                if (has_topic) e->topic = mr_u8(&rr);
                else           e->topic = ASSOC_TOPIC_NONE;
                if (has_pattern) e->pattern = mr_u8(&rr);
                else             e->pattern = ASSOC_PAT_NONE;
                if (has_rel) e->rel = has_wide_rel ? mr_u16(&rr) : mr_u8(&rr);
                else         e->rel = 0;
                if (has_nrel) {
                    e->nrel = has_wide_rel ? mr_u16(&rr) : mr_u8(&rr);
                    e->dyn = mr_u8(&rr);
                } else { e->nrel = 0; e->dyn = 0; }
                if (mver >= 12u) {         /* v12：时刻集合（旧档没有 ⇒ 保持 0 = 空集合） */
                    e->n_tset = mr_u8(&rr);
                    if (rr.err || e->n_tset > 4) goto fail_ai;
                    for (uint8_t k = 0; k < 4; ++k) {       /* **定长**：恒读 4 个槽 */
                        uint32_t tv = mr_u32(&rr);
                        if (k < e->n_tset) e->tset[k] = tv;
                    }
                    if (rr.err) goto fail_ai;
                }
                if (rr.err) goto fail;
                if (main_id != TOKEN_ID_NONE) {
                    if (main_id >= np->count) goto fail;
                    e->main = np->tokens[main_id];
                }
                if (act_id != TOKEN_ID_NONE) {
                    if (act_id >= np->count) goto fail;
                    e->action = np->tokens[act_id];
                }
                if (e->seq != TOKEN_SEQ_NONE && e->seq >= t->next_seq)
                    t->next_seq = e->seq + 1;
            }
        }
    }

    /* 挂起指针（parent / next_in_time / perspective_of） */
    for (size_t i = 0; i < np->count; ++i) {
        Token *t = np->tokens[i];
        t->owner = np;
        if (tok_parent[i] != TOKEN_ID_NONE) {
            if (tok_parent[i] >= np->count) goto fail;
            t->parent = np->tokens[tok_parent[i]];
        }
        if (tok_next[i] != TOKEN_ID_NONE) {
            if (tok_next[i] >= np->count) goto fail;
            t->next_in_time = np->tokens[tok_next[i]];
        }
        if (tok_persp[i] != TOKEN_ID_NONE) {
            if (tok_persp[i] >= np->count) goto fail;
            t->perspective_of = np->tokens[tok_persp[i]];
        }
    }
    free(tok_parent); free(tok_next); tok_parent = tok_next = NULL;
    free(tok_persp); tok_persp = NULL;
    np->current_tick = cur_tick;
    /* unknowing 指针 */
    for (size_t i = 0; i < np->count; ++i) {
        const char *nm = np->tokens[i]->name;
        if (nm && strcmp(nm, TOKEN_UNKNOWING_NAME) == 0) { np->unknowing = np->tokens[i]; break; }
    }
    /* [原语:分割] 指针 —— 存盘格式里**漏了**它（self/good/bad/unknowing/causal…
     * 都按 id 存了，唯独它没有），所以必须在这里按名字找回来。
     * 少了它的症状极其隐蔽：ai_edge_weight() 里「从分割符出发 = 回复首词，权重 ×4」
     * 那一档直接失效，装载态每条边的权重都只有训练态的 1/4 ——
     * 词池逐字节完全相同，可走出来的联想链和回复完全不一样，
     * tests/model_roundtrip.sh 的 [3/4] 就是被它一直挡住的。 */
    ai->split_tok = pool_find(np, AI_DIALOG_SPLIT_NAME);
    /* 视角栈按深度重放 */
    if (depth) {
        /* 视角在快照里只留了深度；用池里名字带 [视角: 的符号节点无法可靠还原，
           这里保守地保持 root——深度不为 0 只可能出现在语料跑 [原语:视角] 中途存盘，
           训练/对话存档时深度恒为 0。 */
        (void)depth;
    }

    /* ---- AI 层 ---- */
    ai->pool = np;
    /* 先铺一遍 ai_new() 的常量默认值；下面从文件读到的字面量会逐个覆盖。 */
    ai->rng = 0x9E3779B9u;
    ai->gc_rng = 0x85EBCA6Bu;
    ai->energy = 1.0f; ai->decay = 0.7f; ai->cutoff = 0.05f;
    ai->max_gen = 8;
    ai->consolidate_every = 50; ai->decay_factor = 0.98f;
    ai->gc.gc_min_age = 3000; ai->gc.gc_time_scale = 1000;
    ai->gc.gc_upgrade_rate = 0.15f; ai->gc.gc_interval = 100;
    ai->gc.gc_delete_state = MEM_FORGOTTEN;
    ai->uncertain.w_gap = 0.30f; ai->uncertain.w_conflict = 0.15f;
    ai->uncertain.w_activation = 0.25f; ai->uncertain.w_effort = 0.15f;
    ai->uncertain.w_novelty = 0.15f;
    ai->uncertain.certain_threshold = 0.60f;
    ai->uncertain.unknown_threshold = 0.35f;
    ai->setpoint.setpoint_comfort = 0.70f;
    ai->setpoint.setpoint_arousal = 0.50f;
    ai->setpoint.regulation_rate = 0.15f;
    ai->inner.predict_accuracy = 0.5f;
    ai->inner.comfort = 0.5f; ai->inner.arousal = 0.5f;
    ai->n_candidates = 3;
    ai->sleep_every = 200;
    ai->last_expr_mode = -1;
    /* 后加的开关/测量钩子集中在一处（`ai.h` 的 ai_switches_init）——
     * 读盘路径**不调用 ai_new()**，漏了这一句就会拿到 calloc 的 0（实测踩过）。 */
    ai_switches_init(ai);
    ai->imagine_rate = 0.03f;
    ai->inner_dialogue_gain = 0.15f;
    ai->narrate_every = 500;
    ai->drift_sample_every = 200;
    /* §6.184 `sleep_max_visits` 也必须在这里对齐（读盘骨架不走 `ai_new`）：
     * 以前它只靠 `ai_sleep_step` 里那句 `? : AI_SLEEP_MAX_VISITS` 兜着，
     * 于是 `AIP_SLEEPVISITS` 对**载入的模型一点用都没有**（实测 1000/4000/20000
     * 三个值跑出来访问数一模一样，全是 440010）——测量钩子形同虚设。 */
    /* §6.188：载入的模型没有"内存里的 pending"（它只在内存），但可以**从标记节点恢复** ——
     * `agent_step` 第一轮会扫一遍 `[追问:…]`。这里只保证几个字段是干净的初值。 */
    ai->ask_pending = 0;
    ai->ask_restored = 0;
    ai->ask_mark[0] = 0;
    ai->ask_kind = 0;
    ai->ask_fact_neg = 0;
    ai->sleep_max_visits = AI_SLEEP_MAX_VISITS;
    { const char *sv = getenv("AIP_SLEEPVISITS");
      if (sv && sv[0]) { long v = strtol(sv, NULL, 10); if (v > 0) ai->sleep_max_visits = (size_t)v; } }
    /* 读盘骨架的默认值必须和 ai_new 对齐，否则训练/加载两条路径行为分叉
     * （n_candidates 就是这么坑过一次）。assoc_slot_off 用反向命名 +
     * calloc 本来就给 0，这里显式写一遍只为把意图留在代码里。 */
    ai->assoc_slot_off = 0;
    ai->coin_off = 0;
    ai->seg_off = 0;
    ai->relay_off = 0;
    ai->fall_off = 0;
    ai->rel_off = 0;
    ai->der_style = DER_STYLE_PLAIN;
    ai->closed_world = 0;
    ai->view_who = NULL;
    for (int i = 0; i <= REL_TABLE_MAX; ++i) ai->rel_excl[i] = 0;
    ai->rel_excl[REL_IS_A] = 1;
    ai->rel_excl[REL_AT] = 1;
    ai->rel_excl[REL_NEED] = 0;

    ai->rng = mr_u32(&r);
    ai->gc_rng = mr_u32(&r);
    ai->turns = (size_t)mr_u64(&r);
    ai->n_learned = (size_t)mr_u64(&r);
    ai->energy = mr_f32(&r);
    ai->decay  = mr_f32(&r);
    ai->cutoff = mr_f32(&r);
    ai->max_gen = mr_u32(&r);
    ai->consolidate_every = mr_u32(&r);
    ai->decay_factor = mr_f32(&r);

    ai->gc.gc_min_age = mr_u64(&r);
    ai->gc.gc_time_scale = mr_u64(&r);
    ai->gc.gc_upgrade_rate = mr_f32(&r);
    ai->gc.gc_interval = mr_u32(&r);
    ai->gc.gc_delete_state = mr_u8(&r);
    ai->uncertain.w_gap = mr_f32(&r);
    ai->uncertain.w_conflict = mr_f32(&r);
    ai->uncertain.w_activation = mr_f32(&r);
    ai->uncertain.w_effort = mr_f32(&r);
    ai->uncertain.w_novelty = mr_f32(&r);
    ai->uncertain.certain_threshold = mr_f32(&r);
    ai->uncertain.unknown_threshold = mr_f32(&r);
    ai->setpoint.setpoint_comfort = mr_f32(&r);
    ai->setpoint.setpoint_arousal = mr_f32(&r);
    ai->setpoint.regulation_rate = mr_f32(&r);

    ai->inner.predict_accuracy = mr_f32(&r);
    ai->inner.surprise_level = mr_f32(&r);
    ai->inner.comfort = mr_f32(&r);
    ai->inner.discomfort = mr_f32(&r);
    ai->inner.arousal = mr_f32(&r);
    ai->inner.fatigue = mr_f32(&r);
    ai->meta.confidence = mr_f32(&r);
    ai->meta.familiarity = mr_f32(&r);
    ai->meta.fluency = mr_f32(&r);
    ai->meta.effort = mr_f32(&r);
    ai->meta.gap_feel = mr_f32(&r);
    ai->meta.total = mr_f32(&r);

    ai->self        = mio_tok(np, mr_u32(&r));
    ai->good        = mio_tok(np, mr_u32(&r));
    ai->bad         = mio_tok(np, mr_u32(&r));
    ai->unknowing   = mio_tok(np, mr_u32(&r));
    ai->causal      = mio_tok(np, mr_u32(&r));
    ai->social_good = mio_tok(np, mr_u32(&r));
    ai->social_bad  = mio_tok(np, mr_u32(&r));
    ai->past_self   = mio_tok(np, mr_u32(&r));
    ai->future_self = mio_tok(np, mr_u32(&r));
    if (r.err) goto fail_ai;
    if (!ai->self || !ai->good || !ai->bad || !ai->unknowing || !ai->causal) goto fail_ai;

    for (size_t i = 0; i < AI_MAX_FOCUS; ++i) {
        ai->focus_ring[i]   = mio_tok(np, mr_u32(&r));
        ai->focus_weight[i] = mr_f32(&r);
    }
    ai->focus_n = mr_u32(&r);
    if (ai->focus_n > AI_MAX_FOCUS) goto fail_ai;
    ai->focus = mio_tok(np, mr_u32(&r));

    /* trace 的容量没存进快照：多留一倍余量，避免刚读回来就涨容 */
    if (mr_ids(&r, np, &ai->trace, &ai->trace_cap, &ai->trace_n, 1u << 24) != 0) goto fail_ai;
    if (ai->trace_n) {
        size_t want = ai->trace_n * 2;
        Token **nt = (Token **)realloc(ai->trace, want * sizeof *nt);
        if (!nt) goto fail_ai;
        ai->trace = nt;
        ai->trace_cap = want;
    }
    if (mr_ids(&r, np, &ai->last_output, &ai->last_output_cap, &ai->last_output_n,
               1u << 24) != 0) goto fail_ai;
    if (mr_ids(&r, np, &ai->last_skipped, &ai->last_skipped_cap, &ai->last_skipped_n,
               1u << 24) != 0) goto fail_ai;

    ai->last_path.n = mr_u32(&r);
    if (ai->last_path.n > AI_MAX_PATH) goto fail_ai;
    for (size_t i = 0; i < ai->last_path.n; ++i)
        ai->last_path.path[i] = mio_tok(np, mr_u32(&r));

    for (int i = 0; i < EXPR_COUNT; ++i)
        ai->expr_root[i] = mio_tok(np, mr_u32(&r));
    ai->last_expr_mode = (int)(int32_t)mr_u32(&r);
    /* 表达池的变体边（应该/大概/… ）在快照池里就有，直接查表补上，
     * 保证它们指向的是快照池而不是任何旧池。 */
    ai_init_expr_pools(ai);

    for (size_t i = 0; i < AI_MAX_HISTORY; ++i)
        ai->timeline[i] = mio_tok(np, mr_u32(&r));
    ai->timeline_n = mr_u32(&r);
    ai->timeline_head = mr_u32(&r);
    if (ai->timeline_n > AI_MAX_HISTORY || ai->timeline_head >= AI_MAX_HISTORY) goto fail_ai;

    ai->others_n = mr_u32(&r);
    if (ai->others_n > AI_MAX_OTHERS) goto fail_ai;
    memset(ai->others, 0, sizeof ai->others);
    for (size_t i = 0; i < ai->others_n; ++i) {
        ai->others[i].who = mio_tok(np, mr_u32(&r));
        ai->others[i].belief_of_self = mio_tok(np, mr_u32(&r));
        ai->others[i].trust = mr_f32(&r);
        ai->others[i].n_interactions = mr_u32(&r);
        if (has_belief) {
            ai->others[i].n_bel_learned = mr_u32(&r);
            uint32_t nb = mr_u32(&r);
            if (r.err || nb > AI_MAX_BELIEF) goto fail_ai;
            for (uint32_t k = 0; k < nb; ++k) {
                Token *ba = mio_tok(np, mr_u32(&r));
                Token *bb = mio_tok(np, mr_u32(&r));
                uint8_t brid = mr_u8(&r);
                uint8_t bneg = mr_u8(&r);
                if (r.err) goto fail_ai;
                if (!ba || !bb) goto fail_ai;
                HypoFact *f = &ai->others[i].bel[ai->others[i].n_bel];
                f->a = ba; f->b = bb; f->rid = brid; f->neg = bneg;
                memset(&ai->others[i].hist[ai->others[i].n_bel], 0,
                       sizeof ai->others[i].hist[ai->others[i].n_bel]);
                ai->others[i].n_bel++;
                if (mver >= 13u) {                 /* v13：改口历史（旧档没有 ⇒ 空历史） */
                    BelHist *h = &ai->others[i].hist[ai->others[i].n_bel - 1];
                    h->total = mr_u32(&r);
                    h->n = mr_u8(&r);
                    if (r.err || h->n > AI_MAX_BELIEF_REV) goto fail_ai;
                    for (uint8_t q = 0; q < AI_MAX_BELIEF_REV; ++q) {
                        uint8_t ov = mr_u8(&r);
                        if (q < h->n) h->old_neg[q] = ov;
                    }
                    for (uint8_t q = 0; q < AI_MAX_BELIEF_REV; ++q) {
                        uint32_t tv = mr_u32(&r);
                        if (q < h->n) h->tick[q] = tv;
                    }
                    if (r.err) goto fail_ai;
                }
            }
        }
    }

    ai->imagine_rate = mr_f32(&r);
    ai->n_imaginations = mr_u32(&r);
    ai->n_imagine_useful = mr_u32(&r);
    ai->inner_dialogue_gain = mr_f32(&r);
    ai->n_inner_dialogues = mr_u32(&r);
    ai->narrate_every = mr_u32(&r);
    ai->n_narrations = mr_u32(&r);
    ai->drift_sample_every = mr_u32(&r);
    ai->n_drifts_detected = mr_u32(&r);
    ai->diag.prev_lexical_diversity = mr_f32(&r);
    ai->diag.prev_perspective_n = mr_u32(&r);
    ai->diag.prev_abstractions = mr_u32(&r);
    ai->diag.prev_imaginations = mr_u32(&r);
    ai->diag.prev_imagine_useful = mr_u32(&r);
    ai->diag.last_diag_tick = mr_u64(&r);
    ai->n_diagnoses = mr_u32(&r);
    ai->calib.n_confident = mr_u32(&r);
    ai->calib.n_confident_wrong = mr_u32(&r);
    ai->calib.calibration_bias = mr_f32(&r);
    ai->last_confidence_at_output = mr_f32(&r);
    ai->n_surprises = mr_u32(&r);
    ai->n_abstractions = mr_u32(&r);
    ai->n_sleeps = mr_u32(&r);
    ai->n_gc_deleted_edges = mr_u32(&r);
    ai->n_gc_deleted_tokens = mr_u32(&r);
    ai->n_credit_events = mr_u32(&r);
    ai->n_expr_learned = mr_u32(&r);
    ai->n_counterfactual = mr_u32(&r);
    ai->last_confidence = mr_f32(&r);
    ai->n_candidates = (int)(int32_t)mr_u32(&r);   /* 基准候选数，逐轮被 bias 调制 */
    ai->sleep_every = mr_u32(&r);
    ai->sleep_cursor = mr_u32(&r);
    ai->sleep_target = mr_u32(&r);
    ai->sleep_in_abstraction = (int)(int32_t)mr_u32(&r);

    /* 话题表：每个话题的「词 id 列表」（写入顺序：话题 → 整句库） */
    memset(ai->topics, 0, sizeof ai->topics);
    for (uint8_t t = 0; t < AI_MAX_TOPICS; ++t) {
        uint32_t n = mr_u32(&r);
        if (r.err || n > 192u) goto fail_ai;
        ai->topics[t].n = 0;
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t id = mr_u32(&r);
            if (r.err) goto fail_ai;
            if (id == TOKEN_ID_NONE) continue;
            Token *tk = pool_token_by_id(np, id);
            if (!tk) goto fail_ai;
            if (ai->topics[t].n < 192) ai->topics[t].seen[ai->topics[t].n++] = tk;
        }
    }

    /* 整句检索库：输入句 id 序列 + 回复句 id 序列 + 话题 */
    ai->pairs_n = 0;
    if (mver >= 5u) {
        uint32_t npair = mr_u32(&r);
        if (r.err || npair > 4000000u) goto fail_ai;
        if (npair) {
            ai->pairs = (AISentPair *)calloc(npair, sizeof *ai->pairs);
            if (!ai->pairs) goto fail_ai;
            ai->pairs_cap = npair;
        }
        for (uint32_t i = 0; i < npair; ++i) {
            AISentPair *p = &ai->pairs[i];
            uint32_t inn = mr_u32(&r);
            if (r.err || inn > AI_MAX_SENT) goto fail_ai;
            p->in_n = inn;
            for (uint32_t k = 0; k < inn; ++k) {
                uint32_t id = mr_u32(&r);
                if (r.err) goto fail_ai;
                p->in[k] = (id == TOKEN_ID_NONE) ? NULL : pool_token_by_id(np, id);
                if (id != TOKEN_ID_NONE && !p->in[k]) goto fail_ai;
            }
            uint32_t on = mr_u32(&r);
            if (r.err || on > AI_MAX_SENT) goto fail_ai;
            p->out_n = on;
            for (uint32_t k = 0; k < on; ++k) {
                uint32_t id = mr_u32(&r);
                if (r.err) goto fail_ai;
                p->out[k] = (id == TOKEN_ID_NONE) ? NULL : pool_token_by_id(np, id);
                if (id != TOKEN_ID_NONE && !p->out[k]) goto fail_ai;
            }
            p->topic = mr_u8(&r);
            if (r.err) goto fail_ai;
        }
        ai->pairs_n = npair;
    }
    ai->use_retrieval = 1;
    /* 动态关系名表（L2，v7 起）：必须**在** relations_load 之前读回来，
     * 否则 relations.txt 里用到的动态谓语会被重新分配 id，存盘前后对不上号。 */
    if (mver >= 11u) {                 /* v11：N1 思考对象（旧档没有这一段 ⇒ 保持 0 = 今天的行为） */
        ai->n1_gaps = (int)mr_u32(&r);
        ai->n1_verified = (int)mr_u32(&r);
        uint32_t nl = mr_u32(&r);
        if (r.err || nl >= sizeof ai->n1_concl) goto fail_ai;
        if (nl) { if (mr_raw(&r, ai->n1_concl, nl) != 0) goto fail_ai; }
        ai->n1_concl[nl] = 0;
    }
    if (mver >= 7u) {
        uint32_t nd = mr_u32(&r);
        if (r.err || nd > AI_MAX_DYNREL) goto fail_ai;
        for (uint32_t i = 0; i < nd; ++i) {
            uint32_t len = mr_u32(&r);
            if (r.err || len > 4096u) goto fail_ai;
            char *nm = (char *)malloc(len + 1);
            if (!nm) goto fail_ai;
            if (len) { if (mr_raw(&r, nm, len) != 0) { free(nm); goto fail_ai; } }
            nm[len] = 0;
            ai->dynrel_name[ai->dynrel_n++] = nm;
        }
    }
    np->loading = 1;                      /* §6.111：下面这段重建里的 touch 不算"观测" */
    rel_names_intern(ai);                 /* 关系词先进词池，否则会被切碎 */
    relations_load(ai, "relations.txt");  /* 静态关系知识（像 frames.txt 一样现读） */
    rel_excl_build(ai);                   /* 「独占」由数据量出来（声明值只当种子） */
    /* 顺序要紧：关系边要算进 emb，所以 relations_load 必须在 ai_embed_build 之前。
     * ⚠ 这三行**曾经被一次字符串替换整段删掉**，症状是「读盘后帧库是空的」
     *   （`::frames` 报「没有帧库」）→ 帧层与检索块里的思维链步骤全部消失。
     *   tests/all.sh 的冒烟现在会硬断言这三层加载后都在（见 tests/all.sh）。 */
    frames_load(ai, "frames.txt");   /* 结构化帧库同理（从文件现读） */
    /* ⚠ 三个派生层必须排在**所有会造词/加边的步骤之后**，而且与 main.c 的
     *   训练收尾顺序完全一致（frames_load → stabilize → emb → 槽类 → idf）。
     *   它们都是「从最终的图现算」：早算一步就等于算在旧的图上，
     *   而存盘存的是最终的图 —— 读盘重算后两边就不一样了。
     *   ① emb 早算的症状最隐蔽：词池逐字节相同，但 810 个符号节点的 emb 不同；
     *   ② idf 早算还会越界（数组按当时的 pool->count 分配，之后又造了词）。 */
    /* §6.214 **反查倒排表**：一次 O(边数) 重建（只进内存、不进快照）。
     * 表是"只追加 + 读时校验"的 —— 载入后用图本身重建，陈旧项由查询侧的
     * `rel_direct*` 过滤，所以这里不需要跟写路径对齐。 */
    {
        TokenPool *rp = ai->pool;
        if (rp) {
            for (size_t ti = 0; ti < rp->count; ++ti) {
                Token *t = rp->tokens[ti];
                if (!t || t->is_garbage) continue;
                for (size_t ei = 0; ei < t->count; ++ei) {
                    const AssocEntry *e = &token_assocs(t)[ei];
                    if (!e->main) continue;
                    for (uint8_t r = 1; r < REL_DYN_BASE; ++r) {
                        uint16_t bit = REL_BIT(r);
                        if (e->rel & bit)  pool_rev_add(rp, e->main, t->id, r, 0);
                        if (e->nrel & bit) pool_rev_add(rp, e->main, t->id, r, 1);
                    }
                    if (e->dyn)
                        pool_rev_add(rp, e->main, t->id,
                                     (uint32_t)(REL_DYN_BASE + e->dyn - 1),
                                     edge_dyn_neg(e) ? 1 : 0);
                }
            }
        }
    }
    /* §6.584 老数据里的"死的自环边"（a→a、无角色/关系/命中）**读盘时清掉**：
     * 留着的话，用户问 `A R A` 会让强化/答复路径把它 touch 成"带语义的自环"，
     * 之后不变式就抓不干净了。清完"图里没有自环"是硬不变式（`aicheck.h`）。
     * 放在 `ai_embed_build` **之前**：那些死边 hits=0，本来就不进 emb，顺序只为让语义清楚。 */
    { size_t dead = pool_drop_dead_self_loops(ai->pool);
      if (dead && getenv("AIP_PATDBG")) fprintf(stderr, "[load] 清掉死的自环边 %zu 条\n", dead); }
    ai_embed_build(ai);       /* 分布式表示（PPMI 随机索引）也现算，不进快照 */
    ai_slots_build(ai);       /* 同位槽类同理 */
    ai_idf_rebuild(ai);       /* IDF 从整句库现算，不进快照 */

    if (r.err) goto fail_ai;

#ifdef MODEL_IO_INVARIANTS
    for (size_t i = 0; i < np->count; ++i) {
        const Token *t = np->tokens[i];
        if (t->name && !strcmp(t->name, AI_DIALOG_SPLIT_NAME) && ai->split_tok != t) {
            fprintf(stderr, "INV: split_tok 没还原到 %s\n", AI_DIALOG_SPLIT_NAME);
            goto fail_ai;
        }
        if ((t->count > 0 && !t->assocs) || t->count > t->cap) {
            fprintf(stderr, "INV: i=%zu '%s' count=%zu cap=%zu assocs=%p\n",
                    i, t->name ? t->name : "(null)", t->count, t->cap, (void *)t->assocs);
            goto fail_ai;
        }
    }
#endif

    /* energy 是每轮由 ai_effective_energy 算出来的量，存盘时存的是上一轮的残留值
     * （agent_step 每轮开头重置成 1.0）。照 ai_new() 的口径还原，免得加载后
     * 「差一档」。n_candidates 不在这里重算 —— 它是基准值，ai_exploration_bias
     * 每轮在它基础上乘 bias；拿偏置过的值当基准会再乘一次 bias（实测 3→4）。 */
    ai->energy = 1.0f;

    /* 学出来的可信度：证据从图里重新数一遍（计数器不进存档，见 ai_recount_evidence） */
    ai_recount_evidence(ai);

    /* 未解问题**不进格式**：它们靠随图存盘的 `[自问:…]` 标记恢复（见 think.h）。
     * 放在最后：rel_parse 需要关系表与词池都已经就位。 */
    (void)think_restore_pending(ai);

    np->loading = 0;                      /* §6.111：重建结束，之后的 touch 才算观测 */
    /* §6.248 读盘路径**不经过 ai_new()** ⇒ 环境变量开关要在这里再读一次
     * （不读的话 `AIP_VERIFYWRITES=1` 对开场就加载的模型完全无效 —— 实测踩过）。 */
    { const char *vw = getenv("AIP_VERIFYWRITES"); ai->verify_writes = (vw && vw[0] == '1') ? 1 : 0; }
    /* §6.574 读盘把每条边都"写"了一遍（走的是 add_assoc 那条路）⇒ 全被标脏了。
     * 但"脏"的语义是**相对 base 改过**，刚读进来时一份都没改 ⇒ 这里统一清掉。
     * 不清的话 `::parts journal` 会把整个 base 抄一遍（实测 278 条而不是 1 条）。 */
    for (size_t i = 0; i < np->count; ++i) {
        Token *t = np->tokens[i];
        if (t) t->edges_dirty = 0;
    }
    free(buf);
    return ai;

fail_ai:
    if (getenv("AIP_LOADDBG")) fprintf(stderr, "[load] fail_ai @%d pos=%zu len=%zu\n", __LINE__, r.pos, r.len);
    free(buf);
    ai_free(ai);        /* ai->pool 此时已是 np（或仍是新建的池），只释放一次 */
    np = NULL;
    return NULL;
fail:
    if (getenv("AIP_LOADDBG")) fprintf(stderr, "[load] fail @%d pos=%zu len=%zu\n", __LINE__, r.pos, r.len);
    free(buf);
    if (np) pool_free(np);
    ai_free(ai);
    return NULL;
}

#endif /* MODEL_IO_H */
