/* tools/patuse.c — **数据行使用率探针**（§6.357）
 *
 * 把一批输入喂进去，统计每条 `[reply]` 命中了几次。
 * 从没命中的行 = 冗余 / 过拟合嫌疑（它们只在"当时的那个样本"里有存在感）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_patuse tools/patuse.c -lm
 *       ./build_patuse session.bin < 输入文件（一行一句）
 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    Agent *ag = agent_new(NULL, NULL, NULL);
    if (!ag) { fprintf(stderr, "建 Agent 失败\n"); return 1; }
    if (argc > 1) {
        AI *loaded = ai_load_file(argv[1]);
        if (loaded) { ai_free(ag->brain); ag->brain = loaded; }
    }
    AI *ai = ag->brain;
    /* agent_step 会往 stdout 打过程日志；探针只关心计数 ⇒ 临时把 stdout 静音 */
    int save1 = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    char line[512];
    size_t n = 0;
    while (fgets(line, sizeof line, stdin)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L || line[0] == '#') continue;
        char out[400];
        /* §6.406 **每句都当"没有挂着的追问"**：`[reply]` 桶只在 `!ask_pending` 时生效，
         * 而上一句若留下追问，这一句就会被跳过 ⇒ 行被误报"从未命中"（实测 `^你在么` 被误报）。
         * 探针只关心"这行能不能匹配"，所以每句前清掉挂起状态。*/
        ai->ask_pending = 0;
        if (devnull >= 0) dup2(devnull, 1);
        (void)agent_step(ag, line, out, sizeof out);
        if (save1 >= 0) dup2(save1, 1);
        n++;
    }
    if (devnull >= 0) close(devnull);
    if (save1 >= 0) close(save1);
    /* §6.361 形状表**真实输入**的使用率（合成输入不可靠：分词/前态都会影响，
     * 所以这里只统计 3265 句测例上的命中数；0 命中的行留给人工核实）。 */
    {
        size_t deadsh = 0;
        for (size_t r = 0; r < g_shape_n; ++r) if (!g_shape_hit[r]) deadsh++;
        printf("形状行 %zu 条，测例上从未命中 %zu 条\n", g_shape_n, deadsh);
        /* 打印所有未命中的**基础行**（跳过紧随其后、rhs 相同的自动变体） */
        for (size_t r = 0; r < g_shape_n; ++r) {
            if (g_shape_hit[r]) continue;
            int is_variant = (r > 0 && strcmp(g_shape_r[r], g_shape_r[r - 1]) == 0);
            if (is_variant) continue;
            printf("  形状未见#%zu: %s => %s\n", r, g_shape_l[r], g_shape_r[r]);
        }
    }
    size_t dead = 0;
    for (size_t i = 0; i < g_reply_n; ++i) if (!g_reply_hit[i]) dead++;
    printf("输入 %zu 句；reply 行 %zu 条，从未命中 %zu 条\n", n, g_reply_n, dead);
    for (size_t i = 0; i < g_reply_n; ++i)
        if (!g_reply_hit[i]) printf("  未见: %s\n", g_reply_kw[i]);
    (void)ai;
    return 0;
}
