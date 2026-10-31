/* tools/langprobe.c — **语言包**探针（阶段二 WP6；§6.639） */
#include "../agent.h"
#include "../lang.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static int passes = 0, fails = 0;
static void ok(int c, const char *w) { if (c) { printf("  [OK]   %s\n", w); passes++; } else { printf("  [FAIL] %s\n", w); fails++; } }
int main(void) {
    AI *a = ai_new(); aip_patterns_load(a); relations_load(a, "relations.txt");
    printf("== 语言包 ==\n");
    ok(g_lang_n == 0, "默认没装（都按中文）");
    size_t n = lang_load_dir("lang/en");
    ok(n >= 10, "lang/en 装得上（11 个词）");
    ok(!g_lang_sov, "meta.txt 读到 order=SVO");
    ok(!strcmp(lang_word("cat"), "猫") && !strcmp(lang_word("is"), "属于"), "逐词映射：cat→猫、is→属于");
    ok(lang_word("zebra") == NULL, "不认识的词 ⇒ NULL（不猜）");
    char out[256] = {0};
    ok(lang_translate("cat is animal", out, sizeof out) == 1 && !strcmp(out, "猫 属于 动物"),
       "整句翻译：cat is animal → 猫 属于 动物");
    memset(out, 0, sizeof out);
    ok(lang_translate("zebra quokka wombat", out, sizeof out) == 0, "一个词都不认识 ⇒ 0（不硬翻）");
    lang_reset();
    ok(g_lang_n == 0 && lang_translate("cat is animal", out, sizeof out) == 0, "卸掉之后不再翻译");
    /* §6.640 少样本归纳 */
    {
        FILE *pf = fopen("_langprobe_par.txt", "wb");
        if (pf) {
            fprintf(pf, "cat is animal = 猫 属于 动物\n");
            fprintf(pf, "dog is animal = 狗 属于 动物\n");
            fprintf(pf, "penguin is bird = 企鹅 属于 鸟类\n");
            fprintf(pf, "bogus line without equals\n");      /* 跳过 */
            fprintf(pf, "one two three four = 猫 属于 动物\n"); /* 词数不等 ⇒ 跳过 */
            fclose(pf);
        }
        mkdir("_langprobe_pack", 0755);      /* 归纳只写盘、不建目录（和 CLI 一样由调用方建） */
        FILE *in = fopen("_langprobe_par.txt", "rb");
        LangInduce st;
        memset(&st, 0, sizeof st);
        size_t pairs = in ? lang_induce(in, "_langprobe_pack", NULL, &st) : 0;
        if (in) fclose(in);
        ok(pairs == 6, "归纳出 6 对**不同**的词（3 行 × 3 词，重复的合并）");
        ok(st.skipped == 2, "跳过 2 行（没等号 / 词数不等）");
        ok(!st.sov && !st.ambiguous, "词序判成 SVO（SVO 冲突 0 / SOV 冲突 1）");
        lang_reset();
        size_t ln = lang_load_dir("_langprobe_pack");
        ok(ln == 6, "归纳出来的包装得上（6 个词）");
        char tr[256] = {0};
        ok(lang_translate("cat is animal", tr, sizeof tr) == 1 && !strcmp(tr, "猫 属于 动物"),
           "用归纳的包翻译：cat is animal → 猫 属于 动物");
        remove("_langprobe_par.txt");
        remove("_langprobe_pack/words.txt");
        remove("_langprobe_pack/meta.txt");
        remove("_langprobe_pack");
    }
    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
