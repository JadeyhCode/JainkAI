#include "../agent.h"
#include <stdio.h>
int main(int argc, char **argv) {
    AI *ai = ai_new();
    size_t nf = frames_load(ai, argc > 1 ? argv[1] : "frames.txt");
    printf("帧库 %zu 个\n", nf);
    const char *tests[] = {
        "你喜欢雪吗","你怕黑吗","你记得月亮吗","你知道量子吗","你想学画画吗",
        "你喜欢猫吗","你怕水吗","你记得我吗","你知道我吗","你想学游泳吗",
        NULL
    };
    for (int i = 0; tests[i]; ++i) {
        size_t n; char **w = ai_tokenize(ai, tests[i], &n);
        Token **t = malloc(n*sizeof *t);
        for (size_t k=0;k<n;++k) t[k]=pool_token_intern(ai->pool,w[k]);
        char *r = frames_generate(ai, t, n, NULL);
        printf("  %-14s → %s\n", tests[i], r ? r : "(未匹配)");
        free(r); free(t); ai_free_words(w,n);
    }
    printf("帧生成次数 %zu\n", ai->n_frames);
    return 0;
}
