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
#define AIP_PARTS_VERSION 1u

/* 定长边记录（逐字段写，别 memcpy 结构体：那会把 padding 也写进文件 ⇒ 存档不确定） */
typedef struct {
    uint32_t b_id, a_id;        /* 邻居 / 动作槽（0 = 无） */
    uint32_t hits, seq;
    uint64_t birth, last;
    uint32_t tset[4];
    uint16_t rel, nrel;
    uint8_t  n_tset, mem_state, role, topic, pattern, dyn;
} AIPartEdge;

typedef struct {
    FILE    *f;
    char     path[256];
    uint64_t *off;              /* 按词 id 索引：段在文件里的偏移（0 = 这个词没有边） */
    uint32_t *cnt;              /* 按词 id 索引：段里几条边 */
    size_t    n_ids;
    size_t    n_loads;          /* 触发了几次"从磁盘读回来"（验收数字） */
    size_t    n_bytes;          /* 一共读了多少字节（验收数字） */
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
    uint8_t zero[16] = {0};
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
        if (fseek(f, (long)(index_off + (uint64_t)n_written * 16u), SEEK_SET) != 0) { fclose(f); return -1; }
        uint32_t id = t->id;
        uint32_t pad = 0;
        if (fwrite(&id, 4, 1, f) != 1 || fwrite(&seg_off, 8, 1, f) != 1 ||
            fwrite(&cnt, 4, 1, f) != 1) { fclose(f); return -1; }
        (void)pad;
        if (fseek(f, save, SEEK_SET) != 0) { fclose(f); return -1; }
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
    if (!pt->off || !pt->cnt) { fclose(f); free(pt->off); free(pt->cnt); free(pt); return NULL; }
    uint32_t nseg = head[3];
    fseek(f, 16, SEEK_SET);
    for (uint32_t i = 0; i < nseg; ++i) {
        uint32_t id, cnt; uint64_t off;
        if (fread(&id, 4, 1, f) != 1 || fread(&off, 8, 1, f) != 1 || fread(&cnt, 4, 1, f) != 1) break;
        if (id < pt->n_ids) { pt->off[id] = off; pt->cnt[id] = cnt; }
    }
    return pt;
}

static inline void aip_parts_close(AIParts *pt) {
    if (!pt) return;
    if (pt->f) fclose(pt->f);
    free(pt->off); free(pt->cnt); free(pt);
}

/* ---------------- 读：把**这一段**读回来，挂到词上 ---------------- */

static inline void aip_parts_load_token(void *ctx, Token *t) {
    AIParts *pt = (AIParts *)ctx;
    if (!pt || !t) return;
    if (t->id >= pt->n_ids || pt->off[t->id] == 0) return;
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
    pt->n_loads++;
}

/* ---------------- 挂上 / 卸下 ---------------- */

/* 装上"按需加载"的钩子：之后 `token_assocs()` 碰到 AIP_PTR_DISK 就会读回来。 */
static inline void aip_parts_attach(AI *ai, AIParts *pt) {
    if (!ai || !ai->pool || !pt) return;
    ai->pool->parts = pt;
    ai->pool->part_load = aip_parts_load_token;   /* (ctx=parts, t) 形状，与 token.h 的声明一致 */
}

/* 把边数组**扔给磁盘**：free 掉，`assocs` 变成带 DISK 标签的哨兵，`count` 留着。
 * 返回被卸下的词数（有边的那些）。 */
static inline size_t aip_parts_detach(AI *ai) {
    if (!ai || !ai->pool) return 0;
    size_t n = 0;
    TokenPool *p = ai->pool;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t || t->count == 0) continue;
        token_assocs_resolve(t);            /* 已经读回来过就先用着 */
        token_assocs_set_disk(t);           /* 存储层的活：free + 打 DISK 标签（§6.566） */
        n++;
    }
    return n;
}

#endif /* AIP_PARTS_H */
