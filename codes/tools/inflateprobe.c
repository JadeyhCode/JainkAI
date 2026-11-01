/* tools/inflateprobe.c — **自带 inflate** 的探针（§6.677）
 *
 * 判据（每条都要能在输出里看到数）：
 *   ① 真 zlib 压出来的向量：解出来长度对、**Adler-32 自证**（zlib 尾部的校验和就是原文的校验和，
 *      对不上 `inf_unzlib` 会报错 ⇒ 这一条等价于"逐字节等于原文"，而明文不用入库）；
 *   ② text2k 的明文入库了，逐字节比；
 *   ③ 存块往返：探针里现造 DEFLATE 流（只有块类型 0），随机数据 **100 KB** 往返；
 *   ④ 坏输入必须**报错而不是崩**：截断 / 篡改 / 随机字节 200 组 / 输出上限 / FDICT；
 *   ⑤ 解压炸弹：256 KB 全零压成 277 字节，给 4 KB 输出缓冲必须拒绝（不是把内存吃穿）。
 *
 * 用法：./build_inflateprobe [夹具目录]（默认 tests/fixtures/inflate）
 */
#include "../inflate.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 20, used = 0;
    unsigned char *b = (unsigned char *)malloc(cap);
    if (!b) { fclose(f); return NULL; }
    for (;;) {
        size_t got = fread(b + used, 1, cap - used, f);
        used += got;
        if (used < cap) break;
        unsigned char *nb = (unsigned char *)realloc(b, cap * 2);
        if (!nb) { free(b); fclose(f); return NULL; }
        b = nb;
        cap *= 2;
    }
    fclose(f);
    *n = used;
    return b;
}

/* 现造一个"只有存块"的 DEFLATE 流（块头 3 位 + 对齐 + LEN/NLEN + 原样字节），
 * 再包一层 zlib 外壳。用来测**框架**（对齐、LEN/NLEN、多块、Adler），不测 Huffman。 */
static unsigned char *wrap_stored(const unsigned char *p, size_t n, size_t *outn) {
    size_t nb = (n + 65534) / 65535;
    if (nb == 0) nb = 1;
    size_t cap = 2 + nb * 5 + n + 4 + 8;
    unsigned char *o = (unsigned char *)malloc(cap);
    if (!o) return NULL;
    size_t k = 0;
    o[k++] = 0x78;                     /* CMF */
    o[k++] = 0x01;                     /* FLG：%31 == 0 */
    size_t at = 0;
    for (size_t b = 0; b < nb; ++b) {
        size_t take = n - at;
        if (take > 65535) take = 65535;
        o[k++] = (b + 1 == nb) ? 0x01 : 0x00;   /* 最后一块的 BFINAL=1，BTYPE=00 */
        o[k++] = (unsigned char)(take & 0xFFu);
        o[k++] = (unsigned char)(take >> 8);
        o[k++] = (unsigned char)((~take) & 0xFFu);
        o[k++] = (unsigned char)(((~take) >> 8) & 0xFFu);
        if (take) memcpy(o + k, p + at, take);
        k += take;
        at += take;
    }
    uint32_t ad = inf_adler32(p, n);
    o[k++] = (unsigned char)(ad >> 24);
    o[k++] = (unsigned char)(ad >> 16);
    o[k++] = (unsigned char)(ad >> 8);
    o[k++] = (unsigned char)(ad);
    *outn = k;
    return o;
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "tests/fixtures/inflate";
    char path[512];
    printf("== inflate（自带 DEFLATE/zlib）==\n");

    /* ---- ① 真 zlib 向量 ---- */
    static const unsigned char *vec[8];
    size_t vn[8] = { 0 };
    const char *names[6] = { "small.z", "text2k.z", "long.z", "zeros.z", "badadler.z", "fdict.z" };
    for (int i = 0; i < 6; ++i) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        vec[i] = slurp(path, &vn[i]);
    }
    if (!vec[0]) {
        printf("  [FAIL] 夹具读不到：%s/%s（先跑 python3 tests/fixtures/mkfix.py）\n", dir, names[0]);
        fails++;
        printf("结果：%d 项通过，%d 项失败\n", passes, fails);
        return 1;
    }
    unsigned char out[300 * 1024];
    size_t dn = 0;
    {
        ok(inf_unzlib(vec[0], vn[0], out, sizeof out, &dn) == 0, "small.z 解开");
        ok(dn == 24 && memcmp(out, "hello hello hello hello\n", 24) == 0, "small.z 内容逐字节相同（24 字节）");
        size_t cn = 0;
        ok(inf_unzlib_ex(vec[0], vn[0], out, sizeof out, &dn, &cn, NULL, 0) == 0 && cn == vn[0],
           "small.z 消费掉整条流（头 + 块 + Adler 一字节不差）");
    }
    {
        ok(inf_unzlib(vec[1], vn[1], out, sizeof out, &dn) == 0, "text2k.z（动态 Huffman）解开");
        size_t tn = 0;
        unsigned char *txt = slurp("tests/fixtures/inflate/text2k.txt", &tn);
        ok(txt && dn == tn && memcmp(out, txt, tn) == 0, "text2k 明文逐字节相同");
        free(txt);
    }
    {
        ok(inf_unzlib(vec[2], vn[2], out, sizeof out, &dn) == 0 && dn == 9984,
           "long.z（近 10 KB 长回指）长度对（Adler-32 内部核对过）");
        ok(inf_unzlib(vec[3], vn[3], out, sizeof out, &dn) == 0 && dn == 256 * 1024,
           "zeros.z 全零 256 KB 解出来（Adler-32 核对过）");
        char e[96] = {0};
        size_t small_out = 0;
        ok(inf_unzlib_ex(vec[3], vn[3], out, 4096, &small_out, NULL, e, sizeof e) != 0,
           "zeros.z 给 4 KB 输出缓冲 ⇒ 拒绝（报错，不吃内存）");
        ok(strstr(e, "上限") != NULL, "拒绝理由写明是**输出上限**");
    }
    {
        char e[96] = {0};
        ok(inf_unzlib_ex(vec[4], vn[4], out, sizeof out, &dn, NULL, e, sizeof e) != 0,
           "badadler.z（尾部校验被篡改）⇒ 拒绝");
        ok(strstr(e, "Adler") != NULL, "拒绝理由写明是 Adler-32 对不上");
        char e2[96] = {0};
        ok(inf_unzlib_ex(vec[5], vn[5], out, sizeof out, &dn, NULL, e2, sizeof e2) != 0,
           "fdict.z（要预置字典）⇒ 拒绝");
        ok(strstr(e2, "字典") != NULL, "拒绝理由写明是 FDICT（不猜字典）");
    }

    /* ---- ② 存块往返（100 KB 随机数据，用 LCG 保证可重复）---- */
    {
        size_t n = 100 * 1024;
        unsigned char *raw = (unsigned char *)malloc(n);
        unsigned char *dst = (unsigned char *)malloc(n + 64);
        uint32_t x = 2463534242u;
        for (size_t i = 0; i < n; ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            raw[i] = (unsigned char)(x >> 24);
        }
        size_t zn = 0;
        unsigned char *z = wrap_stored(raw, n, &zn);
        ok(z != NULL && inf_unzlib(z, zn, dst, n + 64, &dn) == 0 && dn == n && memcmp(raw, dst, n) == 0,
           "存块往返：100 KB 随机数据（2 个块）逐字节相同");
        free(z);
        free(raw);
        free(dst);
    }

    /* ---- ③ 坏输入：必须报错、不许崩 ---- */
    {
        size_t cn = 0;
        char e[96] = {0};
        ok(inf_unzlib_ex(vec[0], vn[0] - 3, out, sizeof out, &cn, NULL, e, sizeof e) != 0,
           "截断 3 字节的流 ⇒ 拒绝（不吐半截数据当成功）");
        ok(inf_unzlib_ex(vec[0], 5, out, sizeof out, &cn, NULL, e, sizeof e) != 0, "只有 5 字节 ⇒ 拒绝");
        ok(inf_unzlib(vec[0], 0, out, sizeof out, &cn) != 0, "空输入 ⇒ 拒绝");
    }
    {
        int bad = 0, survived = 0;
        unsigned char junk[256];
        uint32_t x = 123456789u;
        for (int t = 0; t < 200; ++t) {
            size_t n = 8 + (size_t)(t % 200);
            for (size_t i = 0; i < n; ++i) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                junk[i] = (unsigned char)(x >> 24);
            }
            junk[0] = 0x78;                       /* 头先摆成合法，才有机会走到块解析 */
            junk[1] = 0x01;
            size_t cn = 0;
            if (inf_unzlib(junk, n, out, sizeof out, &cn) != 0) bad++;
            survived++;
        }
        ok(survived == 200, "200 组随机字节全部跑完（没崩）");
        ok(bad >= 195, "其中至少 195 组被**拒绝**（乱码几乎不可能解出合法流）");
        printf("   [数] 随机 200 组：拒绝 %d 组，侥幸解出 %d 组\n", bad, 200 - bad);
    }

    printf("结果：%d 项通过，%d 项失败\n", passes, fails);
    return fails ? 1 : 0;
}
