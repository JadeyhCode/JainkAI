/* tools/synthprobe.c — 对一批输入，分别跑「纯检索」和「模板合成」，看合成是否真的产出了新句子 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
int main(int argc, char **argv) {
    AI *ai = ai_load_file(argv[1]);
    FILE *f = (argc > 2) ? fopen(argv[2], "r") : stdin;
    char line[512];
    size_t n_syn = 0, n_tot = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
        if (!l) continue;
        size_t n; char **w = ai_tokenize(ai, line, &n);
        Token **t = malloc(n*sizeof *t);
        for (size_t k=0;k<n;++k) t[k]=pool_token_intern(ai->pool,w[k]);
        uint8_t tp = ai_detect_topic(ai,t,n);
        int sc=0,cov=0;
        const AISentPair *hit = ai_sent_lookup(ai,t,n,tp,&sc,&cov);
        char *plain = hit ? ai_tokens_to_text(hit->out, hit->out_n) : NULL;
        int ssc=0;
        char *syn = ai_synthesize(ai,t,n,tp,&ssc);
        n_tot++;
        if (syn) {
            n_syn++;
            int same = plain && strcmp(plain, syn)==0;
            printf("  %-16s 合成[%s]%s   纯检索[%s]\n", line, syn,
                   same ? " (同)" : " ★新", plain?plain:"(无)");
        }
        free(syn); free(plain);
        free(t); ai_free_words(w,n);
    }
    printf("合成命中: %zu/%zu\n", n_syn, n_tot);
    return 0;
}
