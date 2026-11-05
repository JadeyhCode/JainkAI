/* tools/c7b2dump.c — 把 `.c7b` v2 分片**导出成三元组文本**（后训练用）§6.751
 *
 * 为什么：分片只能被 `::parts corpus` 查询，不能回灌进模型。要"把真知识灌进模型再测"，
 * 得先能把片里的边倒出来成 `主体<TAB>关系<TAB>客体`（`::batch` 吃的格式）。
 *
 * 用法：zcat shard.c7b2.gz > /tmp/s.c7b2
 *       c7b2dump /tmp/s.c7b2 [最多条数] > facts.triples
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <分片.c7b2> [最多条数]\n", argv[0]); return 2; }
    long maxn = (argc > 2) ? strtol(argv[2], NULL, 10) : 1000000;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", argv[1]); return 1; }
    char magic[4]; uint32_t ver, n_nodes, n_props, nseg, flags;
    uint64_t n_edges, names_off, names_len;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "C7B2", 4) != 0) { fprintf(stderr, "不是 v2 分片\n"); return 1; }
    if (fread(&ver, 4, 1, f) != 1 || fread(&n_nodes, 4, 1, f) != 1 || fread(&n_props, 4, 1, f) != 1 ||
        fread(&n_edges, 8, 1, f) != 1 || fread(&nseg, 4, 1, f) != 1 || fread(&flags, 4, 1, f) != 1 ||
        fread(&names_off, 8, 1, f) != 1 || fread(&names_len, 8, 1, f) != 1) {
        fprintf(stderr, "头读失败\n"); return 1;
    }
    uint64_t props_off = 0, segs_off = 0;
    if (fread(&props_off, 8, 1, f) != 1 || fread(&segs_off, 8, 1, f) != 1) { fprintf(stderr, "头读失败2\n"); return 1; }
    if (ver != 2) { fprintf(stderr, "只支持 v2（这片的 ver=%u）\n", ver); return 1; }

    /* 名字池 */
    char *pool = (char *)malloc((size_t)names_len + 1);
    if (!pool) { fprintf(stderr, "内存不够\n"); return 1; }
    if (fseek(f, (long)names_off, SEEK_SET) != 0 || fread(pool, 1, (size_t)names_len, f) != names_len) {
        fprintf(stderr, "名字池读失败\n"); return 1;
    }
    pool[names_len] = 0;

    /* 节点名表 / 属性名表 / 段表 */
    uint32_t *noff = (uint32_t *)malloc(((size_t)n_nodes + 1) * sizeof(uint32_t));
    uint16_t *nlen = (uint16_t *)malloc(((size_t)n_nodes + 1) * sizeof(uint16_t));
    uint32_t *poff = (uint32_t *)malloc(((size_t)n_props + 1) * sizeof(uint32_t));
    uint16_t *plen = (uint16_t *)malloc(((size_t)n_props + 1) * sizeof(uint16_t));
    uint64_t *soff = (uint64_t *)malloc(((size_t)n_nodes + 1) * sizeof(uint64_t));
    uint32_t *scnt = (uint32_t *)malloc(((size_t)n_nodes + 1) * sizeof(uint32_t));
    if (!noff || !nlen || !poff || !plen || !soff || !scnt) { fprintf(stderr, "内存不够2\n"); return 1; }
    if (fseek(f, 64, SEEK_SET) != 0) return 1;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        uint32_t o; uint16_t l, r;
        if (fread(&o, 4, 1, f) != 1 || fread(&l, 2, 1, f) != 1 || fread(&r, 2, 1, f) != 1) { fprintf(stderr, "节点名表坏\n"); return 1; }
        noff[i] = o; nlen[i] = l;
    }
    if (fseek(f, (long)props_off, SEEK_SET) != 0) return 1;
    for (uint32_t i = 0; i < n_props; ++i) {
        uint32_t o; uint16_t l, r;
        if (fread(&o, 4, 1, f) != 1 || fread(&l, 2, 1, f) != 1 || fread(&r, 2, 1, f) != 1) { fprintf(stderr, "属性表坏\n"); return 1; }
        poff[i] = o; plen[i] = l;
    }
    if (fseek(f, (long)segs_off, SEEK_SET) != 0) return 1;
    for (uint32_t i = 0; i < n_nodes; ++i) {
        uint64_t o; uint32_t c, r;
        if (fread(&o, 8, 1, f) != 1 || fread(&c, 4, 1, f) != 1 || fread(&r, 4, 1, f) != 1) { fprintf(stderr, "段表坏\n"); return 1; }
        soff[i] = o; scnt[i] = c;
    }

    long emitted = 0;
    for (uint32_t i = 0; i < n_nodes && emitted < maxn; ++i) {
        if (!scnt[i]) continue;
        if (fseek(f, (long)soff[i], SEEK_SET) != 0) continue;
        uint32_t b; uint16_t p, r;
        for (uint32_t k = 0; k < scnt[i] && emitted < maxn; ++k) {
            if (fread(&b, 4, 1, f) != 1 || fread(&p, 2, 1, f) != 1 || fread(&r, 2, 1, f) != 1) break;
            if (b >= n_nodes || p >= n_props) continue;
            printf("%.*s\t%.*s\t%.*s\n", (int)nlen[i], pool + noff[i], (int)plen[p], pool + poff[p],
                   (int)nlen[b], pool + noff[b]);
            emitted++;
        }
    }
    fclose(f);
    fprintf(stderr, "导出 %ld 条（片内 %llu 条边 / %u 节点 / %u 属性）\n", emitted,
            (unsigned long long)n_edges, n_nodes, n_props);
    return 0;
}
