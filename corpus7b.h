/* corpus7b.h — 读 `.c7b` 分片语料（阶段四扩图 §6.711）
 *
 * 为什么单有一个读法：7B 边不可能整图进内存（392 GB）。语料按片落盘（每片 2000 万边 / 166 MB），
 * **只读头 + 索引**就能 attach（索引 400 万字节量级），要某个词的边才去读那一段（几十字节 ~ 16 KB）。
 * 这就是 0.5B（§6.706）那条"局部加载"路线在 8 B/边 的新格式上的版本。
 *
 * 只做三件事（没有第四件）：
 *   ① `c7b_open`  读头（magic/版本/词数/边数/段起点/种子），**不读任何一段**；
 *   ② `c7b_find`  按词 id 在索引里二分（索引按 id 升序），拿段偏移 + 条数；
 *   ③ `c7b_read`  读那一段（8 B/条：邻居 4 + 关系 2 + 动态槽 1 + 保留 1）。
 *
 * 只读；不改文件、不写图。损坏的文件一律返回 NULL + 原因（不猜）。
 */
#ifndef AIP_CORPUS7B_H
#define AIP_CORPUS7B_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define C7B_HDR 48u      /* 头长度：magic(4) + ver(4) + n_words(4) + n_seg(4) + n_edges(8) + payload(8) + seed(8) + rel_tail(4) + rsv(4) */
#define C7B_IDX 16u      /* 索引一条：id(4) + off(8) + cnt(4) */
#define C7B_REC 8u       /* 边一条：b_id(4) + rel(2) + dyn(1) + pad(1) */

typedef struct {
    uint32_t b_id;
    uint16_t rel;
    uint8_t  dyn;
} C7BEdge;

typedef struct {
    FILE    *f;
    uint32_t n_words;
    uint32_t n_seg;
    uint64_t n_edges;
    uint64_t payload;
    uint64_t seed;
    uint64_t size;
    char     path[256];
    /* 索引读进内存（n_seg × 16 ≈ 6.4 MB）：挂上以后每次查词只做一次 RAM 二分 */
    uint32_t *iid;
    uint64_t *ioff;
    uint32_t *icnt;
    void     *pool;          /* attach 过的话：TokenPool*（换脑要重新 attach） */
    void    **map;           /* 语料内的词 id → **这个池子里的** Token*（按名字 intern 出来的）
                              * ⚠ 不能直接用 pool_token_by_id(b_id)：池子里原有的词会占掉前面的 id
                              *   ⇒ 语料第 5 个词会被解析成出厂模型第 5 个词（实测：`::rel` 打出一堆中文词）*/
    size_t   n_lookups;      /* 验收数字：查了几次 */
    size_t   n_reads;        /* 验收数字：读了几段 */
    size_t   n_bytes;        /* 验收数字：读回来多少字节 */
} C7B;

static inline const char *c7b_rel_name(uint16_t rel) {
    switch (rel) {
    case 1:  return "属于";
    case 2:  return "有";
    case 3:  return "会";
    case 4:  return "怕";
    case 5:  return "喜欢";
    case 6:  return "爱";
    case 7:  return "在";
    case 11: return "合成关系";
    default: return "动态关系";
    }
}

static inline void c7b_close(C7B *c) {
    if (!c) return;
    if (c->f) fclose(c->f);
    free(c->iid); free(c->ioff); free(c->icnt); free(c->map);
    free(c);
}

/* 打开 = 只读头（+ 记下文件长度，用来核对"头+索引+边×8"）
 * 失败时把原因写进 err（有界）。 */
static inline C7B *c7b_open(const char *path, char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { if (err && cap) snprintf(err, cap, "读不开 %s", path ? path : "(空)"); return NULL; }
    char m[4];
    uint32_t ver = 0, n_words = 0, n_seg = 0, rel_tail = 0, rsv = 0;
    uint64_t n_edges = 0, payload = 0, seed = 0;
    if (fread(m, 1, 4, f) != 4 || m[0] != 'C' || m[1] != '7' || m[2] != 'B' || m[3] != '1' ||
        fread(&ver, 4, 1, f) != 1 || fread(&n_words, 4, 1, f) != 1 || fread(&n_seg, 4, 1, f) != 1 ||
        fread(&n_edges, 8, 1, f) != 1 || fread(&payload, 8, 1, f) != 1 || fread(&seed, 8, 1, f) != 1 ||
        fread(&rel_tail, 4, 1, f) != 1 || fread(&rsv, 4, 1, f) != 1) {
        if (err && cap) snprintf(err, cap, "不是 C7B 文件（头读不全）");
        fclose(f);
        return NULL;
    }
    (void)rel_tail; (void)rsv;
    if (ver != 1 || (uint64_t)ftell(f) != C7B_HDR) {
        if (err && cap) snprintf(err, cap, "版本不认识（version=%u）", ver);
        fclose(f);
        return NULL;
    }
    if (payload != (uint64_t)C7B_HDR + (uint64_t)n_seg * C7B_IDX) {
        if (err && cap) snprintf(err, cap, "段起点不对（头里 %llu，按索引应为 %llu）",
                                 (unsigned long long)payload,
                                 (unsigned long long)(C7B_HDR + (uint64_t)n_seg * C7B_IDX));
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { if (err && cap) snprintf(err, cap, "seek 失败"); fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (uint64_t)sz != payload + n_edges * C7B_REC) {
        if (err && cap) snprintf(err, cap, "长度不对（%ld 字节，按头应为 %llu）", sz,
                                 (unsigned long long)(payload + n_edges * C7B_REC));
        fclose(f);
        return NULL;
    }
    C7B *c = (C7B *)calloc(1, sizeof *c);
    if (!c) { if (err && cap) snprintf(err, cap, "内存不够"); fclose(f); return NULL; }
    c->f = f; c->n_words = n_words; c->n_seg = n_seg; c->n_edges = n_edges;
    c->payload = payload; c->seed = seed; c->size = (uint64_t)sz;
    snprintf(c->path, sizeof c->path, "%s", path);
    return c;
}

/* 按词 id 找段（索引按 id 升序 ⇒ 二分；每次只读 16 字节） */
static inline int c7b_find(C7B *c, uint32_t word_id, uint64_t *off, uint32_t *cnt) {
    if (!c) return -1;
    c->n_lookups++;
    uint32_t lo = 0, hi = c->n_seg;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint8_t rec[C7B_IDX];
        if (fseek(c->f, (long)(C7B_HDR + (uint64_t)mid * C7B_IDX), SEEK_SET) != 0) return -2;
        if (fread(rec, C7B_IDX, 1, c->f) != 1) return -2;
        uint32_t rid, rc; uint64_t ro;
        memcpy(&rid, rec, 4);
        memcpy(&ro, rec + 4, 8);
        memcpy(&rc, rec + 12, 4);
        if (rid == word_id) {
            c->n_bytes += C7B_IDX;
            if (off) *off = ro;
            if (cnt) *cnt = rc;
            return 0;
        }
        if (rid < word_id) lo = mid + 1; else hi = mid;
    }
    if (off) *off = 0;
    if (cnt) *cnt = 0;
    return 1;                    /* 1 = 这个词在这个分片里没有边 */
}

/* 读一段：最多 cap 条，返回读到的条数（< 0 出错） */
static inline int c7b_read(C7B *c, uint64_t off, uint32_t cnt, C7BEdge *out, uint32_t cap) {
    if (!c || !out || cap == 0 || cnt == 0) return 0;
    uint32_t take = cnt < cap ? cnt : cap;
    if (fseek(c->f, (long)off, SEEK_SET) != 0) return -1;
    size_t got = fread(out, C7B_REC, take, c->f);
    c->n_reads++;
    c->n_bytes += got * C7B_REC;
    return (int)got;
}


/* ---------------- 挂到脑上：`.c7b` 的边走 token_assocs 的 DISK 通道 -----------------
 * 这是"7B 能用"的关键一步：挂上以后 `::rel`、求解器、`::contradict` 都**看不到差别**
 * —— 它们是经 `token_assocs()` 读边的，而这里只是把"段在哪/几条"记在词上，
 * 第一次有人要读时才去读那一段（和 parts.h 的 attach 同一个机制）。
 *
 * ⚠ 只读语料：这里只**读**文件；写入路径不会被触发（要改知识请用 `::teach`，别改语料）。
 * ⚠ 本头文件假定调用方已经 include 了 ai.h/token.h（同 parts.h 的约定）。 */

static inline int c7b_load_index(C7B *c) {
    if (!c || !c->f) return -1;
    if (c->iid) return 0;
    if (c->n_seg == 0) return -1;
    c->iid  = (uint32_t *)malloc((size_t)c->n_seg * sizeof(uint32_t));
    c->ioff = (uint64_t *)malloc((size_t)c->n_seg * sizeof(uint64_t));
    c->icnt = (uint32_t *)malloc((size_t)c->n_seg * sizeof(uint32_t));
    if (!c->iid || !c->ioff || !c->icnt) { free(c->iid); free(c->ioff); free(c->icnt);
                                          c->iid = NULL; c->ioff = NULL; c->icnt = NULL; return -1; }
    if (fseek(c->f, (long)C7B_HDR, SEEK_SET) != 0) return -1;
    for (uint32_t k = 0; k < c->n_seg; ++k) {
        uint8_t rec[C7B_IDX];
        if (fread(rec, C7B_IDX, 1, c->f) != 1) { c->n_seg = k; break; }
        memcpy(&c->iid[k], rec, 4);
        memcpy(&c->ioff[k], rec + 4, 8);
        memcpy(&c->icnt[k], rec + 12, 4);
    }
    c->n_bytes += (size_t)c->n_seg * C7B_IDX;
    return 0;
}

/* 读一段 → 真数组（AssocEntry），用的是 parts.h 同一套"存储层"接口 */
static inline void c7b_load_token(void *ctx, Token *t) {
    C7B *c = (C7B *)ctx;
    if (!c || !t) return;
    uint64_t off = 0; uint32_t cnt = 0;
    if (c7b_find(c, t->id, &off, &cnt) != 0 || cnt == 0) {
        token_assocs_set_ram(t, NULL, 0);       /* 没段 ⇒ 空表；绝不留"标签 + count>0" */
        return;
    }
    AssocEntry *as = (AssocEntry *)calloc(cnt, sizeof *as);
    if (!as) { token_assocs_set_ram(t, NULL, 0); return; }
    if (fseek(c->f, (long)off, SEEK_SET) != 0) { free(as); token_assocs_set_ram(t, NULL, 0); return; }
    uint32_t got = 0;
    for (uint32_t k = 0; k < cnt; ++k) {
        C7BEdge eb;
        if (fread(&eb, C7B_REC, 1, c->f) != 1) break;
        as[k].main = (eb.b_id && c->map && eb.b_id < c->n_words) ? (Token *)c->map[eb.b_id] : NULL;
        as[k].rel = (uint16_t)eb.rel;
        as[k].dyn = eb.dyn;
        as[k].hits = 1;            /* 语料没有"命中数"：给 1 = 有这条边，不编造更多 */
        got++;
    }
    c->n_reads++;
    c->n_bytes += (size_t)got * C7B_REC;
    token_assocs_set_ram(t, as, got);
}

/* 把一片挂到脑上：词表按名字重建（w%06d）、索引进内存、有边的词打 DISK 标签。
 * 返回"挂了几个词"；<0 表示失败（err 里有原因）。 */
static inline int c7b_attach(AI *ai, C7B *c, char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    if (!ai || !ai->pool || !c) { if (err && cap) snprintf(err, cap, "没有脑子/没有分片"); return -1; }
    if (c7b_load_index(c) != 0) { if (err && cap) snprintf(err, cap, "索引读不进内存"); return -1; }
    char nm[32];
    c->map = (void **)calloc(c->n_words, sizeof(void *));
    if (!c->map) { if (err && cap) snprintf(err, cap, "内存不够（词表映射）"); return -1; }
    for (uint32_t i = 0; i < c->n_words; ++i) {
        snprintf(nm, sizeof nm, "w%06u", i);
        Token *t = pool_token_intern(ai->pool, nm);
        if (!t) { if (err && cap) snprintf(err, cap, "造词失败 @%u", i); return -1; }
        c->map[i] = t;
    }
    size_t ntag = 0;
    for (uint32_t k = 0; k < c->n_seg; ++k) {
        uint32_t id = c->iid[k], cnt = c->icnt[k];
        if (!cnt || id >= c->n_words) continue;
        Token *t = (Token *)c->map[id];
        if (!t) continue;
        t->count = cnt;
        token_assocs_set_disk(t);
        ntag++;
    }
    ai->pool->parts = c;
    ai->pool->part_load = c7b_load_token;
    ai->pool->part_touch = NULL;      /* 语料是只读的，不需要 LRU 回写钩子 */
    c->pool = ai->pool;
    return (int)ntag;
}

/* 摘掉：把每个"边在语料里"的词清成空表（文件不动）。
 * 关分片/换片之前必须调 —— 否则池里留着 DISK 标签，读边时会去解引用一个已经 free 的上下文。*/
static inline void c7b_detach(AI *ai, C7B *c) {
    if (!ai || !ai->pool) return;
    TokenPool *p = ai->pool;
    if (!c || p->parts != c) {                 /* 不是它挂的：只清指针 */
        if (c && p->parts == c) { p->parts = NULL; p->part_load = NULL; }
        return;
    }
    /* 只清**这一片自己的词**（按 attach 时的映射），而且两种状态都要清：
     *   · 还带 DISK 标签的（没被读过）；
     *   · **已经读过、在 RAM 里的** —— 第一版只清 DISK 标签，于是"查过一次的词"在 close 之后
     *     边还留在池子里（实测套件 ④b 当场红：close 前 10 条 / close 后还是 10 条）。*/
    for (uint32_t i = 0; c->map && i < c->n_words; ++i) {
        Token *t = (Token *)c->map[i];
        if (!t || t->count == 0) continue;
        token_assocs_set_ram(t, NULL, 0);      /* 摘掉边（文件还在，重新 attach 就有） */
    }
    if (!c->map) {
        for (size_t i = 0; i < p->count; ++i) {
            Token *t = p->tokens[i];
            if (!t || t->count == 0) continue;
            if (!token_assocs_on_disk(t)) continue;    /* 应用层不许直接摸边数组（assocaudit 盯着）*/
            token_assocs_set_ram(t, NULL, 0);
        }
    }
    p->parts = NULL;
    p->part_load = NULL;
    p->part_touch = NULL;
    c->pool = NULL;
}

#endif /* AIP_CORPUS7B_H */
