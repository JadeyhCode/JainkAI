/* tools/pdfprobe.c — **PDF 取文字**探针（§6.678）
 *
 * 判据：
 *   ① 三个夹具（不压缩 / FlateDecode / ToUnicode 中文）抽出来的文字**逐字符对**；
 *   ② 两种坏文件必须**明说读不出**（篡改的流 / 加密的 PDF），不许吐半截假字；
 *   ③ 别人写的 PDF 才能证明"不是只会读我自己的写法"：探针自己**现造**一份最小 PDF（内存里拼字符串）
 *      —— 它和夹具的字节布局不同（对象顺序、字典顺序都变了），能过才算通用；
 *   ④ 不认识的滤镜（ASCII85）要**计入 skipped**，不许静默当没这回事；
 *   ⑤ 100 组"带 %PDF- 头的随机垃圾"跑过去不崩（asan 版再跑一遍就是内存证据）。
 *
 * 用法：./build_pdfprobe [夹具目录]（默认 tests/fixtures/pdf）
 */
#include "../pdf.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static const char *P = "tests/fixtures/pdf";

static size_t grab(const char *name, char *out, size_t cap, size_t *dec, size_t *skip, char *note) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", P, name);
    return pdf_extract_file(path, out, cap, dec, skip, note, 256);
}

/* 现造一份**最小的真 PDF**（不压缩内容流）：用来证明"别人的 PDF 也能读" */
static size_t build_mem_pdf(unsigned char *buf, size_t cap) {
    const char *cs = "BT /F1 12 Tf 20 700 Td (Alpha) Tj 40 0 Td (belongs) Tj 60 0 Td (to) Tj "
                     "30 0 Td (Beta.) Tj 0 -14 Td (Gamma) Tj 40 0 Td (likes) Tj 40 0 Td (Delta.) Tj ET";
    int n = snprintf((char *)buf, cap,
        "%%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n%%%%EOF\n", strlen(cs), cs);
    return (n > 0) ? (size_t)n : 0;
}

int main(void) {
    printf("== PDF 取文字 ==\n");
    char out[PDF_MAX_TEXT];
    char note[256];
    size_t dec = 0, skip = 0, n = 0;

    /* ---- ① 不压缩 ---- */
    memset(out, 0, sizeof out); note[0] = 0;
    n = grab("simple.pdf", out, sizeof out, &dec, &skip, note);
    ok(n > 0 && dec == 1, "simple.pdf：1 个内容流解出来");
    ok(strstr(out, "Penguins belong to birds.") != NULL, "整行文字对（句号、空格都在）");
    ok(strstr(out, "Kirin is a beast.") != NULL, "`Tj` + `Td` 的 dx>0 ⇒ 出**空格**（不是换行）");
    printf("   [数] simple.pdf 抽出 %zu 字节\n", n);
    printf("   [文] %s\n", out);

    /* ---- ② FlateDecode：同一段内容，压缩版必须抽出**完全相同**的文字 ---- */
    char out2[PDF_MAX_TEXT];
    memset(out2, 0, sizeof out2);
    size_t n2 = grab("flate.pdf", out2, sizeof out2, &dec, &skip, note);
    ok(n2 == n && strcmp(out2, out) == 0, "flate.pdf（FlateDecode）抽出来与不压缩版**逐字节相同**");
    ok(dec == 1 && skip == 0, "解了 1 个流、跳过 0 个");

    /* ---- ③ ToUnicode：中文 ---- */
    memset(out2, 0, sizeof out2);
    n2 = grab("unicode.pdf", out2, sizeof out2, &dec, &skip, note);
    ok(strstr(out2, "企鹅") != NULL && strstr(out2, "属于") != NULL, "unicode.pdf：ToUnicode 把码位映回**中文**");
    ok(strstr(out2, "鸟类。") != NULL, "整句连得上（含全角句号）");
    printf("   [文] %s", out2);
    if (out2[0] && out2[strlen(out2) - 1] != '\n') printf("\n");

    /* ---- ④ 坏文件：不许假装读到 ---- */
    memset(out2, 0, sizeof out2); note[0] = 0;
    n2 = grab("broken.pdf", out2, sizeof out2, &dec, &skip, note);
    ok(n2 == 0 && skip == 1, "broken.pdf：流解不开 ⇒ 0 文字，跳过计数 = 1");
    ok(strstr(note, "跳过") != NULL, "note 里**明说**跳过了几个流");
    printf("   [注] %s\n", note);
    memset(out2, 0, sizeof out2); note[0] = 0;
    n2 = grab("encrypted.pdf", out2, sizeof out2, &dec, &skip, note);
    ok(n2 == 0 && strstr(note, "加密") != NULL, "encrypted.pdf：明说**加密了我不读**（不猜内容）");

    /* ---- ⑤ 现造的内存 PDF ---- */
    {
        unsigned char *mem = (unsigned char *)malloc(65536);
        char o3[16384];
        size_t len = build_mem_pdf(mem, 65536);
        ok(len > 0, "内存里现造一份最小 PDF");
        size_t g = pdf_extract_text(mem, len, o3, sizeof o3, &dec, &skip, note, sizeof note);
        ok(g > 0 && strstr(o3, "Alpha belongs to Beta.") != NULL,
           "现造的 PDF（布局与夹具不同）也抽出整行");
        ok(strstr(o3, "Gamma likes Delta.") != NULL, "第二行（`0 -14 Td` ⇒ 换行）");
        printf("   [文] %s\n", o3);
        free(mem);
    }

    /* ---- ⑥ 不认识的滤镜 ---- */
    {
        const char *cs = "BT /F1 12 Tf (no) Tj ET";
        char buf[1024];
        int ln = snprintf(buf, sizeof buf,
            "%%PDF-1.4\n1 0 obj\n<< /Length %zu /Filter /ASCII85Decode >>\nstream\n%s\nendstream\nendobj\n"
            "trailer\n<< /Root 1 0 R >>\n%%%%EOF\n", strlen(cs), cs);
        size_t g = pdf_extract_text((const unsigned char *)buf, (size_t)ln, out2, sizeof out2,
                                    &dec, &skip, note, sizeof note);
        ok(g == 0 && skip == 1 && dec == 0, "ASCII85Decode 的流：跳过计数 = 1（不静默）");
    }

    /* ---- ⑦ 不是 PDF ---- */
    {
        const char *txt = "这只是一段纯文本，不是 PDF。\n";
        size_t g = pdf_extract_text((const unsigned char *)txt, strlen(txt), out2, sizeof out2,
                                    &dec, &skip, note, sizeof note);
        ok(g == 0 && strstr(note, "不是 PDF") != NULL, "纯文本：明说不是 PDF");
    }

    /* ---- ⑧ 随机垃圾 + PDF 头（100 组）不崩 ---- */
    {
        unsigned char junk[4096];
        uint32_t x = 0x9E3779B9u;
        int crashed = 0;
        for (int t = 0; t < 100; ++t) {
            size_t ln = 64 + (size_t)(t * 37 % 3500);
            for (size_t i = 0; i < ln; ++i) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                junk[i] = (unsigned char)(x >> 24);
            }
            memcpy(junk, "%PDF-1.4\n", 9);
            size_t g = pdf_extract_text(junk, ln, out2, sizeof out2, &dec, &skip, note, sizeof note);
            (void)g;
            crashed++;
        }
        ok(crashed == 100, "100 组随机垃圾（带 PDF 头）全跑完（没崩）");
    }

    printf("结果：%d 项通过，%d 项失败\n", passes, fails);
    return fails ? 1 : 0;
}
