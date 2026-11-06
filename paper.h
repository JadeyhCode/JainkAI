/* paper.h — **读论文（离线优先）**：抽候选断言 → 全部进"待证"（阶段二 WP5；§6.638）
 *
 * 用户的边界（第 52 轮定的）：**不做"自动读懂论文"**，只做"抽候选断言 + 待证 + 人确认"。
 *
 * 这一版做什么：
 *   · `paper_extract()`：把一段文本切成句子，挑出**像断言**的短句（`A 关系 B` 形状，
 *     关系词在 `relations.txt` 里），做成待证候选；
 *   · `::paper read <文件>`：读工作区里的**纯文本**（≤ 256 KB），抽完写进待证队列（来源 `论文`），
 *     并报"抽了几条 / 记了几条 / 跳过几条"；
 *   · 抽取结果**一律待证**：不写事实边（要用户确认或第二个独立来源，见 §6.636/§6.637）。
 *
 * 明确不做：PDF（要外部命令/库）、联网、表格/公式、指代消解、引用编号追踪。
 */
#ifndef PAPER_H
#define PAPER_H

#include "claim.h"
#include "pdf.h"        /* §6.678 读 PDF：`::paper read x.pdf` 走这里，不再只认纯文本 */

#define PAPER_MAX_BYTES  (256u * 1024u)
#define PAPER_MAX_CLAIMS 64
#define PAPER_PDF_TEXT   (1024u * 1024u)   /* PDF 抽出来的文字缓冲（1 MB ≈ 十多万词） */

typedef struct {
    char a[40];
    char r[24];
    char b[40];
} PaperClaim;

/* 这个 token 能不能当"词"（去掉标点/虚词） */
static inline int paper_word_ok(const char *w) {
    if (!w || !w[0]) return 0;
    static const char *skip[] = { "的", "了", "是", "和", "与", "及", "等等", "等", "这", "那",
                                  "其", "之", "所", "被", "把", "在", "对", "从", "到",
                                  "这个", "那个", "这些", "那些", "它", "他", "她", "我", "你",
                                  "它们", "他们", "我们", "你们", NULL };
    for (size_t i = 0; skip[i]; ++i) if (!strcmp(w, skip[i])) return 0;
    /* 标点：中文标点是多字节，不能写 `*p == '，'`（多字符常量，-Werror 直接拦）——
     * 用字节判据：UTF-8 的标点都是 >0x7F 的三字节，**ASCII 的标点**在这里只判单字节那几个。 */
    for (const char *p = w; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == ',' || c == '.' || c == ';' || c == ':' || c == '?' || c == '!' ||
            c == '(' || c == ')' || c == '"')
            return 0;
        if (c == 0xEF && (unsigned char)p[1] == 0xBC) return 0;   /* 全角标点（，．；：？！（）） */
        if (c == 0xE3 && (unsigned char)p[1] == 0x80) return 0;   /* 。、】等 */
    }
    return 1;
}

/* §6.651 **切分**单独成函数（上轮的教训：切分与抽取混在一起，改坏了查不出根因）。
 * 按空格/制表符切，最多 8 段、每段 ≤63 字节；空段跳过。返回段数。 */
static inline size_t paper_segments(const char *s, size_t len, char seg[][64], size_t max) {
    if (!s || !seg || max == 0 || len == 0) return 0;
    size_t n = 0, i = 0;
    while (i < len && n < max) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= len) break;
        size_t o = 0;
        while (i < len && s[i] != ' ' && s[i] != '\t') {
            if (o + 1 < sizeof seg[0]) seg[n][o++] = s[i];
            i++;
        }
        seg[n][o] = 0;
        if (o) n++;
    }
    return n;
}

/* §6.757 **泛化：说法归一**（不是背事实表，而是加"同义说法 → 规范关系"的映射）
 *
 * 为什么需要：原来的抽取只认**图里已有的关系词**（属于/会/在/…）。同一件事换个说法就抽不到：
 *   `企鹅是鸟类的一种`（关系词"是"）、`企鹅可以游泳`（"可以"）、`企鹅生活在南极`（"生活在"）、
 *   `企鹅害怕北极熊`（"害怕"）—— 实测 16 句里只抽出 14 条，其中 4 条关系词是错的。
 *
 * 这里只做两件事，都是**说法层面**的，不碰事实：
 *   ① 关系词归一：可以/能够/能 → 会；害怕/惧怕/恐惧 → 怕；喜爱/爱好 → 喜欢；拥有/具有 → 有；
 *      生活在/住在/栖息于/分布于/位于 → 在；依赖 → 需要；是 → 属于；
 *   ② 词的尾巴归一：宾语去掉「的一种/一类/之一」，主语去掉「生活/住」（`企鹅生活|在` → `企鹅|在`）。
 * 教一次这种说法，**所有同形句**都能用 —— 这就是"泛化"在白箱里的做法。 */
/* §6.760 **外部说法表**（`paraphrase.map`，一行 `原说法<TAB>规范关系`）。
 * 这是"教一族"的数据通道：`tools/induce_paraphrase.sh` 从例句里归纳出这种行，
 * 追加进文件即可生效（不需要改 C 代码、不需要重编译）。 */
#define PAPER_MAP_MAX 128
static char g_pmap_from[PAPER_MAP_MAX][24];
static char g_pmap_to[PAPER_MAP_MAX][16];
static int  g_pmap_n = 0;

static inline void paper_map_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && g_pmap_n < PAPER_MAP_MAX) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *to = tab + 1;
        size_t tl = strlen(to);
        while (tl && (to[tl - 1] == '\n' || to[tl - 1] == '\r')) to[--tl] = 0;
        if (!line[0] || !to[0]) continue;
        /* 截断保护：超长就跳过（GCC 的 -Wformat-truncation 会拦直接 snprintf） */
        if (strlen(line) >= sizeof g_pmap_from[0] || strlen(to) >= sizeof g_pmap_to[0]) continue;
        memcpy(g_pmap_from[g_pmap_n], line, strlen(line) + 1);
        memcpy(g_pmap_to[g_pmap_n], to, strlen(to) + 1);
        g_pmap_n++;
    }
    fclose(f);
}

/* §6.764 **从图里读说法映射**：`擅长 --[同义]--> 会` 存进图后，抽取器直接用它，
 * 不再依赖 `paraphrase.map`（文件只是"批量导入"的方便通道）。返回 1 = 图里查到了。 */
static inline int paper_graph_canon(AI *ai, const char *w, char *out, size_t cap) {
    if (!ai || !ai->pool || !w || !w[0] || !out || cap == 0) return 0;
    Token *t = pool_find(ai->pool, w);
    if (!t) return 0;
    for (size_t j = 0; j < t->count; ++j) {
        const AssocEntry *e = &token_assocs_ro(t)[j];
        if (!e->main || !e->main->name || !e->dyn) continue;
        const char *dn = rel_dyn_name(ai, (uint8_t)(REL_DYN_BASE + e->dyn - 1));
        if (!dn) continue;
        if (strcmp(dn, "同义") && strcmp(dn, "别名") && strcmp(dn, "等同")) continue;
        size_t l = strlen(e->main->name);
        if (l + 1 >= cap) continue;
        memcpy(out, e->main->name, l + 1);
        return 1;
    }
    return 0;
}

static inline const char *paper_canon_rel(const char *w) {
    if (!w || !w[0]) return w;
    for (int i = 0; i < g_pmap_n; ++i) if (!strcmp(w, g_pmap_from[i])) return g_pmap_to[i];
    if (!strcmp(w, "可以") || !strcmp(w, "能够") || !strcmp(w, "能") || !strcmp(w, "会")) return "会";
    if (!strcmp(w, "害怕") || !strcmp(w, "惧怕") || !strcmp(w, "恐惧")) return "怕";
    if (!strcmp(w, "喜爱") || !strcmp(w, "爱好")) return "喜欢";
    if (!strcmp(w, "拥有") || !strcmp(w, "具有")) return "有";
    if (!strcmp(w, "栖息") || !strcmp(w, "生活")) return "在";
    if (!strcmp(w, "生活在") || !strcmp(w, "住在") || !strcmp(w, "栖息于") ||
        !strcmp(w, "分布于") || !strcmp(w, "位于")) return "在";
    if (!strcmp(w, "依赖")) return "需要";
    if (!strcmp(w, "是")) return "属于";
    return w;
}

/* 去掉宾语尾巴上的「的一种 / 一类 / 之一」 */
static inline void paper_trim_obj(char *b) {
    static const char *sfx[] = { "的一种之一", "的一种", "的一类之一", "的一类", "之一", "的一种之一" };
    for (size_t k = 0; k < sizeof sfx / sizeof sfx[0]; ++k) {
        size_t l = strlen(sfx[k]), bl = strlen(b);
        if (bl > l && !strcmp(b + bl - l, sfx[k])) { b[bl - l] = 0; return; }
    }
}

/* §6.757 ② 宾语里再包一层动词：`喜欢吃鱼` ⇒ 关系换成「吃」、宾语取「鱼」
 * （"喜欢+动作"在中文里通常就是说这个动作；保留 喜欢/怕/会/需要 时宾语以动词开头才拆） */
static inline const char *paper_split_verb(const char *rel, char *b) {
    static const char *verbs[] = { "吃", "喝", "用", "住", "去", "做", "看", "听" };
    if (!rel || !b || !b[0]) return rel;
    if (strcmp(rel, "喜欢") && strcmp(rel, "怕") && strcmp(rel, "会") && strcmp(rel, "需要")) return rel;
    for (size_t k = 0; k < sizeof verbs / sizeof verbs[0]; ++k) {
        size_t l = strlen(verbs[k]);
        if (!strncmp(b, verbs[k], l) && strlen(b) > l) { memmove(b, b + l, strlen(b + l) + 1); return verbs[k]; }
    }
    return rel;
}

/* §6.757 ③ 宾语前缀的虚字：分词器会把「能够」拆成 能+够、「栖息于」拆成 栖息+于，
 * 归一后宾语会剩下「够奔跑」「于海洋」这类前缀 ⇒ 去掉。 */
static inline void paper_trim_obj_prefix(char *b) {
    static const char *pfx[] = { "够", "于", "在", "以", "和", "与", "了" };
    for (size_t k = 0; k < sizeof pfx / sizeof pfx[0]; ++k) {
        size_t l = strlen(pfx[k]);
        if (!strncmp(b, pfx[k], l) && strlen(b) > l + 1) { memmove(b, b + l, strlen(b + l) + 1); return; }
    }
}

/* 去掉主语尾巴上的「生活 / 住」（`企鹅生活` 是分词器切出来的整体） */
static inline void paper_trim_subj(char *a) {
    static const char *sfx[] = { "生活", "居住" };
    for (size_t k = 0; k < sizeof sfx / sizeof sfx[0]; ++k) {
        size_t l = strlen(sfx[k]), al = strlen(a);
        if (al > l + 2 && !strcmp(a + al - l, sfx[k])) { a[al - l] = 0; return; }
    }
}

/* §6.757 ④ **文本级归一**：分词器不认识的词（栖息于/依赖/能够…）先换成它认识的规范词，
 * 再走分词。这是"说法层"的泛化：教一次替换，所有同形句都受益。 */
static inline void paper_normalize_text(const char *in, char *out, size_t cap) {
    static const struct { const char *from, *to; } R[] = {
        { "栖息于", "在" }, { "栖息", "在" }, { "生活在", "在" }, { "住在", "在" },
        { "分布于", "在" }, { "位于", "在" }, { "存在于", "在" },
        { "能够", "会" }, { "可以", "会" }, { "能", "会" },
        { "依赖", "需要" }, { "需要", "需要" },
        { "害怕", "怕" }, { "惧怕", "怕" }, { "恐惧", "怕" },
        { "喜爱", "喜欢" }, { "爱好", "喜欢" },
        { "拥有", "有" }, { "具有", "有" },
        { "是", "属于" },
    };
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 8 < cap; ) {
        int hit = 0;
        for (size_t k = 0; k < sizeof R / sizeof R[0]; ++k) {
            size_t l = strlen(R[k].from);
            if (!strncmp(in + i, R[k].from, l)) {
                size_t tl = strlen(R[k].to);
                if (o + tl + 1 >= cap) break;
                memcpy(out + o, R[k].to, tl); o += tl; i += l; hit = 1; break;
            }
        }
        if (!hit) out[o++] = in[i++];
    }
    out[o] = 0;
}

/* §6.762 **框式结构（circumfix）**：中文里有一类说法把宾语夹在关系词中间 ——
 *   `以鱼为食` / `以肉为主食` / `以花蜜为生` / `以山洞为家`
 * 顺序是 [以][宾语][为X]，抽取器（要求 主语-关系-宾语 顺序）必然抽不到。
 * 做法：**分词之前**把它改写成正常顺序 —— 去掉"以"、把"为X"换成规范关系词：
 *   以鱼为食   ⇒ 吃鱼      （吃/需要）
 *   以山洞为家 ⇒ 在山洞    （在）
 * 只要保留主体在句首，改写后的句子就落回原来的管道（分词 → 关系词匹配 → 主/宾）。
 * ⚠ 只处理"以"字框式；"把/被/将"这类处置式不在这里（那是另一套改写，没做）。 */
#define PAPER_CF_MAX 64
static char g_cf_pre[PAPER_CF_MAX][16];
static char g_cf_suf[PAPER_CF_MAX][16];
static char g_cf_rel[PAPER_CF_MAX][16];
static int  g_cf_n = 0;

/* 外部框式表：`circumfix.map`，一行 `前缀<TAB>后缀<TAB>规范关系`（例如 `以<TAB>为业<TAB>吃`）。
 * 和说法表一样是**数据**：`tools/induce_paraphrase.sh` 能从例句里把它归纳出来。 */
static inline void paper_cf_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && g_cf_n < PAPER_CF_MAX) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = 0;
        char *rel = t2 + 1;
        size_t rl = strlen(rel);
        while (rl && (rel[rl - 1] == '\n' || rel[rl - 1] == '\r')) rel[--rl] = 0;
        if (!line[0] || !t1[1] || !rel[0]) continue;
        if (strlen(line) >= sizeof g_cf_pre[0] || strlen(t1 + 1) >= sizeof g_cf_suf[0] ||
            strlen(rel) >= sizeof g_cf_rel[0]) continue;
        memcpy(g_cf_pre[g_cf_n], line, strlen(line) + 1);
        memcpy(g_cf_suf[g_cf_n], t1 + 1, strlen(t1 + 1) + 1);
        memcpy(g_cf_rel[g_cf_n], rel, strlen(rel) + 1);
        g_cf_n++;
    }
    fclose(f);
}

static inline void paper_rewrite_yiwei(const char *in, char *out, size_t cap) {
    /* 后缀表：`为X` ⇒ 规范关系词。⚠ **长后缀在前**（为主食 要先于 为食 匹配上）。
     * 内置只留最常见的几条；其余靠 `circumfix.map`（"为生"就是留给归纳演示的）。 */
    static const struct { const char *suf; const char *rel; } S[] = {
        { "为主食", "吃" }, { "为食物", "吃" }, { "为食", "吃" },
        { "为家", "在" }, { "为居", "在" },
    };
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 8 < cap; ) {
        /* 找"以"（E4 BB A5）*/
        /* 外部表：前缀匹配（通常是"以"，也可以归纳出别的前缀） */
        int ext_hit = 0, ext_len = 0; const char *ext_rel = NULL; size_t ext_suflen = 0, ext_pos = 0;
        for (int q = 0; q < g_cf_n; ++q) {
            size_t pl = strlen(g_cf_pre[q]);
            if (strncmp(in + i, g_cf_pre[q], pl) != 0) continue;
            const char *hit = strstr(in + i + pl, g_cf_suf[q]);
            if (!hit) continue;
            size_t pos = (size_t)(hit - in);
            if (pos <= i + pl) continue;
            ext_hit = 1; ext_len = (int)pl; ext_rel = g_cf_rel[q];
            ext_suflen = strlen(g_cf_suf[q]); ext_pos = pos;
            break;
        }
        if (ext_hit && ext_rel) {
            size_t rl = strlen(ext_rel);
            if (o + rl + 1 < cap) { memcpy(out + o, ext_rel, rl); o += rl; }
            for (size_t q = (size_t)i + (size_t)ext_len; q < ext_pos && o + 1 < cap; ++q) out[o++] = in[q];
            i = ext_pos + ext_suflen;
            continue;
        }
        if ((unsigned char)in[i] == 0xE4 && (unsigned char)in[i + 1] == 0xBB &&
            (unsigned char)in[i + 2] == 0xA5) {
            /* 从 i+3 起找最近的后缀 */
            size_t best_pos = 0, best_len = 0; const char *best_rel = NULL;
            for (size_t k = 0; k < sizeof S / sizeof S[0]; ++k) {
                const char *hit = strstr(in + i + 3, S[k].suf);
                if (!hit) continue;
                size_t pos = (size_t)(hit - in);
                if (!best_pos || pos < best_pos) { best_pos = pos; best_len = strlen(S[k].suf); best_rel = S[k].rel; }
            }
            if (best_rel && best_pos > i + 3) {
                size_t rl = strlen(best_rel);
                if (o + rl + 1 < cap) { memcpy(out + o, best_rel, rl); o += rl; }   /* 关系词提到前面 */
                for (size_t q = i + 3; q < best_pos && o + 1 < cap; ++q) out[o++] = in[q]; /* 宾语跟在后面 */
                i = best_pos + best_len;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

/* 从句子 [s, s+len) 里抽一条断言：**段数 3–6**、中间择一个关系词、两边各拼成主语/宾语（各 ≤2 段）。
 * 判据全在 `paper_segments` 的结果上做（切分与抽取分开，便于单测）。 */
static inline int paper_sentence_claim(AI *ai, const char *s, size_t len, PaperClaim *out) {
    if (!ai || !s || len == 0 || !out) return 0;
    /* §6.757 先把句子里的"说法"换成规范词（分词器不认识 栖息于/依赖 这类词，实测抽不到） */
    char norm[512];
    if (len < sizeof norm) {
        char raw[512];
        memcpy(raw, s, len); raw[len] = 0;
        paper_normalize_text(raw, norm, sizeof norm);
        char fix[512];
        paper_rewrite_yiwei(norm, fix, sizeof fix);   /* §6.762 以X为食 ⇒ 吃X */
        snprintf(norm, sizeof norm, "%s", fix);
        s = norm; len = strlen(norm);
    }
    char seg[8][64];
    size_t sn = paper_segments(s, len, seg, 8);
    /* §6.674 **没有空格的句子**（真论文那种）：退回**分词器**，但**按 token 拼回去**——
     * `企鹅属于鸟类` 会被切成 企/鹅/属于/鸟/类，找出关系词那个 token，左右各自拼成主/宾。
     * 这一步只处理"整句没有空格"的情形，不动上面 3–6 段的路径。 */
    if (sn < 3) {
        /* 先把"逗号/分号"当分句符，只取**第一段**（复合句只抽前半，照实说） */
        char first[256];
        size_t fl = 0;
        for (size_t i = 0; i < len && fl + 1 < sizeof first; ++i) {
            unsigned char c = (unsigned char)s[i];
            if (c == 0xEF && (unsigned char)s[i + 1] == 0xBC &&
                ((unsigned char)s[i + 2] == 0x8C || (unsigned char)s[i + 2] == 0x9B)) break; /* ，； */
            if (c == 0xE3 && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x81) break; /* 、 */
            first[fl++] = s[i];
        }
        first[fl] = 0;
        len = fl;
        size_t tn = 0;
        char **tw = ai_tokenize(ai, first, &tn);
        if (!tw) return 0;
        size_t ri2 = tn;
        char gcanon[64];
        for (size_t i = 1; i + 1 < tn; ++i) {
            /* §6.764 先查图（`擅长 --[同义]--> 会`），再退回内置/外部说法表 */
            const char *cw = paper_graph_canon(ai, tw[i], gcanon, sizeof gcanon)
                             ? gcanon : paper_canon_rel(tw[i]);
            if (rel_id_of(ai, cw, 0)) { ri2 = i; break; }
        }
        if (ri2 == tn) { ai_free_words(tw, tn); return 0; }
        char a2[64] = {0}, b2[64] = {0};
        size_t ao2 = 0, bo2 = 0;
        for (size_t i = 0; i < ri2; ++i) {
            size_t l = strlen(tw[i]);
            if (ao2 + l + 1 >= sizeof a2) { ai_free_words(tw, tn); return 0; }
            memcpy(a2 + ao2, tw[i], l); ao2 += l;
        }
        for (size_t i = ri2 + 1; i < tn; ++i) {
            size_t l = strlen(tw[i]);
            if (bo2 + l + 1 >= sizeof b2) { ai_free_words(tw, tn); return 0; }
            memcpy(b2 + bo2, tw[i], l); bo2 += l;
        }
        a2[ao2] = 0;
        b2[bo2] = 0;
        /* 去掉宾语尾部的标点（`鸟类。` → `鸟类`） */
        while (bo2 > 0) {
            unsigned char c = (unsigned char)b2[bo2 - 1];
            if (c == 0x82 && bo2 >= 3 && (unsigned char)b2[bo2 - 3] == 0xE3 &&
                (unsigned char)b2[bo2 - 2] == 0x80) { bo2 -= 3; continue; }   /* 。 */
            if (c == 0x81 && bo2 >= 3 && (unsigned char)b2[bo2 - 3] == 0xEF &&
                (unsigned char)b2[bo2 - 2] == 0xBC) { bo2 -= 3; continue; }   /* ！ */
            if (c == 0x9F && bo2 >= 3 && (unsigned char)b2[bo2 - 3] == 0xEF &&
                (unsigned char)b2[bo2 - 2] == 0xBC) { bo2 -= 3; continue; }   /* ？ */
            if (c == ' ' || c == '.') { bo2 -= 1; continue; }
            break;
        }
        b2[bo2] = 0;
        int okr = (ao2 >= 3 && bo2 >= 3);
        if (okr) {
            paper_trim_subj(a2); paper_trim_obj(b2); paper_trim_obj_prefix(b2);
            const char *rr2 = paper_graph_canon(ai, tw[ri2], gcanon, sizeof gcanon)
                              ? gcanon : paper_canon_rel(tw[ri2]);
            rr2 = paper_split_verb(rr2, b2);
            snprintf(out->a, sizeof out->a, "%.39s", a2);
            snprintf(out->r, sizeof out->r, "%.23s", rr2);
            snprintf(out->b, sizeof out->b, "%.39s", b2);
        }
        ai_free_words(tw, tn);
        return okr;
    }
    if (sn < 3 || sn > 6) return 0;
    /* ⚠ 只检查**主/宾**段，不检查关系词段：`在/对/从` 这些在 `paper_word_ok` 的虚词表里
     * （它们不该当主语），但它们**正当关系词** —— 上一轮把整句一刀切掉，四段句一条都抽不到。 */
    /* 候选关系词（从右往左找第一个"两边都不超过 2 段"的） */
    int ri = -1;
    for (int i = (int)sn - 2; i >= 1; --i) {
        if (!rel_id_of(ai, paper_canon_rel(seg[i]), 0)) continue;
        if ((size_t)i <= 2 && (sn - (size_t)i - 1) <= 2) { ri = i; break; }
    }
    if (ri < 1) return 0;
    char a[64] = {0}, b[64] = {0};
    size_t ao = 0, bo = 0;
    for (int i = 0; i < ri; ++i) {
        size_t l = strlen(seg[i]);
        if (ao + l + 1 >= sizeof a) return 0;
        memcpy(a + ao, seg[i], l); ao += l;
    }
    for (size_t i = (size_t)ri + 1; i < sn; ++i) {
        size_t l = strlen(seg[i]);
        if (bo + l + 1 >= sizeof b) return 0;
        memcpy(b + bo, seg[i], l); bo += l;
    }
    if (ao < 3 || bo < 3) return 0;                    /* 单字主/宾太容易误抽 */
    if (!paper_word_ok(a) || !paper_word_ok(b)) return 0;   /* 主/宾里不许混虚词/标点 */
    paper_trim_subj(a); paper_trim_obj(b); paper_trim_obj_prefix(b);
    snprintf(out->a, sizeof out->a, "%.39s", a);
    snprintf(out->r, sizeof out->r, "%.23s", paper_canon_rel(seg[ri]));
    snprintf(out->b, sizeof out->b, "%.39s", b);
    return 1;
}

/* 扫一段文本：按句号/问号/换行切句，逐句抽断言。返回抽到的条数（<= cap）。 */
static inline size_t paper_extract(AI *ai, const char *text, PaperClaim *out, size_t cap) {
    if (!ai || !text || !out || cap == 0) return 0;
    size_t got = 0;
    const char *p = text;
    while (*p && got < cap) {
        const char *st = p;
        /* 分句符：ASCII `! ? \n` + UTF-8 的 `。！？`（E3 80 82 / EF BC 81 / EF BC 9F） */
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c == '\n' || c == '!' || c == '?') break;
            if (c == 0xE3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x82) break;
            if (c == 0xEF && (unsigned char)p[1] == 0xBC &&
                ((unsigned char)p[2] == 0x81 || (unsigned char)p[2] == 0x9F)) break;
            p++;
        }
        size_t len = (size_t)(p - st);
        /* 跳过分句符：中文标点是**三字节**，只 `p++` 会留下两个残字节，
         * 下一句开头就会多出 `??` 前缀（实测：`企鹅|会|游泳` 变成 `��企鹅|会|游泳`）。 */
        if (*p) {
            unsigned char c = (unsigned char)*p;
            if (c == 0xE3 && (unsigned char)p[1] == 0x80) p += 3;
            else if (c == 0xEF && (unsigned char)p[1] == 0xBC) p += 3;
            else p += 1;
        }
        PaperClaim c;
        if (len >= 5 && len <= 200 && paper_sentence_claim(ai, st, len, &c)) {
            /* 去重（同一句里只保留一次） */
            int dup = 0;
            for (size_t i = 0; i < got; ++i)
                if (!strcmp(out[i].a, c.a) && !strcmp(out[i].r, c.r) && !strcmp(out[i].b, c.b)) {
                    dup = 1; break;
                }
            if (!dup) out[got++] = c;
        }
    }
    return got;
}

/* 抽到的断言全部写成"待证"（来源：论文）。返回写进去的条数。 */
static inline size_t paper_to_claims(AI *ai, const PaperClaim *cs, size_t n) {
    if (!ai || !cs) return 0;
    size_t made = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!claim_add(ai, "论文", cs[i].a, cs[i].r, cs[i].b)) continue;
        made++;
    }
    return made;
}

/* §6.678 **PDF 入口**：先取文字（pdf.h），再走同一条"抽断言 ⇒ 待证"的路。
 * 这样 `::paper read x.pdf` 与 `::net get`（下载回来的 PDF）共用一份实现。 */
static inline size_t paper_ingest_pdf(AI *ai, const char *path, size_t *made, char *note, size_t cap) {
    if (made) *made = 0;
    char *txt = (char *)malloc(PAPER_PDF_TEXT);
    if (!txt) {
        if (note && cap) snprintf(note, cap, "内存不够（PDF 文字缓冲要 %u KB）", PAPER_PDF_TEXT / 1024);
        return 0;
    }
    char pnote[256] = {0};
    size_t dec = 0, skip = 0;
    size_t n = pdf_extract_file(path, txt, PAPER_PDF_TEXT, &dec, &skip, pnote, sizeof pnote);
    if (n == 0) {
        if (note && cap)
            snprintf(note, cap, "%s —— 一个字都没读出来（我不猜，也不编）",
                     pnote[0] ? pnote : "PDF 里没抽出文字");
        free(txt);
        return 0;
    }
    PaperClaim cs[PAPER_MAX_CLAIMS];
    size_t k = paper_extract(ai, txt, cs, PAPER_MAX_CLAIMS);
    free(txt);
    if (k == 0) {
        if (note && cap)
            snprintf(note, cap, "%s —— 但一个三段式断言都没抽到（共 %zu 字节文字；中文/英文都要"
                                "「主 关系 宾」形状）", pnote, n);
        return 0;
    }
    size_t m = paper_to_claims(ai, cs, k);
    if (made) *made = m;
    if (note && cap) {
        snprintf(note, cap, "%s；抽出 %zu 字节文字 ⇒ 断言 %zu 条，记为待证 %zu 条（**都不算数**）",
                 pnote, n, k, m);
        for (size_t i = 0; i < k && i < 6; ++i) {
            size_t o = strlen(note);
            if (o + 96 < cap)
                snprintf(note + o, cap - o, "\n  · %.39s %.23s %.39s", cs[i].a, cs[i].r, cs[i].b);
        }
    }
    return k;
}

/* §6.663 **一条入口**：读文件 → 抽断言 → 写待证。`::paper read` 与 `::paper fetch` 共用，
 * 不再各自复制一遍（上一轮的重复代码就是两次实现开始漂移的起点）。
 * 返回抽到的断言数；`*made` 是真正写进待证的条数；note 里是给人看的说明。 */
static inline size_t paper_ingest(AI *ai, const char *path, size_t *made, char *note, size_t cap) {
    if (made) *made = 0;
    if (!ai || !path || !path[0]) {
        if (note && cap) snprintf(note, cap, "没给文件");
        return 0;
    }
    /* 先嗅文件头：PDF 走另一条路（PDF 允许到 6 MB，纯文本只到 256 KB） */
    {
        FILE *hf = fopen(path, "rb");
        if (hf) {
            char hd[8] = {0};
            size_t hg = fread(hd, 1, 5, hf);
            fclose(hf);
            if (hg == 5 && memcmp(hd, "%PDF-", 5) == 0)
                return paper_ingest_pdf(ai, path, made, note, cap);
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (note && cap) snprintf(note, cap, "读不到 %s（只读工作区里的纯文本）", path);
        return 0;
    }
    /* §6.664 缓冲区放**堆**（原来是 static 256 KB ⇒ 不可重入、两个脑不能同时读）*/
    char *txt = (char *)malloc(PAPER_MAX_BYTES + 1);
    if (!txt) {
        fclose(f);
        if (note && cap) snprintf(note, cap, "内存不够（要 %u KB 缓冲）", PAPER_MAX_BYTES / 1024);
        return 0;
    }
    size_t got = fread(txt, 1, PAPER_MAX_BYTES, f);
    int big = !feof(f);
    fclose(f);
    txt[got] = 0;
    if (big) {
        free(txt);
        if (note && cap) snprintf(note, cap, "%s 超过 %u KB，拒绝（先切一段）", path, PAPER_MAX_BYTES / 1024);
        return 0;
    }
    PaperClaim cs[PAPER_MAX_CLAIMS];
    size_t n = paper_extract(ai, txt, cs, PAPER_MAX_CLAIMS);
    free(txt);
    txt = NULL;
    if (n == 0) {
        if (note && cap) snprintf(note, cap, "从 %s 里一**条三段式断言**都没抽到（我不猜）", path);
        return 0;
    }
    size_t m = paper_to_claims(ai, cs, n);
    if (made) *made = m;
    if (note && cap) {
        snprintf(note, cap, "%s：抽到 %zu 条，记为待证 %zu 条（**都不算数**）", path, n, m);
        for (size_t i = 0; i < n && i < 8; ++i) {
            size_t o = strlen(note);
            if (o + 96 < cap)
                snprintf(note + o, cap - o, "\n  · %.39s %.23s %.39s", cs[i].a, cs[i].r, cs[i].b);
        }
    }
    return n;
}

#endif /* PAPER_H */
