#include "../agent.h"
#include <stdio.h>
int main(int argc, char **argv) {
    AI *ai = ai_new();
    size_t nf = frames_load(ai, argc > 1 ? argv[1] : "frames.txt");
    printf("帧库 %zu 个\n", nf);
    FILE *f = (argc > 2) ? fopen(argv[2], "r") : stdin;
    char line[512]; size_t hit = 0, tot = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
        if (!l || line[0]=='#') continue;
        size_t n; char **w = ai_tokenize(ai, line, &n);
        Token **t = malloc(n*sizeof *t);
        for (size_t k=0;k<n;++k) t[k]=pool_token_intern(ai->pool,w[k]);
        char *r = frames_generate(ai, t, n, NULL);
        tot++;
        if (r) hit++;
        printf("  %-24s → %s\n", line, r ? r : "(未匹配)");
        free(r); free(t); ai_free_words(w,n);
    }
    if (f != stdin) fclose(f);
    printf("帧命中 %zu/%zu\n", hit, tot);
    return 0;
}
