/* tools/novelprobe.c — 找出「语料里真的没有」的输入，并在这些输入上看合成是否触发 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    AI *ai = ai_load_file(argv[1]);
    FILE *f = fopen(argv[2], "r"); char line[512];
    size_t n_novel = 0, n_syn = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
        if (!l) continue;
        size_t n; char **w = ai_tokenize(ai, line, &n);
        Token **t = malloc(n*sizeof *t);
        for (size_t k=0;k<n;++k) t[k]=pool_token_intern(ai->pool,w[k]);
        /* 语料里是否有字面相同的输入句？ */
        int exact = 0;
        for (size_t i=0;i<ai->pairs_n && !exact;++i) {
            const AISentPair *p=&ai->pairs[i];
            if (p->in_n != n) continue;
            int same=1;
            for (size_t k=0;k<n;++k) if (p->in[k]!=t[k]) { same=0; break; }
            if (same) exact=1;
        }
        if (!exact) {
            n_novel++;
            int ssc=0;
            char *syn = ai_synthesize(ai, t, n, ai_detect_topic(ai,t,n), &ssc);
            printf("  新输入: %-14s 合成:%s\n", line, syn ? syn : "(未触发)");
            if (syn) n_syn++;
            free(syn);
        }
        free(t); ai_free_words(w,n);
    }
    printf("语料里没有的输入 %zu 条，其中合成触发 %zu 条\n", n_novel, n_syn);
    return 0;
}
