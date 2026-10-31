/* tools/langprobe.c — **语言包**探针（阶段二 WP6；§6.639） */
#include "../agent.h"
#include "../lang.h"
#include <stdio.h>
#include <string.h>
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
    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
