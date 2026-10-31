/* lang_core.h — 语言包的**核心表与翻译**（只依赖标准库；agent.h 和 lang.h 都 include 它）（§6.639）
 *
 * 为什么拆出来：翻译要发生在 `agent_step` 里（分词之前），而 `agent.h` 不能 include `lang.h`
 * （lang.h 依赖 agent.h，会循环）。把"表 + 逐词翻译"这层挪到只依赖 stdio/string 的小头里，
 * 两边都能用，且**只有一份实现**。
 */
#ifndef LANG_CORE_H
#define LANG_CORE_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LANG_MAX_WORDS 512
#define LANG_WORD_CAP  32

/* 全局（与 patterns.txt 的存放方式一致）：一个进程一套语言包 */
static char g_lang_code[16];
static char g_lang_from[LANG_MAX_WORDS][LANG_WORD_CAP];
static char g_lang_to[LANG_MAX_WORDS][LANG_WORD_CAP];
static size_t g_lang_n;
static int  g_lang_sov;

static inline void lang_reset(void) {
    g_lang_code[0] = 0;
    g_lang_n = 0;
    g_lang_sov = 0;
}

/* 读语言包：成功返回词条数（>0），失败 0。`dir` 形如 `lang/en`。 */
static inline size_t lang_load_dir(const char *dir) {
    lang_reset();
    if (!dir || !dir[0]) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/meta.txt", dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "order=SOV", 9)) g_lang_sov = 1;
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/words.txt", dir);
    f = fopen(path, "rb");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f) && g_lang_n < LANG_MAX_WORDS) {
        if (line[0] == '#' || line[0] == '\n' || !line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *to = eq + 1;
        char *nl = strchr(to, '\n');
        if (nl) *nl = 0;
        nl = strchr(to, '\r');
        if (nl) *nl = 0;
        char *frm = line;
        while (*frm == ' ' || *frm == '\t') frm++;
        while (*to == ' ' || *to == '\t') to++;
        if (!*frm || !*to) continue;
        snprintf(g_lang_from[g_lang_n], sizeof g_lang_from[0], "%.31s", frm);
        snprintf(g_lang_to[g_lang_n], sizeof g_lang_to[0], "%.31s", to);
        g_lang_n++;
    }
    fclose(f);
    if (g_lang_n == 0) return 0;
    snprintf(g_lang_code, sizeof g_lang_code, "%.15s", dir);
    return g_lang_n;
}

/* 单词语译：认出返回中文词，否则 NULL */
static inline const char *lang_word(const char *w) {
    if (!w || !w[0] || g_lang_n == 0) return NULL;
    for (size_t i = 0; i < g_lang_n; ++i)
        if (!strcmp(g_lang_from[i], w)) return g_lang_to[i];
    return NULL;
}

/* 整句翻译（逐词 + 词序重排）。返回 1 = 至少换出一个词（out 里是中文句），0 = 一个都没认出。 */
static inline int lang_translate(const char *input, char *out, size_t cap) {
    if (!input || !out || cap == 0 || g_lang_n == 0) return 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", input);
    char *w[24]; size_t n = 0;
    for (char *t = buf; *t && n < 24; ) {
        while (*t == ' ' || *t == '\t') t++;
        if (!*t) break;
        w[n++] = t;
        while (*t && *t != ' ' && *t != '\t') t++;
        if (*t) { *t = 0; t++; }
    }
    if (n == 0) return 0;
    char tw[24][LANG_WORD_CAP];
    int hit = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *zh = lang_word(w[i]);
        if (zh) { snprintf(tw[i], sizeof tw[0], "%s", zh); hit++; }
        else    { snprintf(tw[i], sizeof tw[0], "%s", w[i]); }
    }
    if (hit == 0) return 0;
    /* 词序：SOV ⇒ 主宾谓（把最后的谓语挪到第二位）；SVO ⇒ 原样 */
    size_t order[24];
    for (size_t i = 0; i < n; ++i) order[i] = i;
    if (g_lang_sov && n >= 3) {
        /* [S O V] → [S V O]：把最后一个挪到第 2 个位置 */
        size_t last = n - 1;
        for (size_t i = last; i > 1; --i) order[i] = i - 1;
        order[1] = last;
    }
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *t = tw[order[i]];
        int wr = snprintf(out + o, cap - o, "%s%s", (i ? " " : ""), t);
        if (wr < 0 || o + (size_t)wr >= cap) break;
        o += (size_t)wr;
    }
    return 1;
}


#endif /* LANG_CORE_H */
