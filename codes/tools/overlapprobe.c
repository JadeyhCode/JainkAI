/* tools/overlapprobe.c — 量两个词的「出邻居重叠度」（共现固化判据的实测入口）
 *
 * 共现固化（micro.h 的 D1）的判据是图内证据，不是「一起出现过」：
 *   两个词的出邻居里共同邻居 ≥4，且 Jaccard ≥ 30%，才会把那条边写下来。
 * 这个工具把两个词的邻居数、共同邻居数、Jaccard、以及两个方向有没有无语境边
 * 直接打出来 —— 想知道「某条边为什么会被/不会被固化」，跑它就知道。
 *
 * 用法：gcc -std=c11 -O2 -o build_overlapprobe tools/overlapprobe.c -lm
 *       ./build_overlapprobe [模型文件] 词1 词2 [词3 …]
 *       默认模型文件 model.bin（需要在 frames.txt / relations.txt 所在目录跑，
 *       否则加载器读不到帧库与关系库）
 */
#include "../ai.h"
#include "../frame_io.h"
#include "../model_io.h"

#include <stdio.h>

static Token *find(AI *ai, const char *w) { return pool_find(ai->pool, w); }

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : "model.bin";
    if (argc < 3) {
        printf("用法：%s [模型文件] 词1 词2 [词3 …]\n", argv[0]);
        return 2;
    }
    AI *ai = ai_load_file(model);
    if (!ai) { printf("加载失败：%s\n", model); return 1; }

    Token *ts[32];
    size_t m = 0;
    for (int i = 2; i < argc && m < 32; ++i) {
        Token *t = find(ai, argv[i]);
        if (!t) { printf("%s：不在池里\n", argv[i]); continue; }
        ts[m++] = t;
        printf("%-10s 出边=%zu  空缺感=%.2f\n", argv[i], t->count, (double)token_gap(t));
    }
    printf("\n（邻居按前 64 条算 —— 边是按 hits 排过序的，所以取的是「最常走的那些邻居」）\n");
    for (size_t i = 0; i < m; ++i) {
        for (size_t j = i + 1; j < m; ++j) {
            Token *na[64], *nb[64];
            size_t ca = micro_neighbors(ts[i], na, 64);
            size_t cb = micro_neighbors(ts[j], nb, 64);
            size_t common = 0;
            for (size_t x = 0; x < ca; ++x)
                for (size_t y = 0; y < cb; ++y)
                    if (na[x] == nb[y]) { common++; break; }
            size_t uni = ca + cb - common;
            printf("%-8s ~ %-8s 邻居 %zu/%zu 共同 %zu  Jaccard %.3f  无语境边 %d/%d  会固化=%s\n",
                   ts[i]->name ? ts[i]->name : "?", ts[j]->name ? ts[j]->name : "?",
                   ca, cb, common, uni ? (double)common / (double)uni : 0.0,
                   micro_has_edge(ts[i], ts[j]), micro_has_edge(ts[j], ts[i]),
                   micro_jaccard_at_least(ts[i], ts[j], MICRO_D1_MIN_COMMON) ? "会" : "不会");
        }
    }
    ai_free(ai);
    return 0;
}
