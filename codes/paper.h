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

#define PAPER_MAX_BYTES  (256u * 1024u)
#define PAPER_MAX_CLAIMS 64

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

/* 从句子 [s, s+len) 里抽一条断言：词序形如 `A R B`（关系词必须已在关系表里）。
 * 成功返回 1，并填 out。**只认"恰好三段"**：多了就不猜（宁可漏，不要编）。 */
static inline int paper_sentence_claim(AI *ai, const char *s, size_t len, PaperClaim *out) {
    if (!ai || !s || len == 0 || !out) return 0;
    char buf[256];
    if (len >= sizeof buf) return 0;
    memcpy(buf, s, len); buf[len] = 0;
    /* ⚠ **先按空格切**：论文样本里的断言是"人写好的短句"（`企鹅 属于 鸟类`）。
     * 先分词的话，没进过词池的 `企鹅` 会被切成 企/鹅 ⇒ 三段变五段 ⇒ 一条都抽不到（第一版就是 0 条）。
     * 空格切不出三段时再退回分词器（真论文没有空格，这条路径基本抽不到 —— 照实说）。 */
    char *sp[8]; size_t sn = 0;
    {
        char *t = buf;
        while (*t && sn < 8) {
            while (*t == ' ' || *t == '\t') t++;
            if (!*t) break;
            sp[sn++] = t;
            while (*t && *t != ' ' && *t != '\t') t++;
            if (*t) { *t = 0; t++; }
        }
    }
    char words[8][64]; size_t wn = 0;
    if (sn == 3) {
        for (size_t i = 0; i < 3; ++i) {
            if (!paper_word_ok(sp[i])) return 0;
            snprintf(words[wn], sizeof words[0], "%s", sp[i]);
            wn++;
        }
    } else {
        size_t n = 0;
        char **w = ai_tokenize(ai, buf, &n);
        if (!w) return 0;
        for (size_t i = 0; i < n && wn < 8; ++i) {
            if (!paper_word_ok(w[i])) continue;
            snprintf(words[wn], sizeof words[0], "%s", w[i]);
            wn++;
        }
        ai_free_words(w, n);
    }
    if (wn != 3) return 0;                       /* 恰好 A R B，否则不猜 */
    uint8_t rid = rel_id_of(ai, words[1], 0);
    if (!rid) return 0;                          /* 中间那个必须是已知关系词 */
    /* 主语/宾语不能是虚词（`paper_word_ok` 已经滤过） */
    if (strlen(words[0]) < 3 || strlen(words[2]) < 3) return 0;   /* 单字太容易误抽 */
    snprintf(out->a, sizeof out->a, "%.39s", words[0]);
    snprintf(out->r, sizeof out->r, "%.23s", words[1]);
    snprintf(out->b, sizeof out->b, "%.39s", words[2]);
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

#endif /* PAPER_H */
