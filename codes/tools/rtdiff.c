/* tools/rtdiff.c — 找出「存盘再读之后哪条边/哪个符号节点变了」
 *
 * 用法: ./build_rtdiff model.bin
 * 内部：load → 快照 → save → load → 快照 → 逐条比对。
 * 指纹只能告诉你「edge 维度变了」，这个工具告诉你**是哪一条**。
 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>

typedef struct {
    char name[64];
    char main[64];
    uint32_t hits, seq;
    uint8_t mem_state, role, topic, pattern, rel;
    uint64_t birth, touch;
} EdgeSnap;

static size_t snap(AI *ai, EdgeSnap **out) {
    size_t cap = 0, n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->name) continue;
        cap += t->count;
    }
    EdgeSnap *E = (EdgeSnap *)calloc(cap ? cap : 1, sizeof *E);
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->name) continue;
        for (size_t j = 0; j < t->count; ++j) {
            const AssocEntry *e = &t->assocs[j];
            EdgeSnap *s = &E[n++];
            snprintf(s->name, sizeof s->name, "%s", t->name);
            snprintf(s->main, sizeof s->main, "%s",
                     e->main && e->main->name ? e->main->name : "?");
            s->hits = e->hits; s->seq = e->seq;
            s->mem_state = e->mem_state; s->role = e->role;
            s->topic = e->topic; s->pattern = e->pattern; s->rel = e->rel;
            s->birth = e->birth_tick; s->touch = e->last_touch_tick;
        }
    }
    *out = E;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("用法: %s model.bin\n", argv[0]); return 1; }
    AI *a1 = ai_load_file(argv[1]);
    if (!a1) { printf("load fail\n"); return 1; }
    EdgeSnap *e1 = NULL; size_t n1 = snap(a1, &e1);
    if (ai_save_file(a1, "_rtdiff.bin") == 0) { printf("save fail\n"); return 1; }
    ai_free(a1);
    AI *a2 = ai_load_file("_rtdiff.bin");
    if (!a2) { printf("reload fail\n"); return 1; }
    EdgeSnap *e2 = NULL; size_t n2 = snap(a2, &e2);

    printf("边数: 第一次 %zu  第二次 %zu\n", n1, n2);
    size_t shown = 0;
    for (size_t i = 0; i < n1 && i < n2 && shown < 12; ++i) {
        if (memcmp(&e1[i], &e2[i], sizeof e1[i]) == 0) continue;
        printf("  [%zu] %s -> %s\n", i, e1[i].name, e1[i].main);
        printf("        前: hits=%u seq=%u state=%u role=%u topic=%u pat=%u rel=%u birth=%llu touch=%llu\n",
               e1[i].hits, e1[i].seq, e1[i].mem_state, e1[i].role, e1[i].topic,
               e1[i].pattern, e1[i].rel,
               (unsigned long long)e1[i].birth, (unsigned long long)e1[i].touch);
        printf("        后: hits=%u seq=%u state=%u role=%u topic=%u pat=%u rel=%u birth=%llu touch=%llu\n",
               e2[i].hits, e2[i].seq, e2[i].mem_state, e2[i].role, e2[i].topic,
               e2[i].pattern, e2[i].rel,
               (unsigned long long)e2[i].birth, (unsigned long long)e2[i].touch);
        shown++;
    }
    if (!shown) printf("  逐条一致（那差异在符号节点层，不在边层）\n");
    free(e1); free(e2); ai_free(a2);
    return 0;
}
