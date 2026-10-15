/* tools/rttok.c — 比较两个模型文件的 **符号节点层**（存读往返差异定位）
 * 用法: ./build_rttok A.bin B.bin */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) { printf("用法: %s A.bin B.bin\n", argv[0]); return 1; }
    AI *a = ai_load_file(argv[1]);
    AI *b = ai_load_file(argv[2]);
    if (!a || !b) { printf("load fail\n"); return 1; }
    printf("符号节点数: %zu vs %zu\n", a->pool->count, b->pool->count);
    size_t shown = 0;
    for (size_t i = 0; i < a->pool->count && i < b->pool->count && shown < 10; ++i) {
        Token *x = a->pool->tokens[i], *y = b->pool->tokens[i];
        const char *xn = x->name ? x->name : "(null)";
        const char *yn = y->name ? y->name : "(null)";
        if (x->count != y->count || x->cap != y->cap ||
            x->next_seq != y->next_seq || x->birth_tick != y->birth_tick ||
            x->last_touch_tick != y->last_touch_tick ||
            x->misses != y->misses || x->recent_misses != y->recent_misses ||
            x->mem_state != y->mem_state || x->is_garbage != y->is_garbage ||
            x->parent != y->parent || x->next_in_time != y->next_in_time ||
            strcmp(xn, yn) != 0) {
            printf("  [%zu] %s vs %s\n", i, xn, yn);
            printf("      count %zu/%zu  cap %zu/%zu  next_seq %u/%u  birth %llu/%llu  touch %llu/%llu\n",
                   x->count, y->count, x->cap, y->cap, x->next_seq, y->next_seq,
                   (unsigned long long)x->birth_tick, (unsigned long long)y->birth_tick,
                   (unsigned long long)x->last_touch_tick, (unsigned long long)y->last_touch_tick);
            printf("      miss %u/%u recent %u/%u state %u/%u garbage %u/%u parent %s/%s\n",
                   x->misses, y->misses, x->recent_misses, y->recent_misses,
                   x->mem_state, y->mem_state, x->is_garbage, y->is_garbage,
                   x->parent && x->parent->name ? x->parent->name : "-",
                   y->parent && y->parent->name ? y->parent->name : "-");
            shown++;
        }
    }
    if (!shown) printf("  符号节点层逐条一致\n");
    /* 池级标量 */
    printf("池: tick %llu/%llu  n_entries %zu/%zu\n",
           (unsigned long long)a->pool->current_tick, (unsigned long long)b->pool->current_tick,
           a->pool->n_entries, b->pool->n_entries);
    ai_free(a); ai_free(b);
    return 0;
}
