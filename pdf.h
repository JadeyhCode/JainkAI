/* pdf.h — **PDF 取文字**（不靠外部命令；§6.678/§6.679）
 *
 * 为什么自己写：`pdftotext` 是外部程序，而这一层要能在**内核里**读论文 —— 走 `::act` 那条
 * 白名单路子等于把"读论文"变成"跑外部命令"，边界和账都对不上（谁的输出算观测？）。
 * PDF 取文字的核心只有两件事：**解 FlateDecode**（inflate.h）+ **认内容流的文本算子**。
 *
 * 做法（有意从简，全部照实说）：
 *   · **不解析 xref**：直接扫整份文件里的 `N 0 obj` 和 `stream ... endstream`。好处是对损坏的
 *     xref / 增量更新 / 对象流（ObjStm）全都免疫（不少生成器写的 xref 是错的）；
 *     坏处是**对象边界靠"下一个 obj"推断**，怪文件里会错位（错位了就少读几个流，不会崩）。
 *   · 只认 `FlateDecode`（和"没压缩"两种）。`LZW`/`ASCII85`/`RunLength`/`DCTDecode` 一概不认，
 *     碰到就在 note 里报"跳过几个流"（不静默跳过 —— 静默跳过就是假装读过了）。
 *   · 文本算子只认：`BT`/`ET`、`Tj`/`TJ`/`'`/`"`、`Td`/`TD`/`T*`/`Tm`、`Tf`。`Td` 看**两个操作数**：
 *     dy≠0 出换行、dy=0 且 dx>0 出一个空格（TeX 的 PDF 里词与词就是靠 dx 分开的，
 *     一律换行会把整篇论文切成"一行一个词"，下游一句断言都抽不到 —— 第一版就栽在这）。
 *   · 编码：**按字体**用各自的 `ToUnicode` CMap（`Tf` 切字体；支持 beginbfchar/beginbfrange）。
 *     第一版把所有 CMap **合并**成一张表 ⇒ 数学字体的码位把正文的数字/字母顶掉（实测
 *     arXiv 1706.03762：`Ashish` 变成 `shish`、`arXiv:1706.03762` 变成 `arXiv.←←`）。
 *     名字解不出来时**不猜**：退回 Latin-1 直译，并在 note 里报"几段没有字体映射"。
 *   · 加密（`/Encrypt`）、扫描件（纯图片）**明说读不出**，不猜。
 *
 * 明确不做：版面还原（分栏、表格、公式）、连字/断词合并、字形宽度推算、CID 字体码位推断、
 * `/Differences` 编码表、图片 OCR、书签/元数据。要的是"排版引擎"或"模型"，不是这一层。
 */
#ifndef PDF_H
#define PDF_H

#include "inflate.h"

#define PDF_MAX_FILE   (6u * 1024u * 1024u)   /* 单份 PDF 上限 */
#define PDF_STREAM_MAX (4u * 1024u * 1024u)   /* 单个流解压后上限 */
#define PDF_MAX_TEXT   (2u * 1024u * 1024u)   /* 抽出来的文字上限 */
#define PDF_UMAP_MAX   512                    /* 单张 CMap 的映射条数上限 */
#define PDF_MAX_OBJ    8192                   /* 对象表上限（超大文件截断，note 里报） */
#define PDF_MAX_FONTS  48                     /* 能用上 ToUnicode 的字体数上限 */
#define PDF_MAX_NAMES  128                    /* 资源名 → 字体 的条数上限 */

typedef struct {
    unsigned code;
    int width;          /* 源编码宽度：1 或 2 字节 */
    char u[12];         /* 映射到的 UTF-8（≤ 11 字节 + 0） */
} PdfUmap;

typedef struct {
    PdfUmap e[PDF_UMAP_MAX];
    size_t n;
    int any;
    int width;          /* 整张表的编码宽度（2 = Identity-H 那种双字节码） */
} PdfMap;

typedef struct {
    int num;            /* 对象号 */
    size_t start, end;  /* 对象体 [start, end)（到下一个 obj 之前） */
} PdfObj;

typedef struct {
    int obj;            /* 字体对象号 */
    int cmap_obj;       /* 它的 ToUnicode 对象号（0 = 没有） */
    PdfMap map;
} PdfFont;

typedef struct {
    char name[24];      /* 资源名，如 F84（不带斜杠） */
    int obj;            /* 指向的字体对象号 */
} PdfFName;

typedef struct {
    PdfFont *ft[PDF_MAX_FONTS];
    size_t nft;
    PdfFName nm[PDF_MAX_NAMES];
    size_t nnm;
    int n_double;       /* 宽度为 2 的表有几个字体 */
} PdfFonts;

/* UTF-8 编码一个码位，返回字节数（0 = 非法） */
static inline int pdf_utf8(unsigned cp, char *b) {
    if (cp < 0x80u) { b[0] = (char)cp; return 1; }
    if (cp < 0x800u) { b[0] = (char)(0xC0u | (cp >> 6)); b[1] = (char)(0x80u | (cp & 0x3Fu)); return 2; }
    if (cp < 0x10000u) {
        b[0] = (char)(0xE0u | (cp >> 12));
        b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp < 0x110000u) {
        b[0] = (char)(0xF0u | (cp >> 18));
        b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

static inline const unsigned char *pdf_memmem(const unsigned char *h, size_t hn,
                                              const char *nd, size_t nn) {
    if (!h || !nd || nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; ++i) {
        if (h[i] == (unsigned char)nd[0] && memcmp(h + i, nd, nn) == 0) return h + i;
    }
    return NULL;
}

static inline int pdf_isw(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline int pdf_hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 数字：PDF 的实数只有 `[+-]?digits[.digits]`（没有指数） */
static inline int pdf_number_at(const char *s, size_t i, size_t len, double *v, size_t *next) {
    size_t p = i;
    int neg = 0;
    if (p < len && (s[p] == '+' || s[p] == '-')) { neg = (s[p] == '-'); p++; }
    size_t d0 = p;
    double x = 0.0;
    while (p < len && s[p] >= '0' && s[p] <= '9') { x = x * 10.0 + (s[p] - '0'); p++; }
    if (p < len && s[p] == '.') {
        p++;
        double f = 0.1;
        while (p < len && s[p] >= '0' && s[p] <= '9') { x += (s[p] - '0') * f; f *= 0.1; p++; }
    }
    if (p == d0) return 0;                       /* 连一位数字都没有 */
    *v = neg ? -x : x;
    *next = p;
    return 1;
}

/* "12 0 R" 这种间接引用：从 i 起找一个整数 + " 0 R"，返回对象号（0 = 没找到） */
static inline int pdf_ref_at(const unsigned char *b, size_t len, size_t i) {
    while (i < len && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) i++;
    int num = 0, k = 0;
    while (i < len && b[i] >= '0' && b[i] <= '9' && k < 8) { num = num * 10 + (b[i] - '0'); i++; k++; }
    if (k == 0) return 0;
    while (i < len && (b[i] == ' ' || b[i] == '\t')) i++;
    if (i >= len || b[i] != '0') return 0;
    i++;
    while (i < len && (b[i] == ' ' || b[i] == '\t')) i++;
    if (i >= len || b[i] != 'R') return 0;
    return num;
}

/* 字面串 `( ... )`：处理 `\n \r \t \b \f \( \) \\ \ddd`、行尾续行、括号配对 */
static inline size_t pdf_lit_string(const char *s, size_t i, size_t len,
                                    unsigned char *out, size_t ocap, size_t *outn) {
    size_t n = 0;
    int depth = 1;
    if (i < len && s[i] == '(') i++;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            i++;
            if (i >= len) break;
            unsigned char e = (unsigned char)s[i];
            if (e == 'n') { e = '\n'; i++; }
            else if (e == 'r') { e = '\r'; i++; }
            else if (e == 't') { e = '\t'; i++; }
            else if (e == 'b') { e = '\b'; i++; }
            else if (e == 'f') { e = '\f'; i++; }
            else if (e == '\n') { i++; continue; }            /* 续行：真的什么都不加 */
            else if (e == '\r') { i++; if (i < len && s[i] == '\n') i++; continue; }
            else if (e >= '0' && e <= '7') {
                unsigned v = 0;
                int k = 0;
                while (k < 3 && i < len && s[i] >= '0' && s[i] <= '7') {
                    v = v * 8u + (unsigned)(s[i] - '0');
                    i++;
                    k++;
                }
                e = (unsigned char)(v & 0xFFu);
            } else { i++; }                                   /* `\(` `\)` `\\` 等：原样 */
            if (n < ocap) out[n++] = e;
            continue;
        }
        if (c == '(') { depth++; if (n < ocap) out[n++] = c; i++; continue; }
        if (c == ')') {
            depth--;
            i++;
            if (depth == 0) break;
            if (n < ocap) out[n++] = c;
            continue;
        }
        if (n < ocap) out[n++] = c;
        i++;
    }
    *outn = n;
    return i;
}

/* 十六进制串 `< ... >`（奇数位按 0 补齐，RFC 口径） */
static inline size_t pdf_hex_string(const char *s, size_t i, size_t len,
                                    unsigned char *out, size_t ocap, size_t *outn) {
    size_t n = 0;
    int hi = -1;
    if (i < len && s[i] == '<') i++;
    while (i < len && s[i] != '>') {
        int v = pdf_hexval((unsigned char)s[i]);
        i++;
        if (v < 0) continue;                                  /* 空白等 */
        if (hi < 0) { hi = v; continue; }
        if (n < ocap) out[n++] = (unsigned char)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0 && n < ocap) out[n++] = (unsigned char)(hi << 4);
    if (i < len && s[i] == '>') i++;
    *outn = n;
    return i;
}

/* ---- ToUnicode CMap ---- */
static inline void pdf_map_add(PdfMap *m, unsigned code, int width, const char *u, size_t ul) {
    if (!m || m->n >= PDF_UMAP_MAX || ul == 0) return;
    if (ul > 11) ul = 11;
    PdfUmap *e = &m->e[m->n];
    e->code = code;
    e->width = width;
    for (size_t k = 0; k < ul; ++k) e->u[k] = u[k];
    e->u[ul] = 0;
    m->n++;
    m->any = 1;
    if (width == 2) m->width = 2;
}

static inline const PdfUmap *pdf_map_find(const PdfMap *m, unsigned code) {
    if (!m || !m->any) return NULL;
    for (size_t i = m->n; i-- > 0;) if (m->e[i].code == code) return &m->e[i];
    return NULL;
}

/* UTF-16BE 的字节串 → UTF-8（代理对也合一下） */
static inline size_t pdf_utf16be_to_utf8(const unsigned char *p, size_t n, char *o, size_t ocap) {
    size_t k = 0, u = 0;
    while (k + 1 < n) {
        unsigned cp = ((unsigned)p[k] << 8) | p[k + 1];
        k += 2;
        if (cp >= 0xD800u && cp <= 0xDBFFu && k + 1 < n) {
            unsigned lo = ((unsigned)p[k] << 8) | p[k + 1];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                k += 2;
            }
        }
        char b[4];
        int bl = pdf_utf8(cp, b);
        if (bl <= 0) continue;
        if (u + (size_t)bl > ocap) break;
        memcpy(o + u, b, (size_t)bl);
        u += (size_t)bl;
    }
    return u;
}

/* 扫描一段（已解压的）ToUnicode CMap 流，把映射塞进 m */
static inline void pdf_scan_cmap(const unsigned char *s, size_t len, PdfMap *m) {
    if (!s || !len || !m) return;
    const unsigned char *end = s + len;
    const unsigned char *p = s;
    while (p < end) {
        const unsigned char *b = pdf_memmem(p, (size_t)(end - p), "beginbfchar", 11);
        if (!b) break;
        const unsigned char *e = pdf_memmem(b, (size_t)(end - b), "endbfchar", 9);
        if (!e) break;
        const unsigned char *q = b + 11;
        while (q < e) {
            unsigned char src[4], dst[16];
            size_t sn = 0, dn = 0;
            while (q < e && *q != '<') q++;
            if (q >= e) break;
            q = s + pdf_hex_string((const char *)s, (size_t)(q - s), (size_t)(e - s), src, sizeof src, &sn);
            while (q < e && *q != '<') q++;
            if (q >= e) break;
            q = s + pdf_hex_string((const char *)s, (size_t)(q - s), (size_t)(e - s), dst, sizeof dst, &dn);
            if (sn == 1 || sn == 2) {
                unsigned code = (sn == 1) ? src[0] : (((unsigned)src[0] << 8) | src[1]);
                char u[12];
                size_t ul = pdf_utf16be_to_utf8(dst, dn, u, sizeof u);
                pdf_map_add(m, code, (int)sn, u, ul);
            }
        }
        p = e + 9;
    }
    p = s;
    while (p < end) {
        const unsigned char *b = pdf_memmem(p, (size_t)(end - p), "beginbfrange", 12);
        if (!b) break;
        const unsigned char *e = pdf_memmem(b, (size_t)(end - b), "endbfrange", 10);
        if (!e) break;
        const unsigned char *q = b + 12;
        while (q < e) {
            unsigned char lo[4], hi[4];
            size_t ln = 0, hn = 0;
            while (q < e && *q != '<') q++;
            if (q >= e) break;
            q = s + pdf_hex_string((const char *)s, (size_t)(q - s), (size_t)(e - s), lo, sizeof lo, &ln);
            while (q < e && *q != '<') q++;
            if (q >= e) break;
            q = s + pdf_hex_string((const char *)s, (size_t)(q - s), (size_t)(e - s), hi, sizeof hi, &hn);
            while (q < e && *q != '<' && *q != '[') q++;
            if (q >= e || ln == 0 || hn == 0) break;
            unsigned c0 = (ln == 1) ? lo[0] : (((unsigned)lo[0] << 8) | lo[1]);
            unsigned c1 = (hn == 1) ? hi[0] : (((unsigned)hi[0] << 8) | hi[1]);
            if (c1 < c0) { unsigned t = c0; c0 = c1; c1 = t; }
            if (c1 - c0 > 4096u) c1 = c0 + 4096u;              /* 上限：别被坏 CMap 拖死 */
            int w = (ln == 1) ? 1 : 2;
            if (*q == '[') {
                q++;
                for (unsigned c = c0; c <= c1; ++c) {
                    unsigned char dst[16];
                    size_t dn = 0;
                    while (q < e && *q != '<' && *q != ']') q++;
                    if (q >= e || *q == ']') break;
                    q = s + pdf_hex_string((const char *)s, (size_t)(q - s), (size_t)(e - s),
                                           dst, sizeof dst, &dn);
                    char u[12];
                    size_t ul = pdf_utf16be_to_utf8(dst, dn, u, sizeof u);
                    pdf_map_add(m, c, w, u, ul);
                }
            } else {
                unsigned char dst[16];
                size_t dn = 0;
                q = s + pdf_hex_string((const char *)s, (size_t)(q - s), (size_t)(e - s),
                                       dst, sizeof dst, &dn);
                if (dn >= 2) {
                    unsigned base = ((unsigned)dst[dn - 2] << 8) | dst[dn - 1];
                    for (unsigned c = c0; c <= c1; ++c) {
                        unsigned x = base + (c - c0);
                        dst[dn - 2] = (unsigned char)(x >> 8);
                        dst[dn - 1] = (unsigned char)(x & 0xFFu);
                        char u[12];
                        size_t ul = pdf_utf16be_to_utf8(dst, dn, u, sizeof u);
                        pdf_map_add(m, c, w, u, ul);
                    }
                }
            }
        }
        p = e + 10;
    }
}

/* ---- 对象表：`N G obj` 的位置 ----
 * ⚠ 头部是**两个**数：`5 0 obj` 里 5 是对象号、0 是代号。第一版只往回读了一个数 ⇒
 * 对象号全成 0（`pdf_obj_find` 全错，字体一张都认不出来，中文 PDF 直接退化成 Latin-1）。
 * 往回读两段数字，对象体从上一次的对象号开头算起。 */
static inline size_t pdf_obj_table(const unsigned char *buf, size_t len,
                                   PdfObj *tab, size_t cap, int *truncated) {
    size_t n = 0;
    if (truncated) *truncated = 0;
    for (size_t i = 0; i + 5 < len; ++i) {
        if (buf[i] != 'o' || memcmp(buf + i, "obj", 3) != 0) continue;
        size_t k = i;
        if (k == 0) continue;
        unsigned char b = buf[k - 1];
        if (b != ' ' && b != '\t' && b != '\r' && b != '\n') continue;
        size_t q = k;
        while (q > 0 && (buf[q - 1] == ' ' || buf[q - 1] == '\t' ||
                         buf[q - 1] == '\r' || buf[q - 1] == '\n')) q--;
        size_t ge = q;                                  /* 代号（gen）末尾 */
        while (q > 0 && buf[q - 1] >= '0' && buf[q - 1] <= '9') q--;
        if (q == ge) continue;                          /* 没有 gen 数：不是对象头 */
        size_t q2 = q;
        while (q2 > 0 && (buf[q2 - 1] == ' ' || buf[q2 - 1] == '\t' ||
                          buf[q2 - 1] == '\r' || buf[q2 - 1] == '\n')) q2--;
        size_t ne = q2;                                 /* 对象号末尾 */
        while (q2 > 0 && buf[q2 - 1] >= '0' && buf[q2 - 1] <= '9') q2--;
        if (q2 == ne) continue;
        int num = 0;
        for (size_t j = q2; j < ne; ++j) num = num * 10 + (buf[j] - '0');
        if (n < cap) {
            if (n > 0) tab[n - 1].end = q2;             /* 上一个对象到此为止 */
            tab[n].num = num;
            tab[n].start = k + 3;
            tab[n].end = 0;
            n++;
        } else if (truncated) {
            *truncated = 1;
        }
        i += 2;
    }
    if (n > 0 && tab[n - 1].end == 0) tab[n - 1].end = len;
    return n;
}

static inline const PdfObj *pdf_obj_find(const PdfObj *tab, size_t n, int num) {
    for (size_t i = 0; i < n; ++i) if (tab[i].num == num) return &tab[i];
    return NULL;
}

/* 取某个对象里的流数据 [d0,d1)；没有 stream 返回 0 */
static inline int pdf_obj_stream(const unsigned char *buf, size_t len, const PdfObj *o,
                                 size_t *d0, size_t *d1) {
    if (!o || o->start >= len || o->end <= o->start || o->end > len) return 0;
    const unsigned char *st = pdf_memmem(buf + o->start, o->end - o->start, "stream", 6);
    if (!st) return 0;
    size_t s0 = (size_t)(st - buf) + 6;
    if (s0 < len && buf[s0] == '\r') s0++;
    if (s0 < len && buf[s0] == '\n') s0++;
    const unsigned char *en = pdf_memmem(buf + s0, o->end - s0, "endstream", 9);
    if (!en) return 0;
    *d0 = s0;
    *d1 = (size_t)(en - buf);
    return 1;
}

/* 把对象 o 的流（FlateDecode 或裸）解到 dst，返回字节数（0 = 解不开） */
static inline size_t pdf_obj_data(const unsigned char *buf, size_t len, const PdfObj *o,
                                  unsigned char *dst, size_t dcap) {
    size_t d0 = 0, d1 = 0;
    if (!pdf_obj_stream(buf, len, o, &d0, &d1) || d1 <= d0) return 0;
    int flate = 0;
    for (size_t k = o->start; k + 12 < d0; ++k)
        if (memcmp(buf + k, "/FlateDecode", 12) == 0) { flate = 1; break; }
    size_t dn = 0;
    if (flate) {
        char e[96];
        if (inf_unzlib_ex(buf + d0, d1 - d0, dst, dcap, &dn, NULL, e, sizeof e) != 0) return 0;
        return dn;
    }
    dn = d1 - d0;
    if (dn > dcap) return 0;
    memcpy(dst, buf + d0, dn);
    return dn;
}

/* ---- 字体表：字体对象 → 它的 ToUnicode；资源名 → 字体对象 ---- */
static inline void pdf_build_fonts(const unsigned char *buf, size_t len, const PdfObj *tab, size_t nt,
                                   PdfFonts *fs, unsigned char *tmp, size_t tcap) {
    fs->nft = 0;
    fs->nnm = 0;
    fs->n_double = 0;
    if (!tab || nt == 0) return;
    for (size_t i = 0; i < nt && fs->nft < PDF_MAX_FONTS; ++i) {
        const PdfObj *o = &tab[i];
        if (o->end <= o->start) continue;
        size_t w = o->end - o->start;
        if (w > 2048) w = 2048;
        if (!pdf_memmem(buf + o->start, w, "/Type /Font", 11) &&
            !pdf_memmem(buf + o->start, w, "/Subtype /Type0", 15) &&
            !pdf_memmem(buf + o->start, w, "/Type/Font", 10)) continue;
        const unsigned char *tu = pdf_memmem(buf + o->start, w, "/ToUnicode", 10);
        if (!tu) continue;
        int cn = pdf_ref_at(buf, len, (size_t)(tu - buf) + 10);
        if (cn <= 0) continue;
        const PdfObj *co = pdf_obj_find(tab, nt, cn);
        if (!co) continue;
        PdfFont *f = (PdfFont *)malloc(sizeof(PdfFont));
        if (!f) return;
        memset(f, 0, sizeof *f);
        f->obj = o->num;
        f->cmap_obj = cn;
        size_t dn = pdf_obj_data(buf, len, co, tmp, tcap);
        if (dn == 0) { free(f); continue; }
        if (!pdf_memmem(tmp, dn, "beginbfchar", 11) && !pdf_memmem(tmp, dn, "beginbfrange", 12)) {
            free(f);
            continue;
        }
        pdf_scan_cmap(tmp, dn, &f->map);
        if (!f->map.any) { free(f); continue; }
        fs->ft[fs->nft++] = f;
        if (f->map.width == 2) fs->n_double++;
    }
    if (fs->nft == 0) return;
    /* 资源名 → 字体对象：扫 `/Font << /F1 12 0 R ... >>` */
    for (size_t i = 0; i + 6 < len && fs->nnm < PDF_MAX_NAMES; ++i) {
        if (memcmp(buf + i, "/Font", 5) != 0) continue;
        size_t avail = len - i - 5;
        if (avail > 4096) avail = 4096;
        const unsigned char *lt = pdf_memmem(buf + i + 5, avail, "<<", 2);
        if (!lt) continue;
        size_t gend = (size_t)(buf + len - lt);
        const unsigned char *gt = pdf_memmem(lt, gend, ">>", 2);
        if (!gt) continue;
        size_t gpos = (size_t)(gt - buf);
        size_t q = (size_t)(lt - buf) + 2;
        for (;;) {
            while (q < gpos && buf[q] != '/') q++;
            if (q >= gpos) break;
            q++;
            char name[24];
            size_t nl = 0;
            while (q < gpos && buf[q] != ' ' && buf[q] != '\t' && buf[q] != '\r' &&
                   buf[q] != '\n' && buf[q] != '/' && buf[q] != '>' && nl < 23) {
                name[nl++] = (char)buf[q++];
            }
            name[nl] = 0;
            int on = pdf_ref_at(buf, gpos, q);
            if (on > 0 && nl > 0 && pdf_obj_find(tab, nt, on) && fs->nnm < PDF_MAX_NAMES) {
                int dup = 0;
                for (size_t k = 0; k < fs->nnm; ++k) if (!strcmp(fs->nm[k].name, name)) { dup = 1; break; }
                if (!dup) {
                    snprintf(fs->nm[fs->nnm].name, sizeof fs->nm[0].name, "%s", name);
                    fs->nm[fs->nnm].obj = on;
                    fs->nnm++;
                }
            }
        }
        i = gpos;
    }
}

static inline const PdfFont *pdf_font_by_name(const PdfFonts *fs, const char *name) {
    if (!fs || !name || !name[0]) return NULL;
    int obj = 0;
    for (size_t i = 0; i < fs->nnm; ++i) if (!strcmp(fs->nm[i].name, name)) { obj = fs->nm[i].obj; break; }
    if (!obj) return NULL;
    for (size_t i = 0; i < fs->nft; ++i) if (fs->ft[i]->obj == obj) return fs->ft[i];
    return NULL;
}

static inline void pdf_fonts_free(PdfFonts *fs) {
    if (!fs) return;
    for (size_t i = 0; i < fs->nft; ++i) free(fs->ft[i]);
    fs->nft = 0;
}

/* ---- 一串字符码 → UTF-8 追加到输出 ---- */
static inline void pdf_emit_codes(const unsigned char *b, size_t n, char *out, size_t *on,
                                  size_t ocap, const PdfMap *m) {
    size_t i = 0;
    int wide = (m && m->any && m->width == 2);
    while (i < n) {
        const PdfUmap *e = NULL;
        if (m && m->any) {
            if (wide && i + 1 < n) e = pdf_map_find(m, ((unsigned)b[i] << 8) | b[i + 1]);
            else e = pdf_map_find(m, b[i]);
        }
        if (e) {
            size_t ul = strlen(e->u);
            if (*on + ul < ocap) { memcpy(out + *on, e->u, ul); *on += ul; }
            i += (size_t)(e->width < 1 ? 1 : e->width);
            continue;
        }
        char u[4];
        int ul = pdf_utf8(b[i], u);
        if (ul > 0 && *on + (size_t)ul < ocap) { memcpy(out + *on, u, (size_t)ul); *on += (size_t)ul; }
        i++;
    }
}

static inline void pdf_nl(char *out, size_t *on, size_t ocap) {
    if (*on == 0) return;
    if (out[*on - 1] == '\n') return;
    if (*on + 1 < ocap) out[(*on)++] = '\n';
}

static inline void pdf_sp(char *out, size_t *on, size_t ocap) {
    if (*on == 0) return;
    char c = out[*on - 1];
    if (c == '\n' || c == ' ') return;
    if (*on + 1 < ocap) out[(*on)++] = ' ';
}

/* 解析一段内容流里的文本算子。返回抽到的可见字符数（0 = 这段里没文字）。
 * `unmapped` 记"用了 Latin-1 直译"的串数（照实说用）。 */
static inline size_t pdf_parse_content(const char *s, size_t len, char *out, size_t *on,
                                       size_t ocap, const PdfFonts *fs, size_t *unmapped) {
    size_t i = 0, before = *on;
    int in_bt = 0, in_array = 0, nnum = 0;
    double nums[2] = { 0.0, 0.0 };
    char nm[24] = { 0 };
    const PdfMap *cur = NULL;
    while (i < len && *on + 2 < ocap) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%') {                                        /* 注释：到行尾 */
            while (i < len && s[i] != '\n' && s[i] != '\r') i++;
            continue;
        }
        if (c == '/') {                                        /* 名字（Tf 的操作数在这里） */
            i++;
            size_t nl = 0;
            while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n' &&
                   s[i] != '/' && s[i] != '[' && s[i] != ']' && s[i] != '<' && s[i] != '>' &&
                   s[i] != '(' && s[i] != ')' && nl < 23) {
                nm[nl++] = s[i++];
            }
            nm[nl] = 0;
            nnum = 0;
            continue;
        }
        if (c == '(') {
            unsigned char buf[4096];
            size_t bn = 0;
            i = pdf_lit_string(s, i, len, buf, sizeof buf, &bn);
            if (in_bt) {
                if (!cur && unmapped) (*unmapped)++;
                pdf_emit_codes(buf, bn, out, on, ocap, cur);
            }
            nnum = 0;
            continue;
        }
        if (c == '<' && !(i + 1 < len && s[i + 1] == '<')) {
            unsigned char buf[4096];
            size_t bn = 0;
            i = pdf_hex_string(s, i, len, buf, sizeof buf, &bn);
            if (in_bt) {
                if (!cur && unmapped) (*unmapped)++;
                pdf_emit_codes(buf, bn, out, on, ocap, cur);
            }
            nnum = 0;
            continue;
        }
        if (c == '<' && i + 1 < len && s[i + 1] == '<') { i += 2; continue; }   /* 字典：跳过 */
        if (c == '>') { i++; continue; }
        if (c == '[') { in_array = 1; i++; nnum = 0; continue; }
        if (c == ']') { in_array = 0; i++; continue; }
        if (c == '{' || c == '}' || c == ')' || c == ' ' || c == '\t' ||
            c == '\r' || c == '\n') { i++; continue; }
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            double v = 0.0;
            size_t nx = i;
            if (pdf_number_at(s, i, len, &v, &nx)) {
                /* TJ 数组里的负数 = 往右退（字距拉开）⇒ 当成空格；>= -100 的只是微调 */
                if (in_array && in_bt && v <= -100.0) pdf_sp(out, on, ocap);
                nums[0] = nums[1];
                nums[1] = v;
                if (nnum < 2) nnum++;
                i = nx;
                continue;
            }
        }
        if (pdf_isw(c) || c == '\'' || c == '"' || c == '*') {
            size_t st = i;
            while (i < len && (pdf_isw((unsigned char)s[i]) || s[i] == '*' ||
                               s[i] == '\'' || s[i] == '"')) i++;
            size_t tl = i - st;
            const char *t = s + st;
            if (tl == 2 && t[0] == 'B' && t[1] == 'T') { in_bt = 1; pdf_nl(out, on, ocap); }
            else if (tl == 2 && t[0] == 'E' && t[1] == 'T') { in_bt = 0; pdf_nl(out, on, ocap); }
            else if (tl == 2 && t[0] == 'T' && t[1] == 'f') {
                const PdfFont *f = pdf_font_by_name(fs, nm);
                cur = f ? &f->map : NULL;
            }
            else if (tl == 2 && t[0] == 'T' && t[1] == 'd') {
                if (in_bt && nnum >= 2) {
                    double dx = nums[0], dy = nums[1];
                    if (dy > 0.5 || dy < -0.5) pdf_nl(out, on, ocap);
                    else if (dx > 0.5) pdf_sp(out, on, ocap);
                }
            } else if (tl == 2 && t[0] == 'T' && t[1] == 'D') {
                if (in_bt && nnum >= 2 && (nums[1] > 0.5 || nums[1] < -0.5)) pdf_nl(out, on, ocap);
            } else if (tl == 2 && t[0] == 'T' && t[1] == '*') {
                if (in_bt) pdf_nl(out, on, ocap);
            } else if (tl == 2 && t[0] == 'T' && t[1] == 'm') {
                if (in_bt) pdf_nl(out, on, ocap);
            } else if (tl == 1 && (t[0] == '\'' || t[0] == '"')) {
                if (in_bt) pdf_nl(out, on, ocap);
            }
            /* `Tj`/`TJ`/`Tc` … 一律不产生空白：空格由 Td/TJ 的间距决定 */
            nnum = 0;
            continue;
        }
        i++;
    }
    return *on - before;
}

/* 在 buf 里从 *pos 起找下一个 `stream ... endstream`，给出数据区间 [d0, d1) 与字典窗口起点 */
static inline int pdf_next_stream(const unsigned char *buf, size_t len, size_t *pos,
                                  size_t *d0, size_t *d1, size_t *dict0) {
    if (*pos >= len) return 0;
    const unsigned char *st = pdf_memmem(buf + *pos, len - *pos, "stream", 6);
    if (!st) { *pos = len; return 0; }
    size_t off = (size_t)(st - buf);
    size_t s0 = off + 6;
    if (s0 < len && buf[s0] == '\r') s0++;
    if (s0 < len && buf[s0] == '\n') s0++;
    const unsigned char *en = pdf_memmem(buf + s0, len - s0, "endstream", 9);
    if (!en) { *pos = len; return 0; }
    *d0 = s0;
    *d1 = (size_t)(en - buf);
    *dict0 = (off > 1024) ? off - 1024 : 0;
    *pos = (size_t)(en - buf) + 9;
    return 1;
}

/* 主入口：从 PDF 字节里抽文字。返回写进 out 的字节数（0 = 没抽到）。 */
static inline size_t pdf_extract_text(const unsigned char *buf, size_t len, char *out, size_t ocap,
                                      size_t *decoded, size_t *skipped, char *note, size_t ncap) {
    if (decoded) *decoded = 0;
    if (skipped) *skipped = 0;
    if (!buf || !out || ocap < 64 || len < 8) return 0;
    out[0] = 0;
    if (pdf_memmem(buf, len, "%PDF-", 5) == NULL) {
        if (note && ncap) snprintf(note, ncap, "不是 PDF（文件头里没有 %%PDF-）");
        return 0;
    }
    if (pdf_memmem(buf, len, "/Encrypt", 8) != NULL) {
        if (note && ncap) snprintf(note, ncap, "这份 PDF 加密了（有 /Encrypt）—— 我不会解密，不猜内容");
        return 0;
    }
    unsigned char *zbuf = (unsigned char *)malloc(PDF_STREAM_MAX);
    unsigned char *tbuf = (unsigned char *)malloc(PDF_STREAM_MAX);
    PdfObj *tab = (PdfObj *)malloc(sizeof(PdfObj) * PDF_MAX_OBJ);
    PdfFonts *fs = (PdfFonts *)malloc(sizeof(PdfFonts));
    if (!zbuf || !tbuf || !tab || !fs) {
        free(zbuf); free(tbuf); free(tab); free(fs);
        if (note && ncap) snprintf(note, ncap, "内存不够（要 %u KB × 2 + 表和字体）", PDF_STREAM_MAX / 1024);
        return 0;
    }
    memset(fs, 0, sizeof *fs);
    int trunctab = 0;
    size_t nt = pdf_obj_table(buf, len, tab, PDF_MAX_OBJ, &trunctab);
    pdf_build_fonts(buf, len, tab, nt, fs, tbuf, PDF_STREAM_MAX);

    size_t on = 0, ndec = 0, nskip = 0, nflate = 0, nunmapped = 0, nf_miss = 0;
    size_t pos = 0, d0 = 0, d1 = 0, dict0 = 0;
    while (pdf_next_stream(buf, len, &pos, &d0, &d1, &dict0)) {
        if (d1 <= d0) continue;
        int flate = 0, other = 0;
        for (size_t k = dict0; k + 12 < d0; ++k) {
            if (!flate && memcmp(buf + k, "/FlateDecode", 12) == 0) flate = 1;
            if (!other && (memcmp(buf + k, "ASCII85Decode", 13) == 0 ||
                           memcmp(buf + k, "LZWDecode", 9) == 0 ||
                           memcmp(buf + k, "RunLengthDecode", 15) == 0 ||
                           memcmp(buf + k, "DCTDecode", 9) == 0)) other = 1;
        }
        const char *content;
        size_t clen;
        if (flate) {
            size_t dn = 0;
            char e2[96];
            if (inf_unzlib_ex(buf + d0, d1 - d0, zbuf, PDF_STREAM_MAX, &dn, NULL, e2, sizeof e2) != 0) {
                nskip++;
                continue;
            }
            content = (const char *)zbuf;
            clen = dn;
            nflate++;
        } else if (other) {
            nskip++;
            continue;
        } else {
            content = (const char *)(buf + d0);
            clen = d1 - d0;
        }
        if (clen < 4) continue;
        if (pdf_memmem((const unsigned char *)content, clen, "BT", 2) == NULL) continue;  /* 不是内容流 */
        if (on + 2 >= ocap) break;
        ndec++;
        size_t u0 = nunmapped;
        size_t got = pdf_parse_content(content, clen, out, &on, ocap, fs, &nunmapped);
        if (nunmapped != u0) nf_miss++;
        if (got) pdf_nl(out, &on, ocap);
    }
    out[on] = 0;
    if (decoded) *decoded = ndec;
    if (skipped) *skipped = nskip;
    if (note && ncap) {
        snprintf(note, ncap, "PDF：%zu 个内容流（FlateDecode %zu），跳过 %zu 个流；字体映射 %zu 个",
                 ndec, nflate, nskip, fs->nft);
        if (fs->nft == 0 && ndec) {
            size_t o = strlen(note);
            if (o + 64 < ncap) snprintf(note + o, ncap - o, "（一个都没认出来 ⇒ 全靠 Latin-1 直译）");
        }
        if (nunmapped) {
            size_t o = strlen(note);
            if (o + 64 < ncap) snprintf(note + o, ncap - o, "；%zu 段文字没有字体映射（%zu 个流）",
                                        nunmapped, nf_miss);
        }
        if (trunctab) {
            size_t o = strlen(note);
            if (o + 48 < ncap) snprintf(note + o, ncap - o, "；对象表到 %d 截断", PDF_MAX_OBJ);
        }
    }
    pdf_fonts_free(fs);
    free(fs);
    free(tab);
    free(zbuf);
    free(tbuf);
    return on;
}

/* 从文件读 PDF（≤ PDF_MAX_FILE）→ 抽文字。返回文字长度（0 = 没抽到，note 里有原因）。 */
static inline size_t pdf_extract_file(const char *path, char *out, size_t ocap,
                                      size_t *decoded, size_t *skipped, char *note, size_t ncap) {
    if (decoded) *decoded = 0;
    if (skipped) *skipped = 0;
    if (!path || !out) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) { if (note && ncap) snprintf(note, ncap, "读不到 %.96s", path); return 0; }
    unsigned char *buf = (unsigned char *)malloc(PDF_MAX_FILE);
    if (!buf) {
        fclose(f);
        if (note && ncap) snprintf(note, ncap, "内存不够（要 %u KB）", PDF_MAX_FILE / 1024);
        return 0;
    }
    size_t g = fread(buf, 1, PDF_MAX_FILE, f);
    int big = !feof(f);
    fclose(f);
    if (big) {
        free(buf);
        if (note && ncap) snprintf(note, ncap, "%.96s 超过 %u KB，先切一份再用", path, PDF_MAX_FILE / 1024);
        return 0;
    }
    size_t n = pdf_extract_text(buf, g, out, ocap, decoded, skipped, note, ncap);
    free(buf);
    return n;
}

#endif /* PDF_H */
