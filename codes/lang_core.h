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


/* ---- §6.640 **少样本归纳**：从平行句对里学"词表 + 词序" ------------------
 *
 * 输入：每行 `外语句 = 中文句`（两侧都按空格切词），例如
 *     cat is animal = 猫 属于 动物
 *     dog is animal = 狗 属于 动物
 *
 * 归纳（很朴素的**按位置对齐**——这是明确的选择，不是"实现不好"）：
 *   ① 两侧词数相同才用（不同就跳过，并计入 skipped）；
 *   ② 同一位置上的（外语, 中文）配对，计数后取每个外语词**最常见的**中文（平票取先到的）；
 *   ③ 词序：看中文的**关系词**（用 `is_rel` 回调判定）在中文侧的位置 vs 外语侧的位置：
 *      同位置 ⇒ SVO；外语的关系词在**末尾**而中文在中间 ⇒ SOV；
 *   ④ 结果写成 `words.txt` + `meta.txt`（人可读、可改），再 `::lang load` 生效。
 *
 * 边界：不做统计对齐/词性/形态；两侧词数不同直接跳过；关系词靠外部判定（这里用回调，
 * 因为 `lang_core.h` 不认识 AI 的关系表）。
 */
typedef int (*lang_is_rel_fn)(const char *zh);

typedef struct {
    size_t pairs;      /* 写出的配对数 */
    size_t lines;      /* 读到的有效行数 */
    size_t skipped;    /* 词数不等/空行 跳过的行数 */
    int    sov;        /* 推出的词序：1 = SOV，0 = SVO */
    size_t sov_votes, svo_votes;   /* 两种排法的**冲突数**（不是票数，留着名字免得改调用点） */
    int    ambiguous;              /* 两种排法冲突数相同 ⇒ 词序**猜不准**（照实报） */
} LangInduce;

typedef struct { char from[LANG_WORD_CAP]; char to[LANG_WORD_CAP]; size_t n; } LangPair;

static inline int lang_split_words(const char *s, char w[][LANG_WORD_CAP], size_t max) {
    size_t n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t l = 0;
        while (p[l] && p[l] != ' ' && p[l] != '\t' && l + 1 < LANG_WORD_CAP) l++;
        memcpy(w[n], p, l); w[n][l] = 0;
        n++;
        p += l;
        while (*p == ' ' || *p == '\t') p++;
    }
    return (int)n;
}

/* 归纳并写盘。`dir` 是输出目录（会创建）。返回 pairs（0 = 一条都没学到）。 */
static inline size_t lang_induce(FILE *in, const char *dir, lang_is_rel_fn is_rel,
                                 LangInduce *st) {
    if (!in || !dir) return 0;
    LangInduce local;
    memset(&local, 0, sizeof local);
    LangPair cand[512];
    size_t nc = 0;
    /* 存下句子供第二遍判词序（最多 64 行） */
    static char kept_f[64][16][LANG_WORD_CAP];
    static char kept_z[64][16][LANG_WORD_CAP];
    static int kept_fn[64], kept_zn[64];
    size_t nk = 0;
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        nl = strchr(line, '\r'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { local.skipped++; continue; }
        *eq = 0;
        char fw[16][LANG_WORD_CAP], zw[16][LANG_WORD_CAP];
        int fn = lang_split_words(line, fw, 16);
        int zn = lang_split_words(eq + 1, zw, 16);
        local.lines++;
        if (fn <= 0 || fn != zn) { local.skipped++; continue; }
        if (nk < 64) {
            for (int i = 0; i < fn; ++i) {
                snprintf(kept_f[nk][i], LANG_WORD_CAP, "%s", fw[i]);
                snprintf(kept_z[nk][i], LANG_WORD_CAP, "%s", zw[i]);
            }
            kept_fn[nk] = fn; kept_zn[nk] = zn; nk++;
        }
        for (int i = 0; i < fn; ++i) {
            size_t k = 0;
            for (; k < nc; ++k) if (!strcmp(cand[k].from, fw[i]) && !strcmp(cand[k].to, zw[i])) break;
            if (k == nc) {
                if (nc >= 512) continue;
                snprintf(cand[nc].from, sizeof cand[nc].from, "%.31s", fw[i]);
                snprintf(cand[nc].to, sizeof cand[nc].to, "%.31s", zw[i]);
                cand[nc].n = 1;
                nc++;
            } else {
                cand[k].n++;
            }
        }
    }
    /* ② 词序：**两种排法各建一次映射，看谁的冲突少**。
     * 为什么不能用"位置对齐"直接投：SOV 的外语行 `cat animal is` 按位置会把 animal→属于、
     * is→动物 —— 映射本身就是错的，投出来永远是 SVO（实测）。冲突数才是判据：
     * 同一个外语词被映到两个不同中文词 = 一次冲突。 */
    {
        (void)is_rel;                       /* 词序判据只靠"映射冲突数"，不再看关系词 */
        size_t svo_conf = 0, sov_conf = 0;
        LangPair m1[512], m2[512];
        size_t n1 = 0, n2 = 0;
        for (size_t li = 0; li < nk; ++li) {
            for (int i = 0; i < kept_fn[li]; ++i) {
                for (int which = 0; which < 2; ++which) {
                    int to = i;
                    if (which == 1 && kept_zn[li] == 3 && i < 3) {
                        /* SOV：外语 [S,O,V] 对中文 [S,V,O] ⇒ 位置 1↔2 交换 */
                        to = (i == 1) ? 2 : (i == 2 ? 1 : 0);
                    }
                    LangPair *m = which ? m2 : m1;
                    size_t *nn = which ? &n2 : &n1;
                    size_t *conf = which ? &sov_conf : &svo_conf;
                    size_t k = 0;
                    for (; k < *nn; ++k)
                        if (!strcmp(m[k].from, kept_f[li][i])) break;
                    if (k == *nn) {
                        if (*nn < 512) {
                            snprintf(m[*nn].from, sizeof m[0].from, "%.31s", kept_f[li][i]);
                            snprintf(m[*nn].to, sizeof m[0].to, "%.31s", kept_z[li][to]);
                            m[*nn].n = 1;
                            (*nn)++;
                        }
                    } else if (strcmp(m[k].to, kept_z[li][to]) != 0) {
                        (*conf)++;                    /* 同一个外语词 → 两个中文词 = 冲突 */
                    }
                }
            }
        }
        local.ambiguous = (sov_conf == svo_conf);
        local.sov = (sov_conf < svo_conf) ? 1 : 0;
        local.sov_votes = sov_conf;
        local.svo_votes = svo_conf;
        if (local.sov) {
            nc = n2;
            for (size_t i = 0; i < nc; ++i) cand[i] = m2[i];
        } else {
            nc = n1;
            for (size_t i = 0; i < nc; ++i) cand[i] = m1[i];
        }
    }
    if (nc == 0) { if (st) *st = local; return 0; }
    /* 写盘 */
    char path[512];
    snprintf(path, sizeof path, "%s/words.txt", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { if (st) *st = local; return 0; }
    fprintf(f, "# 由 ::lang induce 自动归纳（按位置对齐，见 §6.640）—— 人可读、可改\n");
    for (size_t i = 0; i < nc; ++i)
        fprintf(f, "%s=%s\n", cand[i].from, cand[i].to);
    fclose(f);
    /* 词序：SOV 票多才算 SOV（默认 SVO） */
    /* 词序判定**只在这里**（252 行按冲突数定的），别再拿"票数"覆盖一遍：
     * 第一版就是被这行覆盖成"冲突少反而变 SVO"（现象：SVO 样本被判成 SOV）。 */
    snprintf(path, sizeof path, "%s/meta.txt", dir);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "# 由 ::lang induce 归纳的词序（SVO = 主谓宾，SOV = 主宾谓）\norder=%s\n",
                local.sov ? "SOV" : "SVO");
        fclose(f);
    }
    local.pairs = nc;
    if (st) *st = local;
    return nc;
}

#endif /* LANG_CORE_H */
