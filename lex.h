/* lex.h — **ASCII 代码词法**（线 A ③：代码词法，ASCII 逐字保留）
 *
 * 为什么单独一套词法：`ai_tokenize` 是**中文/词表**那一路的（最长匹配、会拆复合词、
 * 认不得的整块丢给字符级），拿它切代码就是把 `int` `main` 当"池里认识的词"乱拼。
 * 代码要的是另一套规矩，就四条：
 *   · 标识符：`[A-Za-z_][A-Za-z0-9_]*`
 *   · 数字：  `[0-9]+` 或 `[0-9]+.[0-9]+`（小数点后面没数字就不吃那个点）
 *   · 标点：  **逐字符**一个符号节点（`(` `)` `[` `]` `{` `}` `:` `;` `,` `+` `-` `*` `/`
 *             `=` `<` `>` `!` `"` `#` `&` `|` `%` `^` `~` `?` `.` …）
 *   · 空白：  **归一**（空格/制表/换行…都只是一处分隔，不进符号节点）——
 *             **符号节点里不能有空格**，这是硬边界（存进池子的符号节点名字靠空白分开）
 *
 * **往返判据只写在这一处**（`lex_roundtrip()`，tests 只调它，别在测试里再抄一遍）：
 *   ① **逐字保留**：把符号节点首尾相接（不加分隔）== 源文本去掉空白后的字节串
 *      —— 一个字节不许丢、不许改、不许凭空多出来；
 *   ② **幂等**：canonical 形态（符号节点之间恰好一个空格）**再词法一遍**，
 *      canonical 必须逐字节相同（说明空白真的归一干净了、词法没有上下文依赖）。
 * 两条都过 = 往返成立。`::lex` 打的就是这两条 + 括号配对。
 *
 * 边界（照实说，别当 bug）：
 *   · **非 ASCII 字节按"一个字节一个符号节点"**（UTF-8 汉字会碎成 3 个词）——
 *     这一层管的是 ASCII 代码；`::lex` 会把非 ASCII 字节数出来提醒你。
 *   · **不认字符串与注释**：引号、`//`、块注释符号都与普通标点一样逐字符切。
 *     逐字保留不受影响（字节一个不少），但**括号配对**会被字符串/注释里的括号骗到。
 *   · 词数上限 `LEX_MAX_TOK`；超了记账（`n_trunc`），往返判据直接说"没测"。
 */
#ifndef LEX_H
#define LEX_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEX_MAX_TOK     1024
#define LEX_CANON_SHOW  24      /* `::lex` 一行最多列这么多词 */

enum { LEX_IDENT = 0, LEX_NUMBER, LEX_PUNCT, LEX_OTHER };
#define LEX_KIND_N 4

typedef struct {
    size_t off;      /* 在源文本里的偏移 */
    size_t len;      /* 字节数 */
    int    kind;     /* LEX_* */
} LexTok;

typedef struct {
    const char *src;
    size_t n;                                  /* 源文本字节数 */
    size_t n_tok;
    size_t n_kind[LEX_KIND_N];                 /* 各类词数 */
    size_t n_ws;                               /* 归一掉了几处空白串 */
    size_t n_trunc;                            /* 超上限丢掉的词数 */
    LexTok tok[LEX_MAX_TOK];
} LexProg;

static inline int lex_is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
static inline int lex_is_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static inline int lex_is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static inline const char *lex_kind_name(int k) {
    switch (k) {
    case LEX_IDENT:  return "标识符";
    case LEX_NUMBER: return "数字";
    case LEX_PUNCT:  return "标点";
    case LEX_OTHER:  return "非ASCII";
    default:         return "?";
    }
}

/* 词法扫描：**不改源文本**（符号节点只记 off/len） */
static inline void lex_scan(const char *src, LexProg *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!src) src = "";
    out->src = src;
    out->n = strlen(src);
    size_t i = 0;
    while (i < out->n) {
        unsigned char c = (unsigned char)src[i];
        if (lex_is_ws(c)) {                       /* 空白：整串归一掉 */
            while (i < out->n && lex_is_ws((unsigned char)src[i])) i++;
            out->n_ws++;
            continue;
        }
        size_t start = i;
        int kind;
        if (lex_is_alpha(c)) {
            while (i < out->n && (lex_is_alpha((unsigned char)src[i]) ||
                                  lex_is_digit((unsigned char)src[i]))) i++;
            kind = LEX_IDENT;
        } else if (lex_is_digit(c)) {
            while (i < out->n && lex_is_digit((unsigned char)src[i])) i++;
            if (i + 1 < out->n && src[i] == '.' && lex_is_digit((unsigned char)src[i + 1])) {
                i++;
                while (i < out->n && lex_is_digit((unsigned char)src[i])) i++;
            }
            kind = LEX_NUMBER;
        } else {
            i++;
            kind = (c < 0x80) ? LEX_PUNCT : LEX_OTHER;   /* 标点逐字符；非 ASCII 一字节一个词 */
        }
        if (out->n_tok < LEX_MAX_TOK) {
            LexTok *t = &out->tok[out->n_tok++];
            t->off = start; t->len = i - start; t->kind = kind;
            out->n_kind[kind]++;
        } else {
            out->n_trunc++;
        }
    }
}

/* canonical 形态需要多少字节（符号节点之间恰好一个空格 + 结尾 0） */
static inline size_t lex_canon_need(const LexProg *p) {
    size_t need = 1;
    if (!p) return need;
    for (size_t i = 0; i < p->n_tok; ++i) need += p->tok[i].len + 1;
    return need;
}
/* 拼 canonical 形态；返回写进去的长度（不含结尾 0）。缓冲区不够就截断并返回需要的长度 */
static inline size_t lex_canon(const LexProg *p, char *buf, size_t cap) {
    size_t k = 0;
    if (!p || !buf || !cap) return 0;
    for (size_t i = 0; i < p->n_tok; ++i) {
        if (i) { if (k + 1 < cap) buf[k] = ' '; k++; }
        for (size_t j = 0; j < p->tok[i].len; ++j) {
            if (k + 1 < cap) buf[k] = p->src[p->tok[i].off + j];
            k++;
        }
    }
    buf[k < cap ? k : cap - 1] = 0;
    return k;
}

/* ---- 往返判据：**只写在这一处**，两条都过才算成立 ---- */
static inline int lex_roundtrip(const char *src, char *why, size_t whycap) {
    if (why && whycap) why[0] = 0;
    if (!src) src = "";
    LexProg a;
    lex_scan(src, &a);
    if (a.n_trunc) {
        if (why && whycap) snprintf(why, whycap, "词数超过上限 %d（多出 %zu 个词）—— 没测",
                                    LEX_MAX_TOK, a.n_trunc);
        return 0;
    }

    /* ① 逐字保留：源文本里每个**非空白**字节，必须按顺序正好等于符号节点拼起来的字节串 */
    size_t ti = 0, tj = 0;                     /* 第 ti 个词里的第 tj 个字节 */
    for (size_t i = 0; i < a.n; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (lex_is_ws(c)) continue;
        while (ti < a.n_tok && tj >= a.tok[ti].len) { ti++; tj = 0; }
        if (ti >= a.n_tok || src[a.tok[ti].off + tj] != (char)c) {
            if (why && whycap)
                snprintf(why, whycap,
                         "第 %zu 个字节（`%c`）没进符号节点或被改了 —— 逐字保留不成立",
                         i + 1, (c >= 0x20 && c < 0x7f) ? (char)c : '?');
            return 0;
        }
        tj++;
    }
    while (ti < a.n_tok && tj >= a.tok[ti].len) { ti++; tj = 0; }
    if (ti != a.n_tok || tj != 0) {
        if (why && whycap) snprintf(why, whycap, "符号节点里多出了源文本没有的字节");
        return 0;
    }

    /* ② 幂等：canonical 再词法一遍，canonical 必须逐字节相同 */
    size_t cap1 = lex_canon_need(&a);
    char *c1 = (char *)malloc(cap1);
    if (!c1) { if (why && whycap) snprintf(why, whycap, "内存不够"); return 0; }
    lex_canon(&a, c1, cap1);
    LexProg b;
    lex_scan(c1, &b);
    size_t cap2 = lex_canon_need(&b);
    char *c2 = (char *)malloc(cap2);
    if (!c2) { free(c1); if (why && whycap) snprintf(why, whycap, "内存不够"); return 0; }
    lex_canon(&b, c2, cap2);
    int same = (strcmp(c1, c2) == 0);
    if (!same && why && whycap)
        snprintf(why, whycap, "canonical 再词法一遍变了（空白没归一干净）");
    free(c1);
    free(c2);
    return same;
}

/* 括号配对（`()` `[]` `{}`）：代码文本层面的配对，与 token_lang.h 的
 * 「一个词里 `[`/`]` 成对」不是一回事 —— 那一层管的是原语词，这一层管的是源码。 */
static inline int lex_balance(const char *src, char *why, size_t whycap) {
    if (why && whycap) why[0] = 0;
    LexProg p;
    lex_scan(src, &p);
    char open[16];
    size_t n = 0;
    for (size_t i = 0; i < p.n_tok; ++i) {
        const LexTok *t = &p.tok[i];
        if (t->kind != LEX_PUNCT || t->len != 1) continue;
        char c = src[t->off];
        if (c == '(' || c == '[' || c == '{') {
            if (n < sizeof open) open[n] = c;
            n++;
            continue;
        }
        if (c != ')' && c != ']' && c != '}') continue;
        char want = (c == ')') ? '(' : (c == ']') ? '[' : '{';
        if (n == 0) {
            if (why && whycap) snprintf(why, whycap, "第 %zu 个字节的 `%c` 没有对应的开括号",
                                        t->off + 1, c);
            return 0;
        }
        if (n > sizeof open || open[n - 1] != want) {
            if (why && whycap) snprintf(why, whycap,
                                        "第 %zu 个字节的 `%c` 与它前面的开括号不配",
                                        t->off + 1, c);
            return 0;
        }
        n--;
    }
    if (n > 0) {
        if (why && whycap) snprintf(why, whycap, "还有 %zu 个开括号没关上（最外层是 `%c`）",
                                    n, open[0]);
        return 0;
    }
    return 1;
}

/* `::lex` 的呈现（就这一处拼） */
static inline void lex_print(const char *src) {
    LexProg p;
    lex_scan(src, &p);
    printf("[ASCII 代码词法] 源 %zu 字节 → 词 %zu 个（标识符 %zu / 数字 %zu / 标点 %zu / 非ASCII %zu）"
           "，空白归并 %zu 处%s\n",
           p.n, p.n_tok, p.n_kind[LEX_IDENT], p.n_kind[LEX_NUMBER],
           p.n_kind[LEX_PUNCT], p.n_kind[LEX_OTHER], p.n_ws,
           p.n_trunc ? "（词数超上限，只留前 1024 个）" : "");
    char why[160];
    int rt = lex_roundtrip(src, why, sizeof why);
    printf("  往返：逐字保留 %s / 幂等 %s → %s\n",
           rt ? "✓" : "✗", rt ? "✓" : "✗", rt ? "成立" : "不成立");
    if (!rt) printf("    原因：%s\n", why);
    int bal = lex_balance(src, why, sizeof why);
    printf("  括号 `()` `[]` `{}`：%s\n", bal ? "配对 ✓" : "不配对 ✗");
    if (!bal) printf("    原因：%s\n", why);
    size_t cap = lex_canon_need(&p);
    char *canon = (char *)malloc(cap);
    if (canon) {
        lex_canon(&p, canon, cap);
        printf("  canonical：%s\n", canon);
        free(canon);
    }
    printf("  词表：");
    for (size_t i = 0; i < p.n_tok && i < LEX_CANON_SHOW; ++i) {
        printf("%s%s`%.*s`", i ? " " : "", lex_kind_name(p.tok[i].kind),
               (int)p.tok[i].len, src + p.tok[i].off);
    }
    if (p.n_tok > LEX_CANON_SHOW) printf(" …（还有 %zu 个）", p.n_tok - LEX_CANON_SHOW);
    printf("\n");
}

#endif /* LEX_H */
