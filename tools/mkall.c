/* tools/mkall.c — 把 frames.txt 展开成**可训练的普通句子**
 *
 * 帧库（frames.txt）本身是模板格式（`[帧:]` / `[问]` / `[答]` / `[填:]` / `---`），
 * 直接丢进训练器只会污染图（生出一堆 `[帧:` 之类的标记词）。
 * 这里把每个模板的槽位用声明的填充词实例化，输出成一行一句的普通文本 ——
 * 那样它就能和别的语料一样训练（`token_observe_names` 建共现边）。
 *
 * 用法: ./build_mkall [frames.txt] [每个槽取几个填充词]
 */
#include "../agent.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "frames.txt";
    int K = (argc > 2) ? atoi(argv[2]) : 3;
    if (K < 1) K = 1;
    if (K > 8) K = 8;

    AI *ai = ai_new();
    if (!ai) return 1;
    size_t nf = frames_load(ai, path);
    FrameSet *fs = (FrameSet *)ai->frames;
    if (!fs) { fprintf(stderr, "帧库为空\n"); return 1; }

    for (size_t fi = 0; fi < fs->n_fr; ++fi) {
        Frame *fr = &fs->fr[fi];
        const FrTemplates *Ts[2] = { &fr->ask, &fr->ans };
        for (int t = 0; t < 2; ++t) {
            const FrTemplates *T = Ts[t];
            for (size_t ti = 0; ti < T->n_tmpl; ++ti) {
                /* 收集这条模板用到的槽位 */
                int slots[FR_MAX_SLOT];
                size_t nslot = 0;
                for (size_t k = 0; k < T->n_toks[ti]; ++k) {
                    int si = fr_slot_at(T, ti, k);
                    if (si < 0) continue;
                    int dup = 0;
                    for (size_t q = 0; q < nslot; ++q) if (slots[q] == si) dup = 1;
                    if (!dup && nslot < FR_MAX_SLOT) slots[nslot++] = si;
                }
                /* 逐槽取前 K 个填充词做笛卡尔积（上限 32 行/模板） */
                size_t combos = 1;
                for (size_t q = 0; q < nslot; ++q) {
                    size_t nfill = fr->slot[slots[q]].n_fill;
                    size_t take = nfill < (size_t)K ? nfill : (size_t)K;
                    if (take == 0) { combos = 0; break; }
                    combos *= take;
                    if (combos > 32) combos = 32;
                }
                if (nslot == 0 || combos == 0) {
                    /* 没有槽位：模板本身就是一句完整的话 */
                    FrValue fill[FR_MAX_SLOT];
                    memset(fill, 0, sizeof fill);
                    char *s = fr_render(fr, T, ti, fill);
                    if (s) { printf("%s\n", s); free(s); }
                    continue;
                }
                for (size_t c = 0; c < combos; ++c) {
                    FrValue fill[FR_MAX_SLOT];
                    memset(fill, 0, sizeof fill);
                    size_t rem = c;
                    int ok = 1;
                    for (size_t q = 0; q < nslot; ++q) {
                        size_t nfill = fr->slot[slots[q]].n_fill;
                        size_t take = nfill < (size_t)K ? nfill : (size_t)K;
                        if (take == 0) { ok = 0; break; }
                        size_t pick = rem % take;
                        rem /= take;
                        Token *f = fr->slot[slots[q]].fill[pick];
                        if (!f) { ok = 0; break; }
                        fill[slots[q]].tok[0] = f;
                        fill[slots[q]].n = 1;
                    }
                    if (!ok) continue;
                    char *s = fr_render(fr, T, ti, fill);
                    if (s) { printf("%s\n", s); free(s); }
                }
            }
        }
    }
    fprintf(stderr, "展开帧 %zu 个（每槽前 %d 个填充词）\n", nf, K);
    ai_free(ai);
    return 0;
}
