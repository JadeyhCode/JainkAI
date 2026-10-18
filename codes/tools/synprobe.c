/* tools/synprobe.c — **同义词能否自动发现**的实测（§6.308）
 *
 * 背景：目标要求"新说法靠数据、不靠改代码"。手写 `[syn]`/`[affix]` 表能覆盖变体，
 * 但**能不能自动扩充**？这里用现成的 emb（PPMI 随机索引）查最近邻，看它认不认得同义词。
 *
 * 结论（出厂模型实测）：**认不出来**。`狗` 的最近邻是 月亮/火/鱼（0.70），不是 犬；
 * 而 `猫咪/小猫/老猫` 这类变体**根本不在词池里**，没有 emb，更谈不上发现。
 * 所以"手写表"是当前数据路线的上限，想自动扩充得先有更好的分布表示（另一件事）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_synprobe tools/synprobe.c -lm
 *       ./build_synprobe [model.bin] */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
int main(int argc, char **argv) {
    AI *ai = ai_load_file(argc > 1 ? argv[1] : "model.bin");
    if (!ai) return 1;
    ai_embed_build(ai);
    const char *qs[] = {"猫","狗","喜欢","害怕","鲸鱼","鱼"};
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; ++i) {
        Token *q = pool_find(ai->pool, qs[i]);
        if (!q || !q->emb) { printf("%s: 不在池/无 emb\n", qs[i]); continue; }
        Token *best[5]; double bs[5];
        for (int k = 0; k < 5; ++k) { best[k] = NULL; bs[k] = -2; }
        for (size_t j = 0; j < ai->pool->count; ++j) {
            Token *t = ai->pool->tokens[j];
            if (!t || t == q || !t->emb || !t->name || t->name[0] == '[') continue;
            double c = emb_cosine(q->emb, t->emb);
            for (int k = 0; k < 5; ++k) if (c > bs[k]) {
                for (int m = 4; m > k; --m) { bs[m] = bs[m-1]; best[m] = best[m-1]; }
                bs[k] = c; best[k] = t; break;
            }
        }
        printf("%s →", qs[i]);
        for (int k = 0; k < 5; ++k) if (best[k]) printf(" %s(%.2f)", best[k]->name, bs[k]);
        printf("\n");
    }
    ai_free(ai);
    return 0;
}
