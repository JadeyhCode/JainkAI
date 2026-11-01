/* tools/mkcorpus.c — **合成语料扩图器**（阶段三 M1；§6.683）
 *
 * 和 `tools/mkbig.c` 的区别（为什么另起一支，而不是改 mkbig）：
 *   · `mkbig` 是阶段二的门槛尺子：**均匀**分布、只用核心关系 —— 它量的是"引擎在理想图上"，
 *     所以它的数字**偏乐观**（`docs/规模门槛.md` 第五节自己点过这条）；
 *   · 这一支要量"真语料的形状"：**幂律度分布**（少数枢纽词吃掉大部分边）+ **长尾关系**
 *     （`属于` 独占大半）。剪枝、索引、缓存这些优化在均匀图上看着没用，在幂律图上才见真章。
 *
 * 三种分布（都用现成的 `ai_write_rel` 通道写，**不碰存储结构**）：
 *   uniform —— 主宾都均匀（等价于 mkbig，当对照）
 *   zipf    —— 主宾都按 Zipf(α=1.2) 抽（真图的尾巴）
 *   star    —— 主语均匀、宾语 Zipf（"很多事实都指向少数实体"的星形）
 * 关系分布：`uniform` = 6 个核心关系平均 + 10% 动态关系；
 *           `tail`    = 属于 80% / 会 8% / 有 6% / 在 4% / 喜欢 1% / 怕 0.5% / 动态 0.5%。
 *
 * 用法：
 *   gcc -std=c11 -O2 -o build_mkcorpus tools/mkcorpus.c -lm
 *   ./build_mkcorpus <输出.bin> <词数> <边数> [分布] [关系分布] [种子]
 *   例：./build_mkcorpus /tmp/k10.bin 200000 10000000 zipf tail 7
 *
 * ⚠ 产物只放 `_t/` 或 `/tmp`，不进仓库；它**不是知识**，是标尺。
 * ⚠ 重复边会被内核合并（`ai_write_rel` 合并计数），所以输出里同时报
 *   "请求边数" 与 "真写进去多少" —— 这两个数差太多就说明分布造得不对（自欺）。
 */
#include "../agent.h"
#include "../model_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long g_seed;
static unsigned long long nextr(void) {           /* splitmix64：可复现 */
    g_seed += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = g_seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* 说明：这里**不用**"随机抽样"造 Zipf（第一版就是那样，10^7 次抽样 78% 撞成重复边，
 * 见下面的教训）。现在只保留**权重函数**：第 i 个词的权重 w_i ∝ 1/(i+1)^1.2，
 * 再按"目标出度"分配到每个词头上。 */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "用法: %s <输出.bin> <词数> <边数> [uniform|zipf|star] [uniform|tail] [种子] [出度上限]\n", argv[0]);
        return 2;
    }
    const char *out = argv[1];
    long n_word = strtol(argv[2], NULL, 10);
    long n_fact = strtol(argv[3], NULL, 10);
    const char *dist = (argc > 4) ? argv[4] : "zipf";
    const char *rdist = (argc > 5) ? argv[5] : "tail";
    g_seed = (argc > 6) ? strtoull(argv[6], NULL, 10) : 7ULL;
    /* 出度上限（第 7 个参数）：**不是审美，是写入路径的现实**。
     * `ai_write_rel` 每写一条边要扫一遍该词已有的边（O(出度)）⇒ 造图的代价是 Σ deg²/2，
     * 出度不设上限时 5 万出度的枢纽会把 10^7 边的生成拖成小时级（§6.683 实测：817k 边 20 s）。
     * 所以生成器先带上限，把"这个墙"记清楚，等 M2 把写入路径修好再放开。 */
    long deg_cap = (argc > 7) ? strtol(argv[7], NULL, 10) : 2000;
    unsigned long long seed0 = g_seed;   /* 打印用原来的：g_seed 每抽一次就变（第一版打出来是乱的） */
    if (n_word < 100 || n_word > 5000000L) { fprintf(stderr, "词数范围 100..5e6\n"); return 2; }
    if (n_fact < 0 || n_fact > 100000000L) { fprintf(stderr, "边数范围 0..1e8\n"); return 2; }
    int use_uniform = !strcmp(dist, "uniform");
    int use_zipf = !strcmp(dist, "zipf");
    int use_star = !strcmp(dist, "star");
    if (!use_uniform && !use_zipf && !use_star) {
        fprintf(stderr, "分布只认 uniform|zipf|star\n");
        return 2;
    }
    int rel_tail = !strcmp(rdist, "tail");

    AI *a = ai_new();
    if (!a) { fprintf(stderr, "建脑失败\n"); return 1; }

    clock_t t0 = clock();
    char nm[32];
    for (long i = 0; i < n_word; ++i) {
        snprintf(nm, sizeof nm, "w%06ld", i);
        if (!pool_token_intern(a->pool, nm)) { fprintf(stderr, "造词失败 @%ld\n", i); ai_free(a); return 1; }
    }
    double t_word = (double)(clock() - t0) / CLOCKS_PER_SEC;

    static const uint8_t RID_CORE[] = { REL_IS_A, REL_CAN, REL_HAS, REL_AT, REL_LIKE, REL_FEAR };
    uint8_t dyn = rel_id_of(a, "合成关系", 1);

    /* 度数计数器：只用来**证明分布真的造出来了**（报 top-10 + 平均出度） */
    long *deg = (long *)calloc((size_t)n_word, sizeof(long));

    t0 = clock();
    long made = 0, dup = 0, self_loop = 0;

    if (use_uniform) {
        /* 均匀模式：保持和 mkbig 一样的语义（随机对，重复靠内核合并）—— 它是对照组 */
        for (long k = 0; k < n_fact; ++k) {
            long ia = (long)(nextr() % (unsigned long long)n_word);
            long ib = (long)(nextr() % (unsigned long long)n_word);
            if (ia == ib) { self_loop++; continue; }
            char sa[16], sb[16];
            snprintf(sa, sizeof sa, "w%06ld", ia);
            snprintf(sb, sizeof sb, "w%06ld", ib);
            Token *A = pool_find(a->pool, sa), *B = pool_find(a->pool, sb);
            if (!A || !B) continue;
            uint8_t rid = (nextr() % 10u == 0) ? dyn : RID_CORE[nextr() % 6u];
            if (rid && ai_write_rel(a, A, B, rid, 0, AI_WSRC_RELATION) == 0) { made++; deg[ia]++; }
            else dup++;
        }
    } else {
        /* 幂律模式：**按目标出度造边，宾语各不相同**（不是"随机抽对，撞了就算了"）。
         *
         * 为什么必须这样（§6.683 的教训）：第一版主宾都从 Zipf 里抽，10^7 次抽样里
         * **78% 撞成了重复边**（枢纽词反复连同一批宾语），文件里只剩 222 万条 ——
         * 报"10^7 边"却只有 2.2×10^6，等于自己骗自己。生成器最后那行"真写进去多少"
         * 就是为了让这种自欺当场露馅。
         *
         * 现在的做法：每个词按 Zipf 分到一个**目标出度**，然后从一个随机起点开始沿词表环形
         * 走 `出度` 步（跳过自己）—— 一个词的宾语**天然各不相同**，边数 == 目标出度之和。
         */
        double *want = (double *)malloc(sizeof(double) * (size_t)n_word);
        if (!want) { fprintf(stderr, "度数表分配失败\n"); ai_free(a); return 1; }
        double tot = 0.0;
        double *w0 = (double *)malloc(sizeof(double) * (size_t)n_word);   /* 原权重（水填按它补） */
        if (!w0) { fprintf(stderr, "权重表分配失败\n"); free(want); ai_free(a); return 1; }
        for (long i = 0; i < n_word; ++i) {
            double p = (use_zipf) ? 1.0 / pow((double)(i + 1), 1.2) : 1.0;
            want[i] = p;
            w0[i] = p;
            tot += p;
        }
        long budget = n_fact;
        long cap = n_word - 1;                     /* 硬上限：一个词最多能有多少个不同宾语 */
        if (deg_cap > 0 && deg_cap < cap) cap = deg_cap;   /* 现实上限：写入路径撑不住更高 */
        long assign = 0;
        for (long i = 0; i < n_word; ++i) {
            double d = want[i] / tot * (double)n_fact;
            long n = (long)(d + 0.5);
            if (n < 1) n = 1;
            if (n > cap) n = cap;                      /* 上限砍掉的量**要补给别人** */
            if (assign + n > budget) n = budget - assign;
            if (n <= 0) continue;
            want[i] = (double)n;
            assign += n;
            if (assign >= budget) break;
        }
        /* 水填：上限砍掉的额度**按原权重**补给还没到上限的词。
         * 两个必须：① 不补的话边数会少一大截（第一版少了 55%）；
         * ② 按权重补（不是均摊）—— 均摊会把幂律形状抹平成"大家差不多"，
         *   那样这个图就不再是"真语料的形状"，M2 的剪枝/索引优化就测不出意义。 */
        int pass = 0;
        while (assign < budget && pass < 32) {
            double wsum = 0.0;
            for (long i = 0; i < n_word; ++i) if ((long)want[i] < cap) wsum += w0[i];
            if (wsum <= 0.0) break;
            long remain = budget - assign;
            long did = 0;
            for (long i = 0; i < n_word && assign < budget; ++i) {
                long cur = (long)want[i];
                if (cur >= cap) continue;
                long add = (long)((double)remain * w0[i] / wsum);
                if (add < 1) add = 1;
                if (cur + add > cap) add = cap - cur;
                if (assign + add > budget) add = budget - assign;
                want[i] = (double)(cur + add);
                assign += add;
                did += add;
            }
            if (did == 0) break;
            pass++;
        }
        for (long i = 0; i < n_word && made < budget; ++i) {
            long n = (long)want[i];
            if (n <= 0) continue;
            char sa[16];
            snprintf(sa, sizeof sa, "w%06ld", i);
            Token *A = pool_find(a->pool, sa);
            if (!A) continue;
            long start = (long)(nextr() % (unsigned long long)n_word);
            long got = 0;
            for (long k = 0; k < n && made < budget; ++k) {
                long j = (start + k) % n_word;
                if (j == i) { self_loop++; continue; }
                char sb[16];
                snprintf(sb, sizeof sb, "w%06ld", j);
                Token *B = pool_find(a->pool, sb);
                if (!B) continue;
                uint8_t rid;
                if (rel_tail) {
                    unsigned r = (unsigned)(nextr() % 1000u);
                    if (r < 800) rid = REL_IS_A;
                    else if (r < 880) rid = REL_CAN;
                    else if (r < 940) rid = REL_HAS;
                    else if (r < 980) rid = REL_AT;
                    else if (r < 990) rid = REL_LIKE;
                    else if (r < 995) rid = REL_FEAR;
                    else rid = dyn;
                } else {
                    rid = (nextr() % 10u == 0) ? dyn : RID_CORE[nextr() % 6u];
                }
                if (rid && ai_write_rel(a, A, B, rid, 0, AI_WSRC_RELATION) == 0) { made++; deg[i]++; got++; }
                else dup++;
            }
            (void)got;
        }
        free(w0);
        free(want);
    }
    double t_fact = (double)(clock() - t0) / CLOCKS_PER_SEC;

    t0 = clock();
    size_t sz = ai_save_file(a, out);
    double t_save = (double)(clock() - t0) / CLOCKS_PER_SEC;

    /* top-10 度：幂律的当场凭据（均匀分布不会出现这种形状） */
    long top[10] = { 0 };
    for (long i = 0; i < n_word; ++i) {
        for (int j = 0; j < 10; ++j) {
            if (deg[i] > top[j]) {
                for (int q = 9; q > j; --q) top[q] = top[q - 1];
                top[j] = deg[i];
                break;
            }
        }
    }
    long sum = 0;
    for (long i = 0; i < n_word; ++i) sum += deg[i];
    printf("合成语料图：词 %ld / 请求边 %ld / 真写进去 %ld（重复 %ld / 自环 %ld，合计丢了 %.1f%%）\n",
           n_word, n_fact, made, dup, self_loop,
           n_fact ? 100.0 * (double)(dup + self_loop) / (double)n_fact : 0.0);
    printf("分布：%s（α=1.2，出度上限 %ld）/ 关系：%s / 种子 %llu\n",
           dist, (use_uniform ? (long)0 : deg_cap), rel_tail ? "tail" : "uniform", seed0);
    printf("耗时：造词 %.2fs / 写边 %.2fs / 存盘 %.2fs；文件 %.1f MB\n",
           t_word, t_fact, t_save, (double)sz / (1024.0 * 1024.0));
    printf("度数 top-10：");
    for (int j = 0; j < 10; ++j) printf(" %ld", top[j]);
    {
        long *cp = (long *)malloc(sizeof(long) * (size_t)n_word);
        if (cp) {
            memcpy(cp, deg, sizeof(long) * (size_t)n_word);
            /* 简单插入排序太慢：只做 p50/p90/p99/最大，用"计数直方图 + 前缀"近似 */
            long mx = top[0];
            size_t nb = (size_t)(mx / 64 + 2);
            long *hist = (long *)calloc(nb, sizeof(long));
            if (hist) {
                for (long i = 0; i < n_word; ++i) {
                    size_t b = (size_t)(deg[i] / 64);
                    if (b >= nb) b = nb - 1;
                    hist[b]++;
                }
                printf("\n出度分位（近似，按 64 一档）：");
                const double qs[3] = { 0.5, 0.9, 0.99 };
                for (int qi = 0; qi < 3; ++qi) {
                    long target = (long)(qs[qi] * (double)n_word);
                    long acc2 = 0;
                    for (size_t b = 0; b < nb; ++b) {
                        acc2 += hist[b];
                        if (acc2 >= target) { printf(" p%.0f≈%zu", qs[qi] * 100, b * 64); break; }
                    }
                }
                printf(" max=%ld", mx);
                free(hist);
            }
            free(cp);
        }
    }
    printf("\n平均度 %.1f（= 边数/词数）；top-10 合计占边数 %.1f%%；top1 %.1f%%\n",
           n_word ? (double)sum / (double)n_word : 0.0,
           sum ? 100.0 * (double)(top[0] + top[1] + top[2] + top[3] + top[4] +
                                  top[5] + top[6] + top[7] + top[8] + top[9]) / (double)sum : 0.0,
           sum ? 100.0 * (double)top[0] / (double)sum : 0.0);
    printf("存档：%s\n", out);
    free(deg);
    ai_free(a);
    return sz ? 0 : 1;
}
