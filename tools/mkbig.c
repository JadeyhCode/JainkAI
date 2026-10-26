/* tools/mkbig.c — **合成大图生成器**（阶段二 WP0；§6.632）
 *
 * 目的：**只压测"逻辑/索引"在规模上的表现**，不灌语义、不进仓库资产。
 * 造出来的是：N 个合成词（`w000001`…）+ 若干关系 + M 条随机事实（走 `ai_write_rel` 正规通道）。
 *
 * 为什么不用真语料：① 真语料会把"图扩张"偷偷带进来（用户明确说阶段二不扩图）；
 * ② 合成图**可复现**（固定种子），换机器也能对比；
 * ③ 只关心"查询/证明在多大的图上还撑得住"，不关心"词有没有意义"。
 *
 * 用法：
 *   gcc -std=c11 -O2 -o build_mkbig tools/mkbig.c -lm
 *   ./build_mkbig <输出.bin> <词数> <事实数> [种子]
 *   例：./build_mkbig /tmp/big_M.bin 50000 2000000 7
 *
 * ⚠ 产物只放 /tmp 或 _t/ 下，别提交；它不是模型，是标尺。
 */
#include "../agent.h"      /* 先 agent.h：它把 frame_io/relations 这些 loading 入口带进来 */
#include "../model_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long g_seed;
static unsigned rnd(unsigned n) {
    g_seed = g_seed * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)((g_seed >> 33) % (n ? n : 1));
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "用法: %s <输出.bin> <词数> <事实数> [种子]\n", argv[0]);
        return 2;
    }
    const char *out = argv[1];
    long n_word = strtol(argv[2], NULL, 10);
    long n_fact = strtol(argv[3], NULL, 10);
    g_seed = (argc > 4) ? (unsigned long)strtoul(argv[4], NULL, 10) : 1UL;
    if (n_word < 100 || n_word > 5000000L) { fprintf(stderr, "词数范围 100..5e6\n"); return 2; }
    if (n_fact < 0 || n_fact > 100000000L) { fprintf(stderr, "事实数范围 0..1e8\n"); return 2; }

    AI *a = ai_new();
    if (!a) { fprintf(stderr, "建脑失败\n"); return 1; }

    clock_t t0 = clock();
    /* ① 词：w000001…（定长，便于排序/二分；合成词不进真语料） */
    char nm[32];
    for (long i = 0; i < n_word; ++i) {
        snprintf(nm, sizeof nm, "w%06ld", i);
        if (!pool_token_intern(a->pool, nm)) { fprintf(stderr, "造词失败 @%ld\n", i); ai_free(a); return 1; }
    }
    /* ② 关系：用内核已有的核心关系（属于/会/有/在…）+ 一个动态关系，覆盖两条存储路径 */
    static const uint8_t RIDS[] = { REL_IS_A, REL_CAN, REL_HAS, REL_AT, REL_LIKE, REL_FEAR };
    uint8_t dyn = rel_id_of(a, "合成关系", 1);
    double t_word = (double)(clock() - t0) / CLOCKS_PER_SEC;

    /* ③ 事实：随机 (主语, 关系, 宾语)，十个里一个用动态关系 */
    t0 = clock();
    long made = 0;
    for (long k = 0; k < n_fact; ++k) {
        long ia = (long)rnd((unsigned)n_word);
        long ib = (long)rnd((unsigned)n_word);
        if (ia == ib) continue;
        char sa[16], sb[16];
        snprintf(sa, sizeof sa, "w%06ld", ia);
        snprintf(sb, sizeof sb, "w%06ld", ib);
        Token *A = pool_find(a->pool, sa), *B = pool_find(a->pool, sb);
        if (!A || !B) continue;
        uint8_t rid = (rnd(10) == 0) ? dyn : RIDS[rnd((unsigned)(sizeof RIDS / sizeof RIDS[0]))];
        if (rid && ai_write_rel(a, A, B, rid, 0, AI_WSRC_RELATION) == 0) made++;
    }
    double t_fact = (double)(clock() - t0) / CLOCKS_PER_SEC;

    t0 = clock();
    size_t sz = ai_save_file(a, out);
    double t_save = (double)(clock() - t0) / CLOCKS_PER_SEC;

    printf("合成图：词 %ld / 事实 %ld（写进 %ld 条）/ 文件 %.1f MB\n",
           n_word, n_fact, made, (double)sz / (1024.0 * 1024.0));
    printf("耗时：造词 %.2fs / 写边 %.2fs / 存盘 %.2fs\n", t_word, t_fact, t_save);
    printf("存档：%s\n", out);
    ai_free(a);
    return sz ? 0 : 1;
}
