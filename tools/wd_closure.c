/* tools/wd_closure.c — 在**真事实**上做**可证明的展开**（阶段四：7B 真知识的唯一不等带宽的路；§6.714）
 *
 * 为什么不"编"：所有派生边都来自**两条/多条真实断言**，并在 proofs 文件里写出**证据链**：
 *     Q31 --属于--> Q7275      证据：Q31 P31 Q3624078 ; Q3624078 P279 Q7275
 * 只做两族（都基于 Wikidata 的真实层级属性）：
 *   ① **实例继承**：`x P31 c` + `c P279* a` ⇒ `x 属于 a`（a 连 c 自己一起，含 c 本身）
 *   ② **部分传递**：`x P361 p` + `p P361* q` ⇒ `x 部分 q`
 * 传递闭包**有深度上限**（默认 12）：不设上限的话一条长链会把边数炸成天文数字，
 * 而且"深度 30 的祖先"人类也不认。
 *
 * 输出（`::ingest` 直接吃）：
 *   <out>.triples   实体:X<TAB>属于/部分<TAB>实体:Y
 *   <out>.proofs    前 2000 条的证据链 + 深度直方图（可核对，不是黑箱）
 *
 * 用法：bzip2 -dc dump.nt.bz2 | wd_closure <out> [--max-depth N] [--max-base N]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { char **name; size_t *hash; uint32_t *id; size_t cap, n; } Tab;
static size_t hstr(const char *s) { size_t h = 1469598103934665603ULL; for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }
static int tab_init(Tab *t, size_t cap) {
    t->cap = cap < 1024 ? 1024 : cap; t->n = 0;
    t->name = (char **)calloc(t->cap, sizeof(char *));
    t->hash = (size_t *)calloc(t->cap, sizeof(size_t));
    t->id = (uint32_t *)calloc(t->cap, sizeof(uint32_t));
    return t->name && t->hash && t->id ? 0 : -1;
}
static int tab_find(const Tab *t, const char *s, uint32_t *o) {
    size_t m = t->cap - 1, h = hstr(s), i = h & m;
    for (;;) {
        if (!t->name[i]) return 0;
        if (t->hash[i] == h && strcmp(t->name[i], s) == 0) { *o = t->id[i]; return 1; }
        i = (i + 1) & m;
    }
}
static uint32_t tab_add(Tab *t, const char *s) {
    size_t m = t->cap - 1, h = hstr(s), i = h & m;
    for (;;) {
        if (!t->name[i]) { t->name[i] = strdup(s); t->hash[i] = h; t->id[i] = (uint32_t)t->n; return (uint32_t)(t->n++); }
        if (t->hash[i] == h && strcmp(t->name[i], s) == 0) return t->id[i];
        i = (i + 1) & m;
    }
}
static uint32_t intern(Tab *t, const char *s) { uint32_t x; return tab_find(t, s, &x) ? x : tab_add(t, s); }

/* 动态边表（from,to） */
typedef struct { uint32_t *a, *b; size_t n, cap; } Pairs;
static int pairs_push(Pairs *p, uint32_t a, uint32_t b) {
    if (p->n == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 1024;
        uint32_t *na = (uint32_t *)realloc(p->a, nc * sizeof(uint32_t));
        uint32_t *nb = (uint32_t *)realloc(p->b, nc * sizeof(uint32_t));
        if (!na || !nb) { free(na); free(nb); return -1; }
        p->a = na; p->b = nb; p->cap = nc;
    }
    p->a[p->n] = a; p->b[p->n] = b; p->n++;
    return 0;
}
/* CSR（按 from 排序的邻接） */
typedef struct { uint32_t *off, *to; size_t n_nodes; } Csr;
static int csr_build(const Pairs *p, size_t n_nodes, Csr *c) {
    c->n_nodes = n_nodes;
    c->off = (uint32_t *)calloc(n_nodes + 1, sizeof(uint32_t));
    c->to = (uint32_t *)malloc((p->n ? p->n : 1) * sizeof(uint32_t));
    if (!c->off || !c->to) return -1;
    for (size_t i = 0; i < p->n; ++i) c->off[p->a[i] + 1]++;
    for (size_t i = 1; i <= n_nodes; ++i) c->off[i] += c->off[i - 1];
    uint32_t *cur = (uint32_t *)malloc((n_nodes ? n_nodes : 1) * sizeof(uint32_t));
    if (!cur) return -1;
    memcpy(cur, c->off, n_nodes * sizeof(uint32_t));
    for (size_t i = 0; i < p->n; ++i) c->to[cur[p->a[i]]++] = p->b[i];
    free(cur);
    return 0;
}

static char *line = NULL;
static size_t lcap = 0;

/* 解析一行 NT：<Q..> <prop/direct/P..> <Q..> .    返回 1 = 拿到三元组 */
static int parse_nt(char *s, char **qsubj, char **qprop, char **qobj) {
    if (s[0] != '<') return 0;
    char *e1 = strchr(s, '>'); if (!e1) return 0; *e1 = 0;
    char *p = e1 + 2; if (p[0] != '<') return 0;
    char *e2 = strchr(p, '>'); if (!e2) return 0; *e2 = 0;
    char *o = e2 + 2; if (o[0] != '<') return 0;
    char *e3 = strchr(o, '>'); if (!e3) return 0; *e3 = 0;
    char *rs = strrchr(s + 1, '/'), *rp = strrchr(p + 1, '/'), *ro = strrchr(o + 1, '/');
    if (!rs || !rp || !ro) return 0;
    *qsubj = rs + 1; *qprop = rp + 1; *qobj = ro + 1;
    return (qsubj[0][0] == 'Q' && qobj[0][0] == 'Q' && qprop[0][0] == 'P') ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: bzip2 -dc dump.nt.bz2 | %s <out> [--max-depth N] [--max-base N]\n", argv[0]); return 2; }
    const char *out = argv[1];
    int max_depth = 12;
    long max_base = 0;
    for (int i = 2; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--max-depth")) max_depth = (int)strtol(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--max-base")) max_base = strtol(argv[i + 1], NULL, 10);
    }
    Tab tab;
    if (tab_init(&tab, 1 << 20) != 0) { fprintf(stderr, "内存不够（哈希表）\n"); return 1; }
    Pairs sub = { 0 }, part = { 0 }, inst = { 0 };   /* P279 / P361 / P31 */
    long base = 0;
    ssize_t got;
    while ((got = getline(&line, &lcap, stdin)) > 0) {
        char *qs, *qp, *qo;
        if (!parse_nt(line, &qs, &qp, &qo)) continue;
        if (max_base > 0 && base >= max_base) break;
        base++;
        uint32_t s = intern(&tab, qs), o = intern(&tab, qo);
        if (!strcmp(qp, "P279")) { if (pairs_push(&sub, s, o) != 0) return 1; }
        else if (!strcmp(qp, "P361")) { if (pairs_push(&part, s, o) != 0) return 1; }
        else if (!strcmp(qp, "P31")) { if (pairs_push(&inst, s, o) != 0) return 1; }
    }
    free(line);
    fprintf(stderr, "  读入真事实 %ld 条 ⇒ P279 %zu / P361 %zu / P31 %zu；节点 %zu\n",
            base, sub.n, part.n, inst.n, tab.n);

    size_t N = tab.n;
    Csr cs, cp;
    if (csr_build(&sub, N, &cs) != 0 || csr_build(&part, N, &cp) != 0) { fprintf(stderr, "内存不够（CSR）\n"); return 1; }

    /* 名字直表（按 id） */
    char **nm = (char **)calloc(N ? N : 1, sizeof(char *));
    for (size_t i = 0; i < tab.cap; ++i) if (tab.name[i]) nm[tab.id[i]] = tab.name[i];

    /* 每个源的闭包用"戳记"去重，避免每次 memset */
    int32_t *seen = (int32_t *)calloc(N ? N : 1, sizeof(int32_t));
    uint32_t *queue = (uint32_t *)malloc((N ? N : 1) * sizeof(uint32_t));
    uint32_t *depth = (uint32_t *)malloc((N ? N : 1) * sizeof(uint32_t));
    if (!seen || !queue || !depth) { fprintf(stderr, "内存不够（闭包）\n"); return 1; }

    char tpath[512], ppath[512];
    snprintf(tpath, sizeof tpath, "%s.triples", out);
    snprintf(ppath, sizeof ppath, "%s.proofs", out);
    FILE *ft = fopen(tpath, "wb");
    FILE *fp = fopen(ppath, "wb");
    if (!ft || !fp) { fprintf(stderr, "写不开 %s\n", out); return 1; }
    fprintf(fp, "# 派生边的证据链（前 2000 条；其余见统计）\n");

    long derived = 0, proofs = 0;
    long hist[64] = { 0 };
    /* ① 实例继承：x P31 c ⇒ x 属于 (c 及 c 的所有 P279 祖先) */
    for (size_t k = 0; k < inst.n; ++k) {
        uint32_t x = inst.a[k], c = inst.b[k];
        /* BFS：从 c 出发走 P279，含 c 本身（深度 0） */
        uint32_t head = 0, tail = 0;
        seen[c] = (int32_t)(k + 1);
        queue[tail] = c; depth[tail] = 0; tail++;
        while (head < tail) {
            uint32_t u = queue[head], d = depth[head]; head++;
            fprintf(ft, "实体:%s\t属于\t实体:%s\n", nm[x], nm[u]);
            derived++; hist[d < 64 ? d : 63]++;
            if (proofs < 2000) {
                if (d == 0) fprintf(fp, "实体:%s 属于 实体:%s   证据：%s P31 %s\n", nm[x], nm[u], nm[x], nm[u]);
                else        fprintf(fp, "实体:%s 属于 实体:%s   证据：%s P31 %s ; %s P279* %s（深度 %u）\n",
                                    nm[x], nm[u], nm[x], nm[c], nm[c], nm[u], d);
                proofs++;
            }
            if (d >= (uint32_t)max_depth) continue;
            for (uint32_t e = cs.off[u]; e < cs.off[u + 1]; ++e) {
                uint32_t v = cs.to[e];
                if (seen[v] == (int32_t)(k + 1)) continue;   /* 环/重复 */
                seen[v] = (int32_t)(k + 1);
                queue[tail] = v; depth[tail] = d + 1; tail++;
            }
        }
    }
    /* ② 部分传递：x P361 p ⇒ x 部分 (p 的 P361 祖先) */
    for (size_t k = 0; k < part.n; ++k) {
        uint32_t x = part.a[k], p = part.b[k];
        uint32_t head = 0, tail = 0;
        seen[p] = (int32_t)(k + 1);
        queue[tail] = p; depth[tail] = 0; tail++;
        while (head < tail) {
            uint32_t u = queue[head], d = depth[head]; head++;
            fprintf(ft, "实体:%s\t部分\t实体:%s\n", nm[x], nm[u]);
            derived++; hist[d < 64 ? d : 63]++;
            if (proofs < 2000) { fprintf(fp, "实体:%s 部分 实体:%s   证据：%s P361 %s 传递（深度 %u）\n", nm[x], nm[u], nm[x], nm[u], d); proofs++; }
            if (d >= (uint32_t)max_depth) continue;
            for (uint32_t e = cp.off[u]; e < cp.off[u + 1]; ++e) {
                uint32_t v = cp.to[e];
                if (seen[v] == (int32_t)(k + 1)) continue;
                seen[v] = (int32_t)(k + 1);
                queue[tail] = v; depth[tail] = d + 1; tail++;
            }
        }
    }
    fprintf(fp, "\n# 深度直方图（深度 0 = 断言本身）\n");
    for (int d = 0; d < 64; ++d) if (hist[d]) fprintf(fp, "# 深度 %d：%ld 条\n", d, hist[d]);
    fclose(ft); fclose(fp);

    printf("  真事实 %ld 条中的层级部分：P279 %zu / P361 %zu / P31 %zu\n", base, sub.n, part.n, inst.n);
    printf("  ⇒ 派生边 **%ld** 条（约 %.1f 条/真事实），其中深度 0（断言本身）%ld 条、深度≥1 %ld 条\n",
           derived, base ? (double)derived / (double)base : 0.0, hist[0], derived - hist[0]);
    printf("  证据链：%s（前 %ld 条 + 深度直方图）；三元组：%s\n", ppath, proofs, tpath);
    return 0;
}
