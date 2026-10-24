/* parts.h — **边的段文件 + 按需加载**（多智能体 / 局部加载的第二刀）
 *
 * 用户 2026-10-24 的目标：
 *   「…并且可以暂时只加载一部分图，至于怎么实现，就是指针不是 8 字节对齐吗，
 *     不是 0 就是在磁盘或其他地方」
 *
 * 这一版把这句话落成可运行的三件事：
 *
 *   ① `aip_parts_save(ai, path)`   —— 把**每条词的边**写成一个**段文件**（`*.parts`）：
 *        头 + 索引（词 id → 偏移/条数）+ 一堆定长记录。图的其余部分还在 model.bin 里。
 *   ② `aip_parts_detach(ai)`       —— 把内存里的边数组**扔给"磁盘"**：free 掉数组，
 *        `t->assocs`（经存储层 `token_assocs_set_disk`）改成一个**带 AIP_PTR_DISK 标签的哨兵指针**。
 *        `count` 保留 —— 所以"有几条边"仍然知道，只是**内容不在内存里**。
 *   ③ `token_assocs()` 解析标签    —— 谁要读边，`token.h` 的访问器就调 `pool->part_load`
 *        读回这一段、挂上、清标签。**读过的词才算"加载了"** —— 这就是"只加载一部分"。
 *
 * 三条纪律：
 *   · 带标签的指针**不许 free、不许 realloc、不许直接解引用**；
 *   · 只读视图 `token_assocs_ro` 也会解析标签（**宁可多读一次，也不许静默少几条边**）；
 *   · 计数器（`n_loads` / `n_bytes`）是这套东西的**验收数字**：没有它，等于"看起来跑了"。
 */
#ifndef AIP_PARTS_H
#define AIP_PARTS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ⚠ 本头文件假定调用方**已经** include 了 ai.h（它要用 AI 的 pool 字段）；
 * 只 include 它是编译不过的 —— 这是有意的：它属于"应用层"，不是图的基础层。 */
#include "token.h"
#include "ptrtag.h"

#define AIP_PARTS_MAGIC   0x53545250u   /* 'PRTS' */
#define AIP_PARTS_VERSION 2u   /* v2：索引的空闲字节用来存 flags（bit0 = has_causal） */
#define AIP_PART_IDX_LEN 20u     /* 一条索引：id(4) + off(8) + cnt(4) + flags(2) + pad(2) */

/* 定长边记录（逐字段写，别 memcpy 结构体：那会把 padding 也写进文件 ⇒ 存档不确定） */
typedef struct {
    uint32_t b_id, a_id;        /* 邻居 / 动作槽（0 = 无） */
    uint32_t hits, seq;
    uint64_t birth, last;
    uint32_t tset[4];
    uint16_t rel, nrel;
    uint8_t  n_tset, mem_state, role, topic, pattern, dyn;
} AIPartEdge;

typedef struct AIParts_tag {
    FILE    *f;
    char     path[256];
    uint64_t *off;              /* 按词 id 索引：段在文件里的偏移（0 = 这个词没有边） */
    uint32_t *cnt;              /* 按词 id 索引：段里几条边 */
    uint8_t  *flags;            /* 按词 id 索引：段头的 flags（bit0 = 有因果边）*/
    TokenPool *pool;            /* attach 时挂上：打开索引时顺手把 flags 落到词上 */
    struct AIParts_tag *overlay;    /* §6.573 覆盖层（delta 文件）：同一个词的段以它为准 */
    size_t    n_ids;
    size_t    n_loads;          /* 触发了几次"从磁盘读回来"（验收数字） */
    size_t    n_bytes;          /* 一共读了多少字节（验收数字） */
    size_t    n_resident;       /* 现在内存里有多少个词的边（验收数字） */
    size_t    n_evicted;        /* 被换出过几次（验收数字） */
    uint32_t *loaded_cnt;       /* 按 id 记"读回来时是几条" —— 用来判断这一份有没有被改脏 */
} AIParts;

/* ---------------- 写：把每个词的边写成一个段 ---------------- */

static inline int aip_parts_save(AI *ai, const char *path) {
    if (!ai || !ai->pool || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    TokenPool *p = ai->pool;
    /* 头：magic / version / 词数 / 有边的词数 */
    uint32_t head[4] = { AIP_PARTS_MAGIC, AIP_PARTS_VERSION, (uint32_t)p->count, 0u };
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t && t->count > 0) head[3]++;
    }
    if (fwrite(head, sizeof head, 1, f) != 1) { fclose(f); return -1; }

    /* 索引先占位（每条 16 字节：id / off / cnt / 保留），写完段再回填 */
    uint64_t index_off = (uint64_t)ftell(f);
    uint8_t zero[AIP_PART_IDX_LEN] = {0};
    for (uint32_t i = 0; i < head[3]; ++i)
        if (fwrite(zero, sizeof zero, 1, f) != 1) { fclose(f); return -1; }

    uint32_t n_written = 0;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t || t->count == 0) continue;
        /* ⚠ 保存前也要解析标签：被 detach 过的图，边在段文件里 —— 先读回来再写 */
        token_assocs_resolve(t);
        uint32_t cnt = (uint32_t)t->count;
        uint64_t seg_off = (uint64_t)ftell(f);
        const AssocEntry *as = token_assocs_ro(t);
        for (uint32_t k = 0; k < cnt; ++k) {
            AIPartEdge pe;
            memset(&pe, 0, sizeof pe);
            pe.b_id = as[k].main ? as[k].main->id : 0u;
            pe.a_id = as[k].action ? as[k].action->id : 0u;
            pe.hits = as[k].hits; pe.seq = as[k].seq;
            pe.birth = as[k].birth_tick; pe.last = as[k].last_touch_tick;
            memcpy(pe.tset, as[k].tset, sizeof pe.tset);
            pe.rel = as[k].rel; pe.nrel = as[k].nrel;
            pe.n_tset = as[k].n_tset; pe.mem_state = as[k].mem_state;
            pe.role = as[k].role; pe.topic = as[k].topic;
            pe.pattern = as[k].pattern; pe.dyn = as[k].dyn;
            if (fwrite(&pe, sizeof pe, 1, f) != 1) { fclose(f); return -1; }
        }
        /* 回填索引 */
        long save = ftell(f);
        if (fseek(f, (long)(index_off + (uint64_t)n_written * AIP_PART_IDX_LEN), SEEK_SET) != 0) { fclose(f); return -1; }
        uint32_t id = t->id;
        uint16_t flags = (uint16_t)(t->has_causal ? 1u : 0u);
        uint16_t pad2 = 0;
        if (fwrite(&id, 4, 1, f) != 1 || fwrite(&seg_off, 8, 1, f) != 1 ||
            fwrite(&cnt, 4, 1, f) != 1 || fwrite(&flags, 2, 1, f) != 1 ||
            fwrite(&pad2, 2, 1, f) != 1) { fclose(f); return -1; }
        if (fseek(f, save, SEEK_SET) != 0) { fclose(f); return -1; }
        n_written++;
    }
    fclose(f);
    return (int)n_written;
}

/* ---------------- 写 delta：只写"这个脑改过"的段（§6.573） ---------------- */

static inline int aip_parts_save_delta(AI *ai, const char *path) {
    if (!ai || !ai->pool || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    TokenPool *p = ai->pool;
    uint32_t head[4] = { AIP_PARTS_MAGIC, AIP_PARTS_VERSION, (uint32_t)p->count, 0u };
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t && t->count > 0 && t->edges_dirty) head[3]++;
    }
    if (fwrite(head, sizeof head, 1, f) != 1) { fclose(f); return -1; }
    uint64_t index_off = (uint64_t)ftell(f);
    uint8_t zero[AIP_PART_IDX_LEN] = {0};
    for (uint32_t i = 0; i < head[3]; ++i)
        if (fwrite(zero, sizeof zero, 1, f) != 1) { fclose(f); return -1; }
    uint32_t n_written = 0;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t || t->count == 0 || !t->edges_dirty) continue;
        token_assocs_resolve(t);
        uint32_t cnt = (uint32_t)t->count;
        uint64_t seg_off = (uint64_t)ftell(f);
        const AssocEntry *as = token_assocs_ro(t);
        for (uint32_t k = 0; k < cnt; ++k) {
            AIPartEdge pe;
            memset(&pe, 0, sizeof pe);
            pe.b_id = as[k].main ? as[k].main->id : 0u;
            pe.a_id = as[k].action ? as[k].action->id : 0u;
            pe.hits = as[k].hits; pe.seq = as[k].seq;
            pe.birth = as[k].birth_tick; pe.last = as[k].last_touch_tick;
            memcpy(pe.tset, as[k].tset, sizeof pe.tset);
            pe.rel = as[k].rel; pe.nrel = as[k].nrel;
            pe.n_tset = as[k].n_tset; pe.mem_state = as[k].mem_state;
            pe.role = as[k].role; pe.topic = as[k].topic;
            pe.pattern = as[k].pattern; pe.dyn = as[k].dyn;
            if (fwrite(&pe, sizeof pe, 1, f) != 1) { fclose(f); return -1; }
        }
        long save = ftell(f);
        if (fseek(f, (long)(index_off + (uint64_t)n_written * AIP_PART_IDX_LEN), SEEK_SET) != 0) { fclose(f); return -1; }
        uint32_t id = t->id;
        uint16_t flags = (uint16_t)(t->has_causal ? 1u : 0u), pad2 = 0;
        if (fwrite(&id, 4, 1, f) != 1 || fwrite(&seg_off, 8, 1, f) != 1 ||
            fwrite(&cnt, 4, 1, f) != 1 || fwrite(&flags, 2, 1, f) != 1 ||
            fwrite(&pad2, 2, 1, f) != 1) { fclose(f); return -1; }
        if (fseek(f, save, SEEK_SET) != 0) { fclose(f); return -1; }
        t->edges_dirty = 0;            /* 已经落进 delta ⇒ 干净了 */
        n_written++;
    }
    fclose(f);
    return (int)n_written;
}

/* ---------------- 打开：只读**索引**（不读任何一段） ---------------- */

static inline AIParts *aip_parts_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t head[4];
    if (fread(head, sizeof head, 1, f) != 1 || head[0] != AIP_PARTS_MAGIC ||
        head[1] != AIP_PARTS_VERSION) { fclose(f); return NULL; }
    AIParts *pt = (AIParts *)calloc(1, sizeof *pt);
    if (!pt) { fclose(f); return NULL; }
    pt->f = f;
    snprintf(pt->path, sizeof pt->path, "%s", path);
    pt->n_ids = 65536;   /* 词 id 是线性小整数；给个够用的表 */
    pt->off = (uint64_t *)calloc(pt->n_ids, sizeof *pt->off);
    pt->cnt = (uint32_t *)calloc(pt->n_ids, sizeof *pt->cnt);
    pt->flags = (uint8_t *)calloc(pt->n_ids, sizeof *pt->flags);
    pt->loaded_cnt = (uint32_t *)calloc(pt->n_ids, sizeof *pt->loaded_cnt);
    if (!pt->off || !pt->cnt || !pt->flags || !pt->loaded_cnt) {
        fclose(f); free(pt->off); free(pt->cnt); free(pt->flags); free(pt->loaded_cnt); free(pt); return NULL;
    }
    uint32_t nseg = head[3];
    fseek(f, 16, SEEK_SET);
    TokenPool *pool = pt->pool;   /* 可能为 NULL（open 时还没 attach）*/
    for (uint32_t i = 0; i < nseg; ++i) {
        uint32_t id, cnt; uint64_t off; uint16_t flags = 0, pad2 = 0;
        if (fread(&id, 4, 1, f) != 1 || fread(&off, 8, 1, f) != 1 ||
            fread(&cnt, 4, 1, f) != 1 || fread(&flags, 2, 1, f) != 1 ||
            fread(&pad2, 2, 1, f) != 1) break;
        if (id < pt->n_ids) {
            pt->off[id] = off; pt->cnt[id] = cnt;
            if (flags & 1u) pt->flags[id] |= 1u;
            if (pool) {   /* 已经有这个词 ⇒ 直接把"有因果边"标上 */
                Token *t = pool_token_by_id(pool, id);
                if (t && (flags & 1u)) t->has_causal = 1;
            }
        }
    }
    return pt;
}

static inline void aip_parts_close(AIParts *pt) {
    if (!pt) return;
    if (pt->f) fclose(pt->f);
    free(pt->off); free(pt->cnt); free(pt->flags); free(pt->loaded_cnt); free(pt);
}

/* ---------------- 读：把**这一段**读回来，挂到词上 ---------------- */

static inline void aip_parts_load_token(void *ctx, Token *t) {
    AIParts *pt = (AIParts *)ctx;
    if (!pt || !t) return;
    /* §6.573 覆盖层优先（delta 文件里的段是"这个脑自己改过的"） */
    if (pt->overlay && t->id < pt->overlay->n_ids && pt->overlay->off[t->id] != 0)
        pt = pt->overlay;
    if (t->id >= pt->n_ids || pt->off[t->id] == 0) {
        /* 没有段 ⇒ 把它变成"内存里的空边表"。**绝不留** tag + count>0：
         * 那会让调用方拿到 NULL 数组却按 count 去遍历（实测段错误）。 */
        token_assocs_set_ram(t, NULL, 0);
        return;
    }
    uint32_t cnt = pt->cnt[t->id];
    AssocEntry *as = NULL;
    if (cnt) {
        as = (AssocEntry *)calloc(cnt, sizeof *as);
        if (!as) return;
        if (fseek(pt->f, (long)pt->off[t->id], SEEK_SET) != 0) { free(as); return; }
        for (uint32_t k = 0; k < cnt; ++k) {
            AIPartEdge pe;
            if (fread(&pe, sizeof pe, 1, pt->f) != 1) { free(as); return; }
            as[k].main = pe.b_id ? pool_token_by_id(t->owner, pe.b_id) : NULL;
            as[k].action = pe.a_id ? pool_token_by_id(t->owner, pe.a_id) : NULL;
            as[k].hits = pe.hits; as[k].seq = pe.seq;
            as[k].birth_tick = pe.birth; as[k].last_touch_tick = pe.last;
            memcpy(as[k].tset, pe.tset, sizeof as[k].tset);
            as[k].rel = pe.rel; as[k].nrel = pe.nrel;
            as[k].n_tset = pe.n_tset; as[k].mem_state = pe.mem_state;
            as[k].role = pe.role; as[k].topic = pe.topic;
            as[k].pattern = pe.pattern; as[k].dyn = pe.dyn;
        }
        pt->n_bytes += (size_t)cnt * sizeof(AIPartEdge);
    }
    token_assocs_set_ram(t, as, cnt);   /* 存储层的活：挂回真数组、清标签、定容量 */
    pt->loaded_cnt[t->id] = cnt;        /* 记住"读回来时是几条"：判脏（§6.566 的换出用） */
    pt->n_loads++;
    pt->n_resident++;                   /* 内存里多了一段（验收数字） */
    if (getenv("AIP_PARTLOG"))          /* §6.570 取证：谁被读回来了（默认关，写文件不污染 stdout） */
        fprintf(stderr, "[partload] %s (%u 条)\n", (t->name ? t->name : "?"), cnt);
}

/* ---------------- 覆盖层（delta）：base + 自己的改动 ---------------- */

static inline int aip_parts_set_overlay(AIParts *base, const char *delta_path) {
    if (!base || !delta_path) return -1;
    AIParts *d = aip_parts_open(delta_path);
    if (!d) return -1;
    if (base->overlay && base->overlay != d) aip_parts_close(base->overlay);
    base->overlay = d;
    return 0;
}

/* ---------------- 挂上 / 卸下 ---------------- */

/* 装上"按需加载"的钩子：之后 `token_assocs()` 碰到 AIP_PTR_DISK 就会读回来。 */
static inline void aip_parts_attach(AI *ai, AIParts *pt) {
    if (!ai || !ai->pool || !pt) return;
    pt->pool = ai->pool;
    /* 索引里记着"哪些词有因果边" ⇒ 打开时就落到词上，省得为判断"有没有"去读边（§6.570） */
    for (size_t i = 0; i < pt->n_ids; ++i) {
        if (!(pt->flags[i] & 1u)) continue;
        Token *t = pool_token_by_id(ai->pool, (uint32_t)i);
        if (t) t->has_causal = 1;
    }
    ai->pool->parts = pt;
    ai->pool->part_load = aip_parts_load_token;   /* (ctx=parts, t) 形状，与 token.h 的声明一致 */
}

/* 把边数组**扔给磁盘**：free 掉，`assocs` 变成带 DISK 标签的哨兵，`count` 留着。
 * 返回被卸下的词数（有边的那些）。 */
static inline size_t aip_parts_detach(AI *ai) {
    if (!ai || !ai->pool) return 0;
    AIParts *pt = (AIParts *)ai->pool->parts;
    size_t n = 0, kept = 0;
    TokenPool *p = ai->pool;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t || t->count == 0) continue;
        /* §6.570 **只有磁盘上有它的段，才允许放手**：save 之后新学的边没有段，
         * 硬卸下会把它们默默丢掉（实测：卸下后一句话触发的因果学习就崩在这里）。 */
        if (!pt || t->id >= pt->n_ids || pt->off[t->id] == 0) { kept++; continue; }
        token_assocs_resolve(t);            /* 已经读回来过就先用着 */
        token_assocs_set_disk(t);           /* 存储层的活：free + 打 DISK 标签（§6.566） */
        n++;
    }
    if (kept && getenv("AIP_PARTLOG"))
        fprintf(stderr, "[partdetach] 留下 %zu 个词（磁盘上没有它们的段，卸下会丢数据）\n", kept);
    return n;
}

/* ---------------- 统计（验收数字；没有它等于"看起来跑了"） ---------------- */

static inline void aip_parts_stats(const AIParts *pt, size_t *loads, size_t *bytes,
                                   size_t *resident, size_t *evicted) {
    if (loads) *loads = pt ? pt->n_loads : 0;
    if (bytes) *bytes = pt ? pt->n_bytes : 0;
    if (resident) *resident = pt ? pt->n_resident : 0;
    if (evicted) *evicted = pt ? pt->n_evicted : 0;
}

/* ---------------- 换出：内存上限到了就把**干净**的段扔回磁盘 ----------------
 *
 * 这是"暂时只加载一部分"里的**暂时**：
 *   · 只换出**干净**的（`count` 与读回来时一致）—— 脏的（新加了边/改过）不换，返回 skipped，
 *     否则一换就把没落盘的新知识丢了；
 *   · 换出 = `token_assocs_set_disk`（free + 打 DISK 标签），字段回到"下次读"的状态；
 *   · `keep` 是"想留多少个词的边在内存里"（0 = 全换出）。
 * ⚠ 这是**启发式**判脏（只看条数）：改了已有边的内容但没加条数，判不出来 —— 照实写在文档里。
 */
static inline size_t aip_parts_evict(AI *ai, size_t keep, size_t *skipped) {
    if (skipped) *skipped = 0;
    if (!ai || !ai->pool) return 0;
    AIParts *pt = (AIParts *)ai->pool->parts;
    if (!pt) return 0;
    size_t evicted = 0, skip = 0, resident = 0;
    /* 先数一遍内存里还有多少段的边 */
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->count == 0) continue;
        if (token_assocs_resident(t)) resident++;
    }
    for (size_t i = 0; i < ai->pool->count && resident > keep; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->count == 0) continue;
        if (!token_assocs_resident(t)) continue;
        if (t->id >= pt->n_ids || pt->off[t->id] == 0) { skip++; continue; }   /* 磁盘上没有它的段 */
        if (pt->loaded_cnt[t->id] == 0 || t->count != pt->loaded_cnt[t->id]) { skip++; continue; }  /* 脏 */
        token_assocs_set_disk(t);
        evicted++; resident--;
        pt->n_evicted++; pt->n_resident--;
    }
    if (skipped) *skipped = skip;
    return evicted;
}

#endif /* AIP_PARTS_H */
