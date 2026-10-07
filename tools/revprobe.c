/* tools/revprobe.c — 量「同一对词、同一条关系上正负极性并存」的边有多少条
 *
 * 用途：这一轮给落观测加了「改口语义」（后说的替掉先说的对立极性，见
 * docs/thinking.md 六点六）。改之前先量了一下**现成模型的存量**：
 *
 *     ./build_aip_probe model.bin
 *     总关系边=167  正负并存=0
 *
 * 结论：存量模型里一条都没有 → 这条改动在**装载语义**上是空转的（既有回归一条不变），
 * 它的价值全在「以后用户改口时」。这也是它必须配 A/B 的原因：
 * `tests/revision.sh` 拿同一段对话跑 on/off 两遍来量差异。
 *
 * 以后动这条语义（比如想改成「不同来源报冲突、同一来源改口」），
 * 先跑这个探针看存量：数字不为 0 就说明有老模型带着并存位，
 * 那些模型读进来仍然会报「知识自相矛盾」（见 thinking.md 六点六的边界）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_revprobe tools/revprobe.c -lm
 *       ./build_revprobe [模型文件]     默认 model.bin（会在当前目录读 relations.txt）
 */
#include "../agent.h"
#include "../model_io.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "model.bin";
    AI *ai = ai_load_file(path);
    if (!ai) { printf("加载失败：%s\n", path); return 1; }
    size_t both = 0, tot_rel = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage) continue;
        for (size_t j = 0; j < t->count; ++j) {
            AssocEntry *e = &t->assocs[j];
            if (!e->rel && !e->nrel) continue;
            tot_rel++;
            for (uint8_t rid = 1; rid <= REL_CORE_MAX; ++rid) {
                uint16_t bit = REL_BIT(rid);
                if ((e->rel & bit) && (e->nrel & bit)) {
                    both++;
                    printf("  并存: %s -[%s]-> %s  role=%u hits=%u\n",
                           t->name ? t->name : "?", rel_name_of(ai, rid),
                           e->main && e->main->name ? e->main->name : "?",
                           (unsigned)e->role, (unsigned)e->hits);
                }
            }
        }
    }
    printf("总关系边=%zu  正负并存=%zu\n", tot_rel, both);
    ai_free(ai);
    return 0;
}
