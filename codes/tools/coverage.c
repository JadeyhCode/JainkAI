/* tools/coverage.c — 覆盖率探针：<语料里的问法> 有多少能被整句检索命中 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    AI *ai = ai_load_file(argv[1]);
    if (!ai) { printf("load fail\n"); return 1; }
    FILE *f = (argc > 2) ? fopen(argv[2], "r") : stdin;
    if (!f) { perror("open"); return 1; }
    char line[512];
    size_t hit = 0, tot = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (!l) continue;
        size_t n; char **w = ai_tokenize(ai, line, &n);
        Token **toks = (Token **)malloc(n * sizeof *toks);
        for (size_t k = 0; k < n; ++k) toks[k] = pool_token_intern(ai->pool, w[k]);
        uint8_t tp = ai_detect_topic(ai, toks, n);
        int sc = 0;
        ai_sent_lookup(ai, toks, n, tp, &sc, NULL);
        int need = (tp == ASSOC_TOPIC_NONE) ? 85 : 60;
        tot++;
        if (sc >= need) hit++;
        else printf("  MISS [话题%u sc=%d] %s\n", (unsigned)tp, sc, line);
        free(toks); ai_free_words(w, n);
    }
    if (f != stdin) fclose(f);
    printf("覆盖率: %zu/%zu = %.1f%%\n", hit, tot, 100.0 * (double)hit / (double)tot);
    ai_free(ai);
    return 0;
}
