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

/* 从句子 [s, s+len) 里抽一条断言：**段数 3–6**、中间择一个关系词、两边各拼成主语/宾语（各 ≤2 段）。
 * 判据全在 `paper_segments` 的结果上做（切分与抽取分开，便于单测）。 */
static inline int paper_sentence_claim(AI *ai, const char *s, size_t len, PaperClaim *out) {
    if (!ai || !s || len == 0 || !out) return 0;
    char seg[8][64];
    size_t sn = paper_segments(s, len, seg, 8);
    if (sn < 3 || sn > 6) return 0;
    /* ⚠ 只检查**主/宾**段，不检查关系词段：`在/对/从` 这些在 `paper_word_ok` 的虚词表里
     * （它们不该当主语），但它们**正当关系词** —— 上一轮把整句一刀切掉，四段句一条都抽不到。 */
    /* 候选关系词（从右往左找第一个"两边都不超过 2 段"的） */
    int ri = -1;
    for (int i = (int)sn - 2; i >= 1; --i) {
        if (!rel_id_of(ai, seg[i], 0)) continue;
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
    snprintf(out->a, sizeof out->a, "%.39s", a);
    snprintf(out->r, sizeof out->r, "%.23s", seg[ri]);
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

/* §6.663 **一条入口**：读文件 → 抽断言 → 写待证。`::paper read` 与 `::paper fetch` 共用，
 * 不再各自复制一遍（上一轮的重复代码就是两次实现开始漂移的起点）。
 * 返回抽到的断言数；`*made` 是真正写进待证的条数；note 里是给人看的说明。 */
static inline size_t paper_ingest(AI *ai, const char *path, size_t *made, char *note, size_t cap) {
    if (made) *made = 0;
    if (!ai || !path || !path[0]) {
        if (note && cap) snprintf(note, cap, "没给文件");
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (note && cap) snprintf(note, cap, "读不到 %s（只读工作区里的纯文本）", path);
        return 0;
    }
    static char txt[PAPER_MAX_BYTES + 1];
    size_t got = fread(txt, 1, PAPER_MAX_BYTES, f);
    int big = !feof(f);
    fclose(f);
    txt[got] = 0;
    if (big) {
        if (note && cap) snprintf(note, cap, "%s 超过 %u KB，拒绝（先切一段）", path, PAPER_MAX_BYTES / 1024);
        return 0;
    }
    PaperClaim cs[PAPER_MAX_CLAIMS];
    size_t n = paper_extract(ai, txt, cs, PAPER_MAX_CLAIMS);
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
