/* tools/mkcorpus7b.c — **7B 边合成语料的分片写入器**（阶段四扩图；§6.711）
 *
 * 为什么另起一种格式（`.c7b`）：0.5B 那批分片是**完整模型**（59.2 B/边），
 * 1B 就要 59 GB、7B 要 **414 GB** —— 本机 `/mnt/models` 只剩 327 GB，装不下。
 * 这里只存**边**（8 B/边：邻居 4 + 关系 2 + 动态槽 1 + 保留 1），词表按名字重建
 * （`w%06d` 是确定性的）⇒ 7B 只要 **≈ 56 GB**。
 *
 * 文件布局（小端，定长）：
 *   头 48 字节：magic "C7B1" / version / n_words / n_segments / n_edges / payload_off / seed / rel_tail / rsv
 *   索引：n_segments × 16 字节（id u32 / off u64 / cnt u32）
 *   段：cnt × 8 字节（b_id u32 / rel u16 / dyn u8 / pad u8）
 *
 * 用法：
 *   mkcorpus7b <输出文件> <词数> <边数> <分片号> [种子] [出度上限]
 *   mkcorpus7b --probe <文件> [词名]      ← 校验 + 查一个词的邻居（验收用）
 *
 * 分布与 `tools/mkcorpus.c` 的 zipf/tail 一致（主语按 Zipf(1.2)，关系 tail），
 * 但**逐条边直接生成**（不过 `ai_write_rel` 的 O(出度) 去重）⇒ 20M 边是秒级，不是分钟级。
 * 重复边在这里**不会产生**：每个词的邻居用小型哈希集合去重（同一条边只会写一次）。
 */
#define _POSIX_C_SOURCE 200809L   /* fileno/fsync 在 -std=c11 下要显式要 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>          /* fsync / fileno */

#define C7B_MAGIC0 'C'
#define C7B_MAGIC1 '7'
#define C7B_MAGIC2 'B'
#define C7B_MAGIC3 '1'
#define C7B_VERSION 1u
#define C7B_REC 8u
#define C7B_IDX 16u
#define C7B_HEAD 48u

#define REL_IS_A  1u
#define REL_HAS   2u
#define REL_CAN   3u
#define REL_FEAR  4u
#define REL_LIKE  5u
#define REL_AT    7u
#define REL_DYN   11u          /* 动态关系槽（REL_DYN_BASE+1）*/

typedef struct { uint32_t b_id; uint16_t rel; uint8_t dyn; uint8_t pad; } C7BEdge;

static unsigned long long g_seed;
static unsigned long long nextr(void) {
    g_seed += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = g_seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static double rnd01(void) { return (double)(nextr() >> 11) / 9007199254740992.0; }

/* tail 关系分布（和 mkcorpus 一致）：属于 80 / 会 8 / 有 6 / 在 4 / 喜欢 1 / 怕 0.5 / 动态 0.5 */
static uint16_t pick_rel(void) {
    double u = rnd01() * 100.0;
    if (u < 80.0) return REL_IS_A;
    if (u < 88.0) return REL_CAN;
    if (u < 94.0) return REL_HAS;
    if (u < 98.0) return REL_AT;
    if (u < 99.0) return REL_LIKE;
    if (u < 99.5) return REL_FEAR;
    return REL_DYN;
}

/* 逐词出度：Zipf(1.2) 权重 + 出度上限，再把余数补平到 n_edges */
static long *assign_degrees(long n_word, long n_edges, long cap) {
    long *deg = (long *)calloc((size_t)n_word, sizeof(long));
    if (!deg) return NULL;
    double sumw = 0.0;
    for (long i = 0; i < n_word; ++i) sumw += 1.0 / pow((double)i + 1.0, 1.2);
    long total = 0;
    for (long i = 0; i < n_word; ++i) {
        double w = 1.0 / pow((double)i + 1.0, 1.2);
        long d = (long)(w / sumw * (double)n_edges);
        if (d < 1) d = 1;
        if (d > cap) d = cap;
        deg[i] = d;
        total += d;
    }
    /* 补/削到正好 n_edges（只动"还没到上限"的词，从尾巴开始，保证确定性） */
    for (long i = n_word - 1; i >= 0 && total != n_edges; --i) {
        if (total < n_edges && deg[i] < cap) {
            long add = n_edges - total;
            long room = cap - deg[i];
            if (add > room) add = room;
            deg[i] += add; total += add;
        } else if (total > n_edges && deg[i] > 1) {
            long sub = total - n_edges;
            long room = deg[i] - 1;
            if (sub > room) sub = room;
            deg[i] -= sub; total -= sub;
        }
    }
    return deg;
}

/* 每词一个去重集合（开放寻址；空槽 = 0xFFFFFFFF） */
typedef struct { uint32_t *slot; size_t cap; } IdSet;
static int idset_init(IdSet *s, size_t expect) {
    size_t cap = 8;
    while (cap < expect * 2 + 8) cap <<= 1;
    s->slot = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!s->slot) return -1;
    memset(s->slot, 0xFF, cap * sizeof(uint32_t));
    s->cap = cap;
    return 0;
}
static int idset_add(IdSet *s, uint32_t v) {   /* 1 = 新加进去；0 = 已存在 */
    size_t m = s->cap - 1;
    size_t h = ((size_t)v * 0x9E3779B1u) & m;
    for (;;) {
        if (s->slot[h] == 0xFFFFFFFFu) { s->slot[h] = v; return 1; }
        if (s->slot[h] == v) return 0;
        h = (h + 1) & m;
    }
}

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

static int cmd_make(const char *out, long n_word, long n_edges, long shard, unsigned long long seed, long cap) {
    if (n_word < 100 || n_word > 5000000L) { fprintf(stderr, "词数范围 100..5e6\n"); return 2; }
    if (n_edges < 0 || n_edges > 200000000L) { fprintf(stderr, "边数范围 0..2e8\n"); return 2; }
    if (cap < 1) cap = 1;

    long *deg = assign_degrees(n_word, n_edges, cap);
    if (!deg) { fprintf(stderr, "内存不够\n"); return 1; }
    long nseg = 0, sum = 0;
    for (long i = 0; i < n_word; ++i) { if (deg[i] > 0) nseg++; sum += deg[i]; }

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "写不开 %s\n", out); free(deg); return 1; }
    /* 头 48 字节 */
    fputc(C7B_MAGIC0, f); fputc(C7B_MAGIC1, f); fputc(C7B_MAGIC2, f); fputc(C7B_MAGIC3, f);
    put_u32(f, C7B_VERSION);
    put_u32(f, (uint32_t)n_word);
    put_u32(f, (uint32_t)nseg);
    put_u64(f, (uint64_t)sum);
    put_u64(f, (uint64_t)(C7B_HEAD + (uint64_t)nseg * C7B_IDX));
    put_u64(f, seed);
    put_u32(f, 1u);              /* rel_tail = 1 */
    put_u32(f, 0u);
    if (ftell(f) != (long)C7B_HEAD) { fprintf(stderr, "头长度不对：%ld\n", ftell(f)); fclose(f); free(deg); return 1; }

    /* 索引：先占位，写完段回填（id/off/cnt） */
    for (long k = 0; k < nseg; ++k) { put_u32(f, 0); put_u64(f, 0); put_u32(f, 0); }

    /* 逐词写段：每个词一个去重集合（出度小，开一次用完就换） */
    uint64_t rec_off = (uint64_t)(C7B_HEAD + (uint64_t)nseg * C7B_IDX);
    long seg = 0;
    IdSet set = { NULL, 0 };
    for (long i = 0; i < n_word; ++i) {
        if (deg[i] <= 0) continue;
        if (idset_init(&set, (size_t)deg[i]) != 0) { fprintf(stderr, "内存不够（去重集合）\n"); fclose(f); free(deg); return 1; }
        uint32_t cnt = 0;
        while (cnt < (uint32_t)deg[i]) {
            uint32_t b;
            /* 邻居按 zipf 偏斜（和 mkcorpus 的宾语分布一致）：先抽 zipf 序号再取模 */
            double u = rnd01();
            long idx = (long)(pow((double)n_word, 1.0 - u) - 1.0);  /* 越小越常见 */
            if (idx < 0) idx = 0;
            if (idx >= n_word) idx = n_word - 1;
            b = (uint32_t)idx;
            if (b == (uint32_t)i) continue;                          /* 自环不收 */
            if (!idset_add(&set, b)) continue;                       /* 同一对只留一次 */
            uint16_t rel = pick_rel();
            put_u32(f, b);
            fwrite(&rel, 2, 1, f);
            { uint8_t dyn = (rel == REL_DYN) ? 1u : 0u, pad = 0; fwrite(&dyn, 1, 1, f); fwrite(&pad, 1, 1, f); }
            cnt++;
        }
        free(set.slot); set.slot = NULL; set.cap = 0;
        /* 回填索引 */
        long save = ftell(f);
        if (fseek(f, (long)(C7B_HEAD + (uint64_t)seg * C7B_IDX), SEEK_SET) != 0) { fclose(f); free(deg); return 1; }
        put_u32(f, (uint32_t)i);
        put_u64(f, rec_off);
        put_u32(f, cnt);
        if (fseek(f, save, SEEK_SET) != 0) { fclose(f); free(deg); return 1; }
        rec_off += (uint64_t)cnt * C7B_REC;
        seg++;
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); free(deg); return 1; }
    fclose(f);
    fprintf(stderr, "分片 %ld：词 %ld / 有边的词 %ld / 边 %ld / 段结束偏移 %llu（每边 %u 字节）\n",
            shard, n_word, nseg, sum, (unsigned long long)rec_off, C7B_REC);
    free(deg);
    return 0;
}

/* ---------- 校验 + 抽查（验收用；只读索引，看一个词才读那一段） ---------- */
static int cmd_probe(const char *path, const char *word) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "读不开 %s\n", path); return 1; }
    char m[4];
    if (fread(m, 1, 4, f) != 4 || m[0] != C7B_MAGIC0 || m[1] != C7B_MAGIC1 || m[2] != C7B_MAGIC2 || m[3] != C7B_MAGIC3) {
        fprintf(stderr, "%s：不是 C7B 文件\n", path); fclose(f); return 1;
    }
    uint32_t ver, n_word, nseg, rel_tail, rsv;
    uint64_t n_edges, payload, seed;
    if (fread(&ver, 4, 1, f) != 1 || fread(&n_word, 4, 1, f) != 1 || fread(&nseg, 4, 1, f) != 1 ||
        fread(&n_edges, 8, 1, f) != 1 || fread(&payload, 8, 1, f) != 1 || fread(&seed, 8, 1, f) != 1 ||
        fread(&rel_tail, 4, 1, f) != 1 || fread(&rsv, 4, 1, f) != 1) {
        fprintf(stderr, "%s：头读不全\n", path); fclose(f); return 1;
    }
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell(f);
    uint64_t want = payload + n_edges * C7B_REC;
    fseek(f, (long)C7B_HEAD, SEEK_SET);          /* ⚠ 回到索引开头：上面 seek(END) 之后忘了回来 */

    uint64_t sum = 0; uint32_t maxdeg = 0, nzero = 0;
    /* 索引按块读（40 万条 × 16 B = 6.4 MB）：逐条 fread 太慢（§6.711 实测 400k 次调用 ~0.3 s/片）*/
    {
        enum { CHUNK = 4096 };
        uint8_t *buf = (uint8_t *)malloc((size_t)CHUNK * C7B_IDX);
        if (!buf) { fprintf(stderr, "内存不够（读索引）\n"); fclose(f); return 1; }
        uint32_t left = nseg;
        while (left > 0) {
            uint32_t take = left < CHUNK ? left : CHUNK;
            size_t got = fread(buf, C7B_IDX, take, f);
            if (got != take) { fprintf(stderr, "索引读不全（剩 %u 条）\n", left); free(buf); fclose(f); return 1; }
            for (uint32_t k = 0; k < take; ++k) {
                uint32_t cnt;
                memcpy(&cnt, buf + (size_t)k * C7B_IDX + 12, 4);
                if (cnt == 0) nzero++;
                sum += cnt;
                if (cnt > maxdeg) maxdeg = cnt;
            }
            left -= take;
        }
        free(buf);
    }
    printf("%s\n  version=%u 词=%u 有边词=%u 边=%llu（索引合计 %llu）%s\n",
           path, ver, n_word, nseg, (unsigned long long)n_edges, (unsigned long long)sum,
           (sum == n_edges) ? " ✔" : " ✘ 索引与头不符");
    printf("  文件 %llu 字节（期望 %llu）%s；每边 %u 字节；种子 %llu\n",
           (unsigned long long)size, (unsigned long long)want, (size == want) ? " ✔" : " ✘ 长度不符",
           C7B_REC, (unsigned long long)seed);
    printf("  最大出度 %u；空段 %u\n", maxdeg, nzero);
    if (!word) { fclose(f); return (sum == n_edges && size == want) ? 0 : 1; }

    /* 查一个词：先按名字算 id（w%06d），再在索引里二分/线性找 */
    long wid = -1;
    if (sscanf(word, "w%ld", &wid) != 1 || wid < 0 || (uint32_t)wid >= n_word) {
        fprintf(stderr, "词名只认 w%%06d 形式（给的是 %s）\n", word); fclose(f); return 2;
    }
    uint32_t id = (uint32_t)wid, cnt = 0; uint64_t off = 0;
    {   /* 索引按 id 升序 ⇒ 二分（线性扫 40 万条太慢）*/
        uint32_t lo = 0, hi = nseg;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t rid = 0, rc = 0; uint64_t ro = 0;
            fseek(f, (long)(C7B_HEAD + (uint64_t)mid * C7B_IDX), SEEK_SET);
            if (fread(&rid, 4, 1, f) != 1 || fread(&ro, 8, 1, f) != 1 || fread(&rc, 4, 1, f) != 1) break;
            if (rid == id) { cnt = rc; off = ro; break; }
            if (rid < id) lo = mid + 1; else hi = mid;
        }
    }
    if (cnt == 0) { printf("  %s：这个分片里没有边\n", word); fclose(f); return 0; }
    static const char *RELNAME[12] = { "?", "属于", "有", "会", "怕", "喜欢", "?", "在", "", "", "", "合成关系" };
    /* 整段读回来：既打前 8 条，也**数重复**（写入器承诺"同一对只写一次" —— 要能被验到） */
    C7BEdge *all = (C7BEdge *)malloc((size_t)cnt * sizeof(C7BEdge));
    if (!all) { fprintf(stderr, "内存不够（读段）\n"); fclose(f); return 1; }
    fseek(f, (long)off, SEEK_SET);
    if (fread(all, C7B_REC, cnt, f) != cnt) { fprintf(stderr, "段读不全\n"); free(all); fclose(f); return 1; }
    uint32_t dup = 0;
    for (uint32_t k = 0; k < cnt; ++k)
        for (uint32_t m = k + 1; m < cnt; ++m)
            if (all[k].b_id == all[m].b_id) { dup++; }
    printf("  %s 的边 %u 条（重复 %u）；前 8 条：", word, cnt, dup);
    for (uint32_t k = 0; k < cnt && k < 8; ++k)
        printf(" w%06u --[%s]--> ", all[k].b_id, (all[k].rel < 12 && RELNAME[all[k].rel][0]) ? RELNAME[all[k].rel] : "动态");
    printf("\n");
    free(all);
    fclose(f);
    return (dup == 0) ? 0 : 3;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--probe")) {
        return cmd_probe(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (argc < 5) {
        fprintf(stderr, "用法: %s <输出.c7b> <词数> <边数> <分片号> [种子] [出度上限]\n", argv[0]);
        fprintf(stderr, "      %s --probe <文件.c7b> [w000042]\n", argv[0]);
        return 2;
    }
    const char *out = argv[1];
    long n_word = strtol(argv[2], NULL, 10);
    long n_edges = strtol(argv[3], NULL, 10);
    long shard = strtol(argv[4], NULL, 10);
    unsigned long long seed = (argc > 5) ? strtoull(argv[5], NULL, 10) : 7ULL;
    long cap = (argc > 6) ? strtol(argv[6], NULL, 10) : 2000;
    g_seed = seed * 1000003ULL + (unsigned long long)shard;
    return cmd_make(out, n_word, n_edges, shard, g_seed, cap);
}
