/* inflate.h — **自带 DEFLATE 解码**（RFC 1951）+ zlib 外壳（RFC 1950）
 *
 * 为什么自己写（§6.677）：PDF 的 FlateDecode 需要 zlib。项目一直是**单文件 + `-lm`** 就能编，
 * 引 `-lz` 会让每条构建命令、每个探针、sanitizer 脚本全都多一个外部依赖 —— 那不是一个"内核"
 * 该有的样子。DEFLATE 解码器是**有界、可测、无依赖**的一小块（本文件 ~300 行），值得自己写。
 *
 * 支持范围（写死）：
 *   · 块类型 0（存块）/ 1（固定 Huffman）/ 2（动态 Huffman）；类型 3 报错；
 *   · 规范 Huffman 表（`构造` 与 `解码` 都按 RFC 1951 §3.2.2）；
 *   · zlib 外壳：CMF/FLG 校验（%31）、FDICT 拒绝（PDF 不用预置字典）、尾部 Adler-32 **核对**；
 *   · 输出上限 INF_MAXOUT（默认 8 MB），超过即报错 —— 解压炸弹不该把进程吃穿。
 *
 * 明确不做：
 *   · 不做 gzip 外壳（PDF 里是 zlib 外壳；要 gzip 先剥 10 字节头再喂 `inf_inflate`）；
 *   · 不做压缩（只解压）；不做 `inflateBack`；不做流式"喂一段吐一段"（一次性给全，输出进一块缓冲）；
 *   · 不做 preset dictionary（碰到 FDICT 直接拒绝，不猜）。
 *
 * 正确性口径（照实说）：这里**没有**"和 zlib 逐字节比过所有流"这种话。
 * 证据是 tests/fixtures/pdf/ 下用**真 zlib 压出来的**文件 + 探针里 ①真的 zlib 产物
 * ②随机数据的存块往返 ③坏流必须报错 三组（见 tools/inflateprobe.c）。
 */
#ifndef INFLATE_H
#define INFLATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INF_MAXBITS    15
#define INF_MAXLCODES  288
#define INF_MAXDCODES  32
#define INF_MAXCODES   19
#define INF_MAXBCODES  (INF_MAXLCODES + INF_MAXDCODES)
#define INF_MAXOUT     (8u * 1024u * 1024u)

typedef struct {
    const unsigned char *in;
    size_t inlen, inpos;
    unsigned bitbuf;      /* 当前字节里还没用掉的位（低位先出） */
    int bitcnt;
    unsigned char *out;
    size_t outcap, outpos;
    char err[96];
} InfState;

/* 规范 Huffman 表：count[len] = 该长度的码字数；sym[] = 按（长度，符号）排好的符号 */
typedef struct {
    short count[INF_MAXBITS + 1];
    short sym[INF_MAXBCODES];
} InfHuff;

static inline int inf_fail(InfState *s, const char *msg) {
    if (s && !s->err[0]) snprintf(s->err, sizeof s->err, "%s", msg);
    return -1;
}

/* ---- 位读取：DEFLATE 的位是**低地址位先出**（LSB-first）---- */
static inline int inf_bit(InfState *s, unsigned *v) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) return inf_fail(s, "流在块没结束时断了（字节用光了）");
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    *v = s->bitbuf & 1u;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return 0;
}

static inline int inf_bits(InfState *s, int need, unsigned *v) {
    unsigned x = 0;
    for (int i = 0; i < need; ++i) {
        unsigned b;
        if (inf_bit(s, &b) != 0) return -1;
        x |= b << i;
    }
    *v = x;
    return 0;
}

/* ---- 规范 Huffman：构造（puff 口径）----
 * `lens[i]` = 符号 i 的码长（0 = 该符号不用）。超订（left<0）报错；
 * **不完整表接受** —— 动态块里"只有一个距离码"是合法且常见的（此时 left>0）。 */
static inline int inf_build(InfHuff *h, const short *lens, int n) {
    if (n <= 0 || n > INF_MAXBCODES) return -1;
    for (int l = 0; l <= INF_MAXBITS; ++l) h->count[l] = 0;
    for (int i = 0; i < n; ++i) {
        int l = lens[i];
        if (l < 0 || l > INF_MAXBITS) return -1;
        h->count[l]++;
    }
    int left = 1;
    for (int l = 1; l <= INF_MAXBITS; ++l) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;              /* 超订：码比位多，必然解错 */
    }
    short offs[INF_MAXBITS + 1];
    offs[1] = 0;
    for (int l = 1; l < INF_MAXBITS; ++l) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; ++i) {
        int l = lens[i];
        if (l) h->sym[offs[l]++] = (short)i;
    }
    return 0;
}

/* 解一个码：一位一位往下走（位是 MSB-first 拼进 code 的，见 RFC 1951 §3.1.1） */
static inline int inf_decode(InfState *s, const InfHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= INF_MAXBITS; ++len) {
        unsigned b;
        if (inf_bit(s, &b) != 0) return -1;
        code |= (int)b;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    inf_fail(s, "Huffman 码走满 15 位还没匹配（流坏了）");
    return -1;
}

static inline int inf_put(InfState *s, unsigned ch) {
    if (s->outpos >= s->outcap) return inf_fail(s, "解压结果超过输出上限");
    s->out[s->outpos++] = (unsigned char)ch;
    return 0;
}

static inline int inf_copy(InfState *s, unsigned dist, unsigned len) {
    if (dist == 0) return inf_fail(s, "回指距离 0（非法）");
    if (dist > s->outpos) return inf_fail(s, "回指跑到输出开头之前（流坏了）");
    if (s->outpos + len > s->outcap) return inf_fail(s, "解压结果超过输出上限");
    for (unsigned i = 0; i < len; ++i) {
        s->out[s->outpos] = s->out[s->outpos - dist];
        s->outpos++;
    }
    return 0;
}

/* RFC 1951 §3.2.5 的长度/距离基值 + 附加位 */
static const short inf_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short inf_lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short inf_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                     6145, 8193, 12289, 16385, 24577 };
static const short inf_dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* 固定 Huffman 表（一块块都要用，每次重建一遍；288+32 个 short 的代价可以忽略） */
static inline void inf_fixed(InfHuff *lit, InfHuff *dist) {
    short l[INF_MAXLCODES];
    int i = 0;
    for (; i < 144; ++i) l[i] = 8;
    for (; i < 256; ++i) l[i] = 9;
    for (; i < 280; ++i) l[i] = 7;
    for (; i < 288; ++i) l[i] = 8;
    (void)inf_build(lit, l, 288);
    short d[INF_MAXDCODES];
    for (i = 0; i < 30; ++i) d[i] = 5;
    for (; i < INF_MAXDCODES; ++i) d[i] = 0;
    (void)inf_build(dist, d, 30);
}

/* 一张表把字面量/长度码解到底（直到遇到块结束码 256） */
static inline int inf_codes(InfState *s, const InfHuff *lit, const InfHuff *dist) {
    for (;;) {
        int sym = inf_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (inf_put(s, (unsigned)sym) != 0) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return inf_fail(s, "长度码 286/287 在 RFC 里是保留码（流坏了）");
            unsigned extra = 0;
            if (inf_bits(s, inf_lext[sym], &extra) != 0) return -1;
            unsigned len = (unsigned)inf_lbase[sym] + extra;
            int dsym = inf_decode(s, dist);
            if (dsym < 0) return -1;
            if (dsym >= 30) return inf_fail(s, "距离码 30/31 在 RFC 里是保留码（流坏了）");
            unsigned dextra = 0;
            if (inf_bits(s, inf_dext[dsym], &dextra) != 0) return -1;
            unsigned d = (unsigned)inf_dbase[dsym] + dextra;
            if (inf_copy(s, d, len) != 0) return -1;
        }
    }
}

/* 动态 Huffman 表（RFC 1951 §3.2.7）：先读码长码的码长，再读长度表 */
static const short inf_order[INF_MAXCODES] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                               11, 4, 12, 3, 13, 2, 14, 1, 15 };

static inline int inf_dynamic(InfState *s) {
    unsigned hlit = 0, hdist = 0, hclen = 0;
    if (inf_bits(s, 5, &hlit) != 0 || inf_bits(s, 5, &hdist) != 0 ||
        inf_bits(s, 4, &hclen) != 0) return -1;
    hlit += 257;      /* 257..288 */
    hdist += 1;       /* 1..32 */
    hclen += 4;       /* 4..19 */
    if (hlit > INF_MAXLCODES) return inf_fail(s, "HLIT 超过 288");
    if (hdist > INF_MAXDCODES) return inf_fail(s, "HDIST 超过 32");
    if (hclen > INF_MAXCODES) return inf_fail(s, "HCLEN 超过 19");
    short cl[INF_MAXCODES];
    memset(cl, 0, sizeof cl);
    for (unsigned i = 0; i < hclen; ++i) {
        unsigned v = 0;
        if (inf_bits(s, 3, &v) != 0) return -1;
        cl[inf_order[i]] = (short)v;
    }
    InfHuff ch;
    if (inf_build(&ch, cl, INF_MAXCODES) != 0) return inf_fail(s, "码长码的表超订（流坏了）");
    short lens[INF_MAXBCODES];
    memset(lens, 0, sizeof lens);
    int n = 0, total = (int)hlit + (int)hdist;
    while (n < total) {
        int sym = inf_decode(s, &ch);
        if (sym < 0) return -1;
        if (sym < 16) {
            lens[n++] = (short)sym;
            continue;
        }
        int rep = 0;
        short val = 0;
        if (sym == 16) {
            if (n == 0) return inf_fail(s, "码长重复码 16 出现在第一个（没有前一个可重复）");
            unsigned r = 0;
            if (inf_bits(s, 2, &r) != 0) return -1;
            rep = 3 + (int)r;
            val = lens[n - 1];
        } else if (sym == 17) {
            unsigned r = 0;
            if (inf_bits(s, 3, &r) != 0) return -1;
            rep = 3 + (int)r;
        } else if (sym == 18) {
            unsigned r = 0;
            if (inf_bits(s, 7, &r) != 0) return -1;
            rep = 11 + (int)r;
        } else {
            return inf_fail(s, "码长码 19 非法（只有 0..18）");
        }
        if (n + rep > total) return inf_fail(s, "码长重复把表撑爆了（流坏了）");
        while (rep-- > 0) lens[n++] = val;
    }
    if (lens[256] == 0) return inf_fail(s, "长度表里没有块结束码 256（流坏了）");
    InfHuff lit, dist;
    if (inf_build(&lit, lens, (int)hlit) != 0) return inf_fail(s, "字面量/长度表超订（流坏了）");
    if (inf_build(&dist, lens + hlit, (int)hdist) != 0) return inf_fail(s, "距离表超订（流坏了）");
    return inf_codes(s, &lit, &dist);
}

/* 存块（类型 0）：先丢到字节边界，再 LEN/NLEN + 原样字节 */
static inline int inf_stored(InfState *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return inf_fail(s, "存块的头（LEN/NLEN）被截断");
    unsigned len = (unsigned)s->in[s->inpos] | ((unsigned)s->in[s->inpos + 1] << 8);
    unsigned nlen = (unsigned)s->in[s->inpos + 2] | ((unsigned)s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if ((len ^ 0xFFFFu) != nlen) return inf_fail(s, "存块 LEN/NLEN 对不上（流坏了）");
    if (s->inpos + len > s->inlen) return inf_fail(s, "存块的数据被截断");
    if (s->outpos + len > s->outcap) return inf_fail(s, "解压结果超过输出上限");
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->outpos += len;
    s->inpos += len;
    return 0;
}

/* 解一个块：返回 0 = 还有块、1 = 这是最后一块、-1 = 错 */
static inline int inf_block(InfState *s) {
    unsigned last = 0, type = 0;
    if (inf_bits(s, 1, &last) != 0 || inf_bits(s, 2, &type) != 0) return -1;
    int r;
    if (type == 0) {
        r = inf_stored(s);
    } else if (type == 1) {
        InfHuff lit, dist;
        inf_fixed(&lit, &dist);
        r = inf_codes(s, &lit, &dist);
    } else if (type == 2) {
        r = inf_dynamic(s);
    } else {
        return inf_fail(s, "块类型 3 在 RFC 里是非法值（流坏了）");
    }
    if (r != 0) return -1;
    return last ? 1 : 0;
}

/* **裸 DEFLATE** 解压。返回 0 = 成功（*dout = 输出字节数），-1 = 失败（s->err 里有原因）。 */
static inline int inf_inflate_ex(const unsigned char *src, size_t slen,
                                 unsigned char *dst, size_t dcap, size_t *dout,
                                 size_t *consumed, char *errbuf, size_t errcap) {
    InfState st;
    memset(&st, 0, sizeof st);
    st.in = src;
    st.inlen = slen;
    st.out = dst;
    st.outcap = dcap;
    if (dout) *dout = 0;
    if (consumed) *consumed = 0;
    int r;
    for (;;) {
        r = inf_block(&st);
        /* ⚠ 这里是 r == 0 才继续（还有块），r == 1 = 最后一块（收工）。
         * 第一版写的是 `r <= 0` —— 正好反过来：单块流会去解析"第二块"（那时指针已在
         * Adler 上），多块流则只解了第一块。探针立刻抓到（存块往返长度不对 + 报"LEN 截断"）。 */
        if (r != 0) break;
    }
    if (r < 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "%s", st.err[0] ? st.err : "解压失败");
        return -1;
    }
    if (dout) *dout = st.outpos;
    if (consumed) *consumed = st.inpos;
    return 0;
}

static inline uint32_t inf_adler32(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* **zlib 外壳**（RFC 1950）：校验头、跳过、解压、**核对尾部 Adler-32**。
 * ⚠ PDF 里 `endstream` 之前可能有多余的换行/填充 —— 它们不影响：我们只读到"最后一块结束"
 * 那个字节（`consumed`），Adler 紧跟其后，多出来的尾巴一概不看。 */
static inline int inf_unzlib_ex(const unsigned char *src, size_t slen,
                                unsigned char *dst, size_t dcap, size_t *dout,
                                size_t *consumed, char *errbuf, size_t errcap) {
    if (dout) *dout = 0;
    if (consumed) *consumed = 0;
    if (!src || slen < 6) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "zlib 流太短（连头带尾都不够）");
        return -1;
    }
    unsigned cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "CMF=%02X 不是 DEFLATE（低 4 位要 8）", cmf);
        return -1;
    }
    if (((unsigned)cmf * 256u + flg) % 31u != 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "zlib 头校验失败（CMF*256+FLG 不是 31 的倍数）");
        return -1;
    }
    if (flg & 0x20u) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "这条 zlib 流要预置字典（FDICT）—— 我不猜字典，拒绝");
        return -1;
    }
    size_t used = 0;
    if (inf_inflate_ex(src + 2, slen - 2, dst, dcap, dout, &used, errbuf, errcap) != 0) return -1;
    size_t at = 2 + used;
    if (at + 4 <= slen) {
        uint32_t want = ((uint32_t)src[at] << 24) | ((uint32_t)src[at + 1] << 16) |
                        ((uint32_t)src[at + 2] << 8) | (uint32_t)src[at + 3];
        uint32_t got = inf_adler32(dst, dout ? *dout : 0);
        if (want != got) {
            if (errbuf && errcap) snprintf(errbuf, errcap, "Adler-32 对不上（要 %08X 得 %08X）", want, got);
            return -1;
        }
        if (consumed) *consumed = at + 4;
    } else {
        /* 尾巴都不全 ⇒ 没有校验 ⇒ 不敢当成功（**契约**：返回 0 就意味着 Adler 核对过了） */
        if (errbuf && errcap) snprintf(errbuf, errcap, "zlib 尾部的 Adler-32 被截断（没有校验就不算成功）");
        return -1;
    }
    return 0;
}

/* 常用两个参数的短写（错误信息丢掉，只看成败） */
static inline int inf_inflate(const unsigned char *src, size_t slen,
                              unsigned char *dst, size_t dcap, size_t *dout) {
    return inf_inflate_ex(src, slen, dst, dcap, dout, NULL, NULL, 0);
}
static inline int inf_unzlib(const unsigned char *src, size_t slen,
                             unsigned char *dst, size_t dcap, size_t *dout) {
    return inf_unzlib_ex(src, slen, dst, dcap, dout, NULL, NULL, 0);
}

#endif /* INFLATE_H */
