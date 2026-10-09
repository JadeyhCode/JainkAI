/* tools/loadcheck.c — 载入 model.bin 后做一致性自检，用来定位存取问题 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s model.bin\n", argv[0]); return 2; }
    AI *ai = ai_load_file(argv[1]);
    if (!ai) { fprintf(stderr, "加载失败\n"); return 1; }
    printf("sizeof(AssocEntry)=%zu TOKEN_CTX_MAX=%d\n", sizeof(AssocEntry), TOKEN_CTX_MAX);
    printf("tokens=%zu tick=%llu\n", ai->pool->count,
           (unsigned long long)ai->pool->current_tick);
    size_t bad = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        if (t->count > t->cap || (t->count && !t->assocs)) {
            printf("BAD i=%zu '%s' count=%zu cap=%zu assocs=%p\n", i,
                   t->name ? t->name : "(null)", t->count, t->cap, (void *)t->assocs);
            bad++;
        }
    }
    printf("坏 token 数 = %zu\n", bad);
    /* 单个 token 的追加测试：不存在的目标，必然走到 append 分支 */
    Token *cat = pool_token_intern(ai->pool, "猫");
    Token *probe = pool_token_intern(ai->pool, "[探针:测试]");
    printf("cat count=%zu cap=%zu assocs=%p probe=%p\n",
           cat->count, cat->cap, (void *)cat->assocs, (void *)probe);
    fflush(stdout);
    int rc = token_add_assoc_unique(cat, probe);
    printf("append rc=%d 之后 count=%zu cap=%zu assocs=%p\n",
           rc, cat->count, cat->cap, (void *)cat->assocs);
    ai_free(ai);
    return bad ? 1 : 0;
}
