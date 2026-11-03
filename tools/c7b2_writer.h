#define _POSIX_C_SOURCE 200809L   /* strdup / fileno / fsync 在 -std=c11 下要显式要 */
/* tools/c7b2_writer.h — `.c7b` **v2** 写法（节点名 + 片内属性表；「文本事实 → 分片」共用；§6.716）
 *
 * 布局与 `tools/wd2c7b.c` 里那份一致（**这里是唯一一份实现**，新工具都 include 它；
 * `wd2c7b.c` 的历史内联副本保留，别再加第三份）：
 *
 *   头 64 B： "C7B2" | ver=2 | n_nodes | n_props | n_edges(u64) | n_seg | flags
 *            | names_off(u64) | names_len(u64) | props_off(u64) | segs_off(u64)
 *   节点名表 n_nodes × 8（off u32 / len u16 / 保留 u16）
 *   属性名表 n_props × 8（同）
 *   段表     n_nodes × 16（off u64 / cnt u32 / 保留 u32）
 *   名字池   节点名 + 属性名（UTF-8 + '\0'）
 *   边       cnt × 8（b_node u32 / prop u16 / 保留 u16）
 */
#ifndef AIP_C7B2_WRITER_H
#define _POSIX_C_SOURCE 200809L   /* strdup / fileno / fsync 在 -std=c11 下要显式要 */
#define AIP_C7B2_WRITER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define C7B2W_HDR  64u
#define C7B2W_NODE 8u
#define C7B2W_PROP 8u
#define C7B2W_SEG  16u
#define C7B2W_REC  8u

typedef struct { char **name; size_t *hash; uint32_t *id; size_t cap, n; } C7B2Tab;
typedef struct { uint32_t *b; uint16_t *p; uint32_t n, cap; } C7B2Vec;

typedef struct {
    C7B2Tab nodes, props;
    C7B2Vec *vec;
    size_t   vec_cap;
    long     edges;
    size_t   dropped;         /* 属性数超 65535（u16）而丢掉的边 */
} C7B2W;

static inline size_t c7b2w_hash(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
static inline int c7b2w_tab_init(C7B2Tab *t, size_t cap) {
    t->cap = cap < 1024 ? 1024 : cap; t->n = 0;
    t->name = (char **)calloc(t->cap, sizeof(char *));
    t->hash = (size_t *)calloc(t->cap, sizeof(size_t));
    t->id = (uint32_t *)calloc(t->cap, sizeof(uint32_t));
    return t->name && t->hash && t->id ? 0 : -1;
}
static inline uint32_t c7b2w_intern(C7B2Tab *t, const char *s) {
    size_t m = t->cap - 1, h = c7b2w_hash(s), i = h & m;
    for (;;) {
        if (!t->name[i]) { t->name[i] = strdup(s); t->hash[i] = h; t->id[i] = (uint32_t)t->n; return (uint32_t)(t->n++); }
        if (t->hash[i] == h && strcmp(t->name[i], s) == 0) return t->id[i];
        i = (i + 1) & m;
    }
}
static inline int c7b2w_init(C7B2W *w, size_t cap) {
    memset(w, 0, sizeof *w);
    return (c7b2w_tab_init(&w->nodes, cap) == 0 && c7b2w_tab_init(&w->props, 4096) == 0) ? 0 : -1;
}
static inline int c7b2w_push(C7B2Vec *v, uint32_t b, uint16_t p) {
    if (v->n == v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 4;
        uint32_t *nb = (uint32_t *)realloc(v->b, (size_t)nc * sizeof(uint32_t));
        uint16_t *np = (uint16_t *)realloc(v->p, (size_t)nc * sizeof(uint16_t));
        if (!nb || !np) { free(nb); free(np); return -1; }
        v->b = nb; v->p = np; v->cap = nc;
    }
    v->b[v->n] = b; v->p[v->n] = p; v->n++;
    return 0;
}
/* 加一条边：S --[P]--> O（S/P/O 都是名字；内部按 id 存） */
static inline int c7b2w_edge(C7B2W *w, const char *s, const char *p, const char *o) {
    uint32_t sid = c7b2w_intern(&w->nodes, s);
    uint32_t pid = c7b2w_intern(&w->props, p);
    uint32_t oid = c7b2w_intern(&w->nodes, o);
    if (pid > 65535u) { w->dropped++; return 0; }     /* u16 装不下：丢掉并计数 */
    if (sid >= w->vec_cap) {
        size_t nc = w->vec_cap ? w->vec_cap * 2 : 1024;
        while (nc <= sid) nc *= 2;
        C7B2Vec *nv = (C7B2Vec *)realloc(w->vec, nc * sizeof(C7B2Vec));
        if (!nv) return -1;
        memset(nv + w->vec_cap, 0, (nc - w->vec_cap) * sizeof(C7B2Vec));
        w->vec = nv; w->vec_cap = nc;
    }
    if (c7b2w_push(&w->vec[sid], oid, (uint16_t)pid) != 0) return -1;
    w->edges++;
    return 0;
}
/* 写一个分片；返回写进去的边数（<0 出错） */
static inline long c7b2w_write(C7B2W *w, const char *path) {
    uint32_t n_nodes = (uint32_t)w->nodes.n, n_props = (uint32_t)w->props.n;
    char **nname = (char **)calloc(n_nodes ? n_nodes : 1, sizeof(char *));
    char **pname = (char **)calloc(n_props ? n_props : 1, sizeof(char *));
    size_t *noff = (size_t *)calloc(n_nodes ? n_nodes : 1, sizeof(size_t));
    size_t *poff = (size_t *)calloc(n_props ? n_props : 1, sizeof(size_t));
    if (!nname || !pname || !noff || !poff) { free(nname); free(pname); free(noff); free(poff); return -1; }
    for (size_t i = 0; i < w->nodes.cap; ++i) if (w->nodes.name[i]) nname[w->nodes.id[i]] = w->nodes.name[i];
    for (size_t i = 0; i < w->props.cap; ++i) if (w->props.name[i]) pname[w->props.id[i]] = w->props.name[i];
    size_t names_len = 0;
    for (uint32_t i = 0; i < n_nodes; ++i) { noff[i] = names_len; names_len += strlen(nname[i]) + 1; }
    for (uint32_t i = 0; i < n_props; ++i) { poff[i] = names_len; names_len += strlen(pname[i]) + 1; }

    uint64_t names_off = C7B2W_HDR + (uint64_t)n_nodes * C7B2W_NODE + (uint64_t)n_props * C7B2W_PROP + (uint64_t)n_nodes * C7B2W_SEG;
    uint64_t segs_off  = C7B2W_HDR + (uint64_t)n_nodes * C7B2W_NODE + (uint64_t)n_props * C7B2W_PROP;
    uint64_t props_off = C7B2W_HDR + (uint64_t)n_nodes * C7B2W_NODE;

    FILE *f = fopen(path, "wb");
    if (!f) { free(nname); free(pname); free(noff); free(poff); return -1; }
    char magic[4] = { 'C', '7', 'B', '2' };
    uint32_t ver = 2u, nseg = n_nodes, flags = 0u;
    uint64_t nedges = (uint64_t)w->edges;
    fwrite(magic, 1, 4, f); fwrite(&ver, 4, 1, f); fwrite(&n_nodes, 4, 1, f); fwrite(&n_props, 4, 1, f);
    fwrite(&nedges, 8, 1, f); fwrite(&nseg, 4, 1, f); fwrite(&flags, 4, 1, f);
    fwrite(&names_off, 8, 1, f); fwrite(&names_len, 8, 1, f); fwrite(&props_off, 8, 1, f); fwrite(&segs_off, 8, 1, f);
    for (uint32_t i = 0; i < n_nodes; ++i) {
        uint32_t o32 = (uint32_t)noff[i]; uint16_t l16 = (uint16_t)strlen(nname[i]), r = 0;
        fwrite(&o32, 4, 1, f); fwrite(&l16, 2, 1, f); fwrite(&r, 2, 1, f);
    }
    for (uint32_t i = 0; i < n_props; ++i) {
        uint32_t o32 = (uint32_t)poff[i]; uint16_t l16 = (uint16_t)strlen(pname[i]), r = 0;
        fwrite(&o32, 4, 1, f); fwrite(&l16, 2, 1, f); fwrite(&r, 2, 1, f);
    }
    for (uint32_t i = 0; i < n_nodes; ++i) {          /* 段表占位 */
        uint64_t o = 0; uint32_t c = 0, r = 0;
        fwrite(&o, 8, 1, f); fwrite(&c, 4, 1, f); fwrite(&r, 4, 1, f);
    }
    for (uint32_t i = 0; i < n_nodes; ++i) fwrite(nname[i], strlen(nname[i]) + 1, 1, f);
    for (uint32_t i = 0; i < n_props; ++i) fwrite(pname[i], strlen(pname[i]) + 1, 1, f);

    uint64_t cur = names_off + names_len;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        uint64_t off = cur;
        uint32_t cnt = w->vec ? w->vec[i].n : 0;
        for (uint32_t k = 0; k < cnt; ++k) {
            uint32_t b = w->vec[i].b[k]; uint16_t pr = w->vec[i].p[k], r = 0;
            fwrite(&b, 4, 1, f); fwrite(&pr, 2, 1, f); fwrite(&r, 2, 1, f);
        }
        long save = ftell(f);
        if (fseek(f, (long)(segs_off + (uint64_t)i * C7B2W_SEG), SEEK_SET) == 0) {
            uint32_t r = 0;
            fwrite(&off, 8, 1, f); fwrite(&cnt, 4, 1, f); fwrite(&r, 4, 1, f);
            fseek(f, save, SEEK_SET);
        }
        cur += (uint64_t)cnt * C7B2W_REC;
    }
    fflush(f); fsync(fileno(f)); fclose(f);
    free(nname); free(pname); free(noff); free(poff);
    return w->edges;
}
static inline void c7b2w_free(C7B2W *w) {
    for (size_t i = 0; i < w->nodes.cap; ++i) free(w->nodes.name[i]);
    for (size_t i = 0; i < w->props.cap; ++i) free(w->props.name[i]);
    free(w->nodes.name); free(w->nodes.hash); free(w->nodes.id);
    free(w->props.name); free(w->props.hash); free(w->props.id);
    if (w->vec) { for (size_t i = 0; i < w->vec_cap; ++i) { free(w->vec[i].b); free(w->vec[i].p); } free(w->vec); }
    memset(w, 0, sizeof *w);
}
#endif /* AIP_C7B2_WRITER_H */
