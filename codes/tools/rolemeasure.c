/* tools/rolemeasure.c — 量化「回复角色」子图的密度：瓶颈在哪一目了然 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
int main(int argc, char **argv) {
    AI *ai = ai_load_file(argv[1]);
    if (!ai) { printf("load fail\n"); return 1; }
    size_t n_rep = 0, out_rep = 0, out_all = 0, dead = 0;
    size_t hist[8] = {0};
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->name || t->name[0] == '[') continue;
        size_t rp = 0;
        for (size_t j = 0; j < t->count; ++j)
            if (t->assocs[j].role == ASSOC_ROLE_REPLY && t->assocs[j].hits > 0) rp++;
        n_rep++; out_rep += rp; out_all += t->count;
        if (rp == 0) dead++;
        if (rp > 7) rp = 7;
        hist[rp]++;
    }
    printf("内容 Token=%zu  平均角色出边=%.2f  平均总出边=%.2f\n",
           n_rep, (double)out_rep / (n_rep ? n_rep : 1),
           (double)out_all / (n_rep ? n_rep : 1));
    printf("没有回复角色出边的 Token = %zu (%.0f%%)\n", dead,
           100.0 * (double)dead / (n_rep ? n_rep : 1));
    printf("角色出边数分布 (0..7+):");
    for (int i = 0; i < 8; ++i) printf(" %zu", hist[i]);
    printf("\n");
    ai_free(ai);
    return 0;
}
