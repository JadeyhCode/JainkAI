/* tools/wd2c7b.c — **Wikidata NT dump → `.c7b` v2 分片**（节点名 + 每片属性表；§6.712）
 *
 * 为什么必须升到 v2：
 *   ① v1 的词表是 `w%06u`（靠 n_words 重建）——Wikidata 的节点是 `Q123`，不复用这个约定；
 *   ② **引擎的动态关系上限是 64**（`token.h: AI_MAX_DYNREL`），而 Wikidata 光 1.25 MB 样本里就有
 *      446 个属性（全量上万）。把"属性"塞进引擎关系表是死路。
 *      ⇒ v2 把**属性表放进分片自己**（每片一张），边里只存 16 位属性号：8 B/边不变，
 *        却不再受 64 限制。
 *
 * v2 布局（小端）：
 *   头 64 B： magic "C7B2" | ver=2 | n_nodes | n_props | n_edges(u64) | n_seg | flags
 *            | names_off(u64) | names_len(u64) | props_off(u64) | segs_off(u64)
 *   节点名表： n_nodes × 8（name_off u32 / name_len u16 / 保留 u16）
 *   属性名表： n_props × 8（name_off u32 / name_len u16 / 保留 u16）
 *   段表    ： n_nodes × 16（off u64 / cnt u32 / 保留 u32）   ← 按节点 id 直接索引，O(1)
 *   名字池  ： 节点名 + 属性名，UTF-8，不补零
 *   边      ： cnt × 8（b_node u32 / prop u16 / 保留 u16）
 *
 * 用法：
 *   bzip2 -dc dump.nt.bz2 | wd2c7b <输出.c7b> [--max-edges N] [--min-edges N]
 *   wd2c7b --probe <文件.c7b> [Q42]
 *   bzip2 -dc dump.nt.bz2 | wd2c7b - | wc -l        ← `-` = 只统计不落盘（量产量用）
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>          /* fsync / fileno */

#define C7B2_HDR   64u
#define C7B2_NODE  8u
#define C7B2_PROP  8u
#define C7B2_SEG   16u
#define C7B2_REC   8u

/* ---------------- 名字 → id 的哈希表（节点/属性各一张） ---------------- */
typedef struct {
    char   **name;
    size_t  *hash;
    uint32_t *id;
    size_t   cap, n;
} NameTab;

static size_t hash_str(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static int nt_init(NameTab *t, size_t cap) {
    t->cap = cap < 1024 ? 1024 : cap;
    t->name = (char **)calloc(t->cap, sizeof(char *));
    t->hash = (size_t *)calloc(t->cap, sizeof(size_t));
    t->id = (uint32_t *)calloc(t->cap, sizeof(uint32_t));
    t->n = 0;
    return t->name && t->hash && t->id ? 0 : -1;
}
static int nt_find(const NameTab *t, const char *s, uint32_t *out_id) {
    size_t m = t->cap - 1;
    size_t h = hash_str(s);
    size_t i = h & m;
    for (;;) {
        if (!t->name[i]) return 0;
        if (t->hash[i] == h && strcmp(t->name[i], s) == 0) { *out_id = t->id[i]; return 1; }
        i = (i + 1) & m;
    }
}
static uint32_t nt_intern(NameTab *t, const char *s) {
    size_t m = t->cap - 1;
    size_t h = hash_str(s);
    size_t i = h & m;
    for (;;) {
        if (!t->name[i]) {
            t->name[i] = strdup(s);
            t->hash[i] = h;
            t->id[i] = (uint32_t)t->n;
            return (uint32_t)(t->n++);
        }
        if (t->hash[i] == h && strcmp(t->name[i], s) == 0) return t->id[i];
        i = (i + 1) & m;
    }
}

/* ---------------- 边缓冲（按节点分组：每个节点一个动态数组） ---------------- */
typedef struct { uint32_t *b; uint16_t *p; uint32_t n, cap; } EdgeVec;
static int ev_push(EdgeVec *v, uint32_t b, uint16_t p) {
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

static void put(FILE *f, const void *p, size_t n) { if (f) fwrite(p, n, 1, f); }

/* 把 NT 转成 v2 分片。out 为 NULL 时只统计。返回写进去的边数。 */
static long convert(FILE *out, long max_edges, long *n_nodes_out, long *n_props_out) {
    NameTab nodes, props;
    if (nt_init(&nodes, 1 << 20) != 0 || nt_init(&props, 4096) != 0) { fprintf(stderr, "内存不够（哈希表）\n"); return -1; }
    EdgeVec *vec = NULL;
    size_t vec_cap = 0;
    long edges = 0, skipped_lit = 0, bad = 0;
    char *line = NULL;
    size_t lcap = 0;
    ssize_t got;

    while ((got = getline(&line, &lcap, stdin)) > 0) {
        char *s = line, *e1 = NULL, *p = NULL, *e2 = NULL, *o = NULL;
        /* <subj> <pred> <obj> . */
        if (s[0] != '<') { bad++; continue; }
        e1 = strchr(s, '>');
        if (!e1) { bad++; continue; }
        *e1 = 0;
        p = e1 + 2;                       /* 跳过 "> " */
        if (p[0] != '<') { bad++; continue; }
        e2 = strchr(p, '>');
        if (!e2) { bad++; continue; }
        *e2 = 0;
        o = e2 + 2;
        if (o[0] != '<') { skipped_lit++; continue; }   /* 字面量/值：不收 */
        char *e3 = strchr(o, '>');
        if (!e3) { bad++; continue; }
        *e3 = 0;
        /* 只认 wikidata 实体 / 属性 */
        char *qs = strrchr(s + 1, '/');
        char *qp = strrchr(p + 1, '/');
        char *qo = strrchr(o + 1, '/');
        if (!qs || !qp || !qo) { bad++; continue; }
        qs++; qp++; qo++;
        if (qs[0] != 'Q' || qo[0] != 'Q' || qp[0] != 'P') { bad++; continue; }
        if (edges >= max_edges) break;

        uint32_t sid = nt_intern(&nodes, qs);
        uint32_t pid = nt_intern(&props, qp);
        uint32_t oid;
        if (!nt_find(&nodes, qo, &oid)) oid = nt_intern(&nodes, qo);

        if (sid >= vec_cap) {                 /* 按节点 id 索引的边数组（最多 2 倍扩容） */
            size_t nc = vec_cap ? vec_cap * 2 : 1024;
            while (nc <= sid) nc *= 2;
            EdgeVec *nv = (EdgeVec *)realloc(vec, nc * sizeof(EdgeVec));
            if (!nv) { fprintf(stderr, "内存不够（边缓冲）\n"); free(line); return -1; }
            memset(nv + vec_cap, 0, (nc - vec_cap) * sizeof(EdgeVec));
            vec = nv; vec_cap = nc;
        }
        if (ev_push(&vec[sid], oid, (uint16_t)pid) != 0) { fprintf(stderr, "内存不够（边）\n"); free(line); return -1; }
        edges++;
    }
    free(line);
    if (edges == 0) {
        fprintf(stderr, "FAIL: 一条实体-实体事实都没读到（字面量跳过 %ld / 坏行 %ld）\n", skipped_lit, bad);
        return -1;
    }

    /* 写文件：分成 头 / 节点名表 / 属性名表 / 段表 / 名字池 / 边 */
    uint32_t n_nodes = (uint32_t)nodes.n, n_props = (uint32_t)props.n;
    /* 名字池：节点名（按 id 顺序）+ 属性名 */
    size_t *noff = (size_t *)calloc(n_nodes ? n_nodes : 1, sizeof(size_t));
    size_t *poff = (size_t *)calloc(n_props ? n_props : 1, sizeof(size_t));
    size_t names_len = 0;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        /* 哈希表里顺序 != id 顺序 ⇒ 用一次反查把所有名字按 id 排好 */
        (void)noff;
    }
    /* 建立 id → name 的直表（省得每写一次都扫哈希） */
    char **nname = (char **)calloc(n_nodes ? n_nodes : 1, sizeof(char *));
    char **pname = (char **)calloc(n_props ? n_props : 1, sizeof(char *));
    for (size_t i = 0; i < nodes.cap; ++i) if (nodes.name[i]) nname[nodes.id[i]] = nodes.name[i];
    for (size_t i = 0; i < props.cap; ++i) if (props.name[i]) pname[props.id[i]] = props.name[i];
    for (uint32_t i = 0; i < n_nodes; ++i) { noff[i] = names_len; names_len += strlen(nname[i]) + 1; }
    for (uint32_t i = 0; i < n_props; ++i) { poff[i] = names_len; names_len += strlen(pname[i]) + 1; }

    /* 段表：off 先占位，写完边再回填 */
    uint64_t names_off = C7B2_HDR + (uint64_t)n_nodes * C7B2_NODE + (uint64_t)n_props * C7B2_PROP + (uint64_t)n_nodes * C7B2_SEG;
    uint64_t segs_off = C7B2_HDR + (uint64_t)n_nodes * C7B2_NODE + (uint64_t)n_props * C7B2_PROP;
    uint64_t props_off = C7B2_HDR + (uint64_t)n_nodes * C7B2_NODE;

    if (out) {
        char magic[4] = { 'C', '7', 'B', '2' };
        uint32_t ver = 2u, nseg = n_nodes, flags = 0u;
        uint64_t nedges = (uint64_t)edges;
        put(out, magic, 4); put(out, &ver, 4); put(out, &n_nodes, 4); put(out, &n_props, 4);
        put(out, &nedges, 8); put(out, &nseg, 4); put(out, &flags, 4);
        put(out, &names_off, 8); put(out, &names_len, 8); put(out, &props_off, 8); put(out, &segs_off, 8);
        /* 节点名表（off/len） */
        for (uint32_t i = 0; i < n_nodes; ++i) {
            uint32_t o32 = (uint32_t)noff[i];
            uint16_t l16 = (uint16_t)strlen(nname[i]);
            uint16_t rsv = 0;
            put(out, &o32, 4); put(out, &l16, 2); put(out, &rsv, 2);
        }
        /* 属性名表 */
        for (uint32_t i = 0; i < n_props; ++i) {
            uint32_t o32 = (uint32_t)poff[i];
            uint16_t l16 = (uint16_t)strlen(pname[i]);
            uint16_t rsv = 0;
            put(out, &o32, 4); put(out, &l16, 2); put(out, &rsv, 2);
        }
        /* 段表占位 */
        for (uint32_t i = 0; i < n_nodes; ++i) {
            uint64_t off0 = 0; uint32_t c0 = 0, r0 = 0;
            put(out, &off0, 8); put(out, &c0, 4); put(out, &r0, 4);
        }
        /* 名字池 */
        for (uint32_t i = 0; i < n_nodes; ++i) put(out, nname[i], strlen(nname[i]) + 1);
        for (uint32_t i = 0; i < n_props; ++i) put(out, pname[i], strlen(pname[i]) + 1);
    }

    /* 边（按节点 id 顺序写，正好是段） */
    uint64_t cur = names_off + names_len;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        uint64_t off = cur;
        uint32_t cnt = vec ? vec[i].n : 0;
        if (out && cnt) {
            for (uint32_t k = 0; k < cnt; ++k) {
                uint32_t b = vec[i].b[k];
                uint16_t pr = vec[i].p[k], rsv = 0;
                put(out, &b, 4); put(out, &pr, 2); put(out, &rsv, 2);
            }
        }
        if (out) {   /* 回填段表 */
            long save = ftell(out);
            if (fseek(out, (long)(segs_off + (uint64_t)i * C7B2_SEG), SEEK_SET) == 0) {
                uint32_t r0 = 0;
                put(out, &off, 8); put(out, &cnt, 4); put(out, &r0, 4);
                fseek(out, save, SEEK_SET);
            }
        }
        cur += (uint64_t)cnt * C7B2_REC;
    }
    if (out) { fflush(out); fsync(fileno(out)); }

    fprintf(stderr, "  v2 分片：节点 %u / 属性 %u / 边 %ld（字面量跳过 %ld / 坏行 %ld）\n",
            n_nodes, n_props, edges, skipped_lit, bad);
    *n_nodes_out = n_nodes; *n_props_out = n_props;
    free(noff); free(poff); free(nname); free(pname);
    for (size_t i = 0; i < nodes.cap; ++i) free(nodes.name[i]);
    for (size_t i = 0; i < props.cap; ++i) free(props.name[i]);
    free(nodes.name); free(nodes.hash); free(nodes.id);
    free(props.name); free(props.hash); free(props.id);
    if (vec) { for (size_t i = 0; i < vec_cap; ++i) { free(vec[i].b); free(vec[i].p); } free(vec); }
    return edges;
}

/* ---------------- 校验 + 抽查 ---------------- */
static int probe(const char *path, const char *qid) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "读不开 %s\n", path); return 1; }
    char magic[4]; uint32_t ver, n_nodes, n_props, n_seg, flags;
    uint64_t n_edges, names_off, names_len, props_off, segs_off;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "C7B2", 4) != 0) { fprintf(stderr, "不是 C7B2\n"); fclose(f); return 1; }
    if (fread(&ver, 4, 1, f) != 1 || fread(&n_nodes, 4, 1, f) != 1 || fread(&n_props, 4, 1, f) != 1 ||
        fread(&n_edges, 8, 1, f) != 1 || fread(&n_seg, 4, 1, f) != 1 || fread(&flags, 4, 1, f) != 1 ||
        fread(&names_off, 8, 1, f) != 1 || fread(&names_len, 8, 1, f) != 1 ||
        fread(&props_off, 8, 1, f) != 1 || fread(&segs_off, 8, 1, f) != 1) { fprintf(stderr, "头读不全\n"); fclose(f); return 1; }
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell(f);
    uint64_t want = names_off + names_len + n_edges * C7B2_REC;
    printf("%s\n  v%u 节点 %u 属性 %u 边 %llu（n_seg %u）文件 %llu 字节（期望 %llu）%s\n",
           path, ver, n_nodes, n_props, (unsigned long long)n_edges, n_seg,
           (unsigned long long)size, (unsigned long long)want, size == want ? " ✔" : " ✘");
    if (!qid) { fclose(f); return size == want ? 0 : 1; }

    /* 找节点：线性扫名字（样本够用；正式查询该建反查表） */
    char *names = (char *)malloc((size_t)names_len);
    if (!names) { fclose(f); return 1; }
    fseek(f, (long)names_off, SEEK_SET);
    if (fread(names, 1, (size_t)names_len, f) != names_len) { free(names); fclose(f); return 1; }
    uint32_t nid = UINT32_MAX;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        uint32_t o32; uint16_t l16, rsv;
        fseek(f, (long)(C7B2_HDR + (uint64_t)i * C7B2_NODE), SEEK_SET);
        if (fread(&o32, 4, 1, f) != 1 || fread(&l16, 2, 1, f) != 1 || fread(&rsv, 2, 1, f) != 1) break;
        if (o32 + l16 > names_len) continue;
        if (strcmp(names + o32, qid) == 0) { nid = i; break; }
    }
    if (nid == UINT32_MAX) { printf("  %s：这个分片里没有它\n", qid); free(names); fclose(f); return 0; }
    uint64_t off; uint32_t cnt, rsv;
    fseek(f, (long)(segs_off + (uint64_t)nid * C7B2_SEG), SEEK_SET);
    if (fread(&off, 8, 1, f) != 1 || fread(&cnt, 4, 1, f) != 1 || fread(&rsv, 4, 1, f) != 1) { free(names); fclose(f); return 1; }
    printf("  %s 的边 %u 条：", qid, cnt);
    char *props = (char *)malloc((size_t)names_len);
    if (props) memcpy(props, names, (size_t)names_len);
    fseek(f, (long)off, SEEK_SET);
    for (uint32_t k = 0; k < cnt && k < 8; ++k) {
        uint32_t b; uint16_t pr; uint16_t r2;
        if (fread(&b, 4, 1, f) != 1 || fread(&pr, 2, 1, f) != 1 || fread(&r2, 2, 1, f) != 1) break;
        /* 属性名 */
        uint32_t po; uint16_t pl, pr2;
        fseek(f, (long)(props_off + (uint64_t)pr * C7B2_PROP), SEEK_SET);
        if (fread(&po, 4, 1, f) != 1 || fread(&pl, 2, 1, f) != 1 || fread(&pr2, 2, 1, f) != 1) break;
        const char *pname = (props && po + pl <= names_len) ? props + po : "?";
        /* 宾语名 */
        uint32_t bo; uint16_t bl, br;
        fseek(f, (long)(C7B2_HDR + (uint64_t)b * C7B2_NODE), SEEK_SET);
        if (fread(&bo, 4, 1, f) != 1 || fread(&bl, 2, 1, f) != 1 || fread(&br, 2, 1, f) != 1) break;
        const char *bname = (bo + bl <= names_len) ? names + bo : "?";
        printf(" %s --[%s]--> %s ;", qid, pname, bname);
        fseek(f, (long)(off + (uint64_t)(k + 1) * C7B2_REC), SEEK_SET);
    }
    printf("\n");
    free(props); free(names); fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--probe")) return probe(argv[2], argc > 3 ? argv[3] : NULL);
    if (argc < 2) {
        fprintf(stderr, "用法: <NT> | %s <输出.c7b|-> [--max-edges N]\n       %s --probe <文件.c7b> [Q42]\n", argv[0], argv[0]);
        return 2;
    }
    const char *out = argv[1];
    long max_edges = 20000000;
    for (int i = 2; i + 1 < argc; ++i) if (!strcmp(argv[i], "--max-edges")) max_edges = strtol(argv[i + 1], NULL, 10);
    FILE *f = NULL;
    if (strcmp(out, "-") != 0) {
        f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "写不开 %s\n", out); return 1; }
    }
    long nn = 0, np = 0;
    long e = convert(f, max_edges, &nn, &np);
    if (f) fclose(f);
    if (e < 0) return 1;
    printf("  边 %ld / 节点 %ld / 属性 %ld ⇒ %.1f 字节/边（含名字与索引）\n",
           e, nn, np, f ? (double)(C7B2_HDR + (double)nn * (C7B2_NODE + C7B2_SEG) + (double)np * C7B2_PROP) / (double)e + C7B2_REC : 0.0);
    return 0;
}
