#!/usr/bin/env python3
# tests/fixtures/mkfix.py — 生成 PDF / inflate 测试夹具（**产物已入库**，平时不用跑）
#
# 为什么要入库产物：套件必须**离线、可重复**。夹具是"我自己造的小 PDF"，不涉及任何第三方版权；
# 生成脚本留在库里是为了"这些字节是怎么来的"可复核（`python3 tests/fixtures/mkfix.py` 会重写它们）。
#
# 生成：
#   tests/fixtures/pdf/simple.pdf     没压缩的内容流（只测文本算子）
#   tests/fixtures/pdf/flate.pdf      内容流 FlateDecode（走 inflate.h 全链路）
#   tests/fixtures/pdf/broken.pdf      流被篡改（必须**报错**，不许崩、不许假装读到）
#   tests/fixtures/pdf/encrypted.pdf   带 /Encrypt（必须拒读）
#   tests/fixtures/pdf/unicode.pdf    ToUnicode CMap → 中文（词与词用 Td 的 dx 隔开）
#   tests/fixtures/inflate/*.z         真 zlib 压出来的向量（探针用；靠 Adler-32 自证）
import os
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PDFD = os.path.join(HERE, "pdf")
INFD = os.path.join(HERE, "inflate")
os.makedirs(PDFD, exist_ok=True)
os.makedirs(INFD, exist_ok=True)


def build_pdf(objs, root=1, encrypt=None):
    """objs: [(num, body_bytes, stream_bytes_or_None, extra_dict)] -> 完整 PDF 字节"""
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    for num, body, stream, extra in objs:
        offs[num] = len(out)
        out += b"%d 0 obj\n" % num
        if stream is None:
            # ⚠ 对象体必须是**字典**（`<< ... >>`）。第一版把字典内容裸着写进去，
            # poppler 直接报 "Catalog object is wrong type (name)" —— 夹具不是真 PDF，
            # 拿它当裁判就等于自己跟自己比。
            out += b"<< " + body + b" >>\nendobj\n"
        else:
            d = body if body is not None else b""
            if extra:
                d += b" " + extra
            d += b" /Length %d" % len(stream)
            out += b"<< " + d + b" >>\nstream\n" + stream + b"\nendstream\nendobj\n"
    xref = len(out)
    n = max(offs) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % n
    for i in range(1, n):
        if i in offs:
            out += b"%010d 00000 n \n" % offs[i]
        else:
            out += b"0000000000 65535 f \n"
    tr = b"<< /Size %d /Root %d 0 R" % (n, root)
    if encrypt:
        tr += b" /Encrypt " + encrypt
    tr += b" >>\n"
    out += b"trailer\n" + tr + b"startxref\n%d\n%%%%EOF\n" % xref
    return bytes(out)


def font_objs():
    return [
        (5, b"/Type /Font /Subtype /Type1 /BaseFont /Helvetica", None, None),
    ]


def content_simple():
    # 三行英文，第二行靠 Td 的 dx 分词
    s = bytearray()
    s += b"BT /F1 12 Tf 72 720 Td (Penguins belong to birds.) Tj\n"
    s += b"0 -18 Td (Penguins can swim.) Tj\n"
    s += b"0 -18 Td (Kirin) Tj 60 0 Td (is a beast.) Tj\n"
    s += b"ET\n"
    return bytes(s)


def main():
    # ---------- simple.pdf：不压缩 ----------
    simple = build_pdf([
        (1, b"/Type /Catalog /Pages 2 0 R", None, None),
        (2, b"/Type /Pages /Kids [3 0 R] /Count 1", None, None),
        (3, b"/Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >>", None, None),
        (4, None, content_simple(), None),
        (5, b"/Type /Font /Subtype /Type1 /BaseFont /Helvetica", None, None),
    ])
    open(os.path.join(PDFD, "simple.pdf"), "wb").write(simple)

    # ---------- flate.pdf：内容流 FlateDecode ----------
    flate = build_pdf([
        (1, b"/Type /Catalog /Pages 2 0 R", None, None),
        (2, b"/Type /Pages /Kids [3 0 R] /Count 1", None, None),
        (3, b"/Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >>", None, None),
        (4, None, zlib.compress(content_simple(), 9), b"/Filter /FlateDecode"),
        (5, b"/Type /Font /Subtype /Type1 /BaseFont /Helvetica", None, None),
    ])
    open(os.path.join(PDFD, "flate.pdf"), "wb").write(flate)

    # ---------- broken.pdf：压好的流中间改一个字节 ----------
    bad = bytearray(zlib.compress(content_simple(), 9))
    bad[len(bad) // 2] ^= 0xFF
    broken = build_pdf([
        (1, b"/Type /Catalog /Pages 2 0 R", None, None),
        (2, b"/Type /Pages /Kids [3 0 R] /Count 1", None, None),
        (3, b"/Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R", None, None),
        (4, None, bytes(bad), b"/Filter /FlateDecode"),
        (5, b"/Type /Font /Subtype /Type1 /BaseFont /Helvetica", None, None),
    ])
    open(os.path.join(PDFD, "broken.pdf"), "wb").write(broken)

    # ---------- encrypted.pdf：带 /Encrypt ----------
    enc = build_pdf([
        (1, b"/Type /Catalog /Pages 2 0 R", None, None),
        (2, b"/Type /Pages /Kids [3 0 R] /Count 1", None, None),
        (3, b"/Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R", None, None),
        (4, None, content_simple(), None),
        (5, b"/Type /Font /Subtype /Type1 /BaseFont /Helvetica", None, None),
    ], encrypt=b"6 0 R")
    open(os.path.join(PDFD, "encrypted.pdf"), "wb").write(enc)

    # ---------- unicode.pdf：ToUnicode + 中文 ----------
    # 词与词之间用 Td 的 dx 隔开（真 PDF 就是这样）；末尾的句号单独一段
    words = ["企鹅", "属于", "鸟类。", "企鹅", "会", "游泳。"]
    codes = {}
    nxt = 1
    for w in words:
        for ch in w:
            if ch not in codes:
                codes[ch] = nxt
                nxt += 1
    # 内容流：第一行 "企鹅 属于 鸟类。"，第二行 "企鹅 会 游泳。"
    def enc_words(ws):
        parts = []
        for i, w in enumerate(ws):
            h = "".join("%04X" % codes[ch] for ch in w)
            if i:
                parts.append("14 0 Td")
            parts.append("<%s> Tj" % h)
        return " ".join(parts)
    cs = ("BT /F1 12 Tf 72 720 Td " + enc_words(words[:3]) + "\n"
          "0 -18 Td " + enc_words(words[3:]) + "\nET\n").encode("ascii")
    chars = sorted(codes.items(), key=lambda kv: kv[1])
    cmap = ("/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n"
            "%d beginbfchar\n" % len(chars)
            + "".join("<%04X> <%04X>\n" % (c, ord(ch)) for ch, c in chars)
            + "endbfchar\nendcmap\nend\nend\n").encode("ascii")
    uni = build_pdf([
        (1, b"/Type /Catalog /Pages 2 0 R", None, None),
        (2, b"/Type /Pages /Kids [3 0 R] /Count 1", None, None),
        (3, b"/Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            b"/Contents 7 0 R /Resources << /Font << /F1 5 0 R >> >>", None, None),
        (5, b"/Type /Font /Subtype /Type0 /BaseFont /TestCJK /Encoding /Identity-H "
            b"/DescendantFonts [8 0 R] /ToUnicode 6 0 R", None, None),
        (6, None, zlib.compress(cmap, 9), b"/Filter /FlateDecode"),
        (7, None, zlib.compress(cs, 9), b"/Filter /FlateDecode"),
        (8, b"/Type /Font /Subtype /CIDFontType0 /BaseFont /TestCJK "
            b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>", None, None),
    ])
    open(os.path.join(PDFD, "unicode.pdf"), "wb").write(uni)

    # ---------- inflate 向量 ----------
    def vec(name, data, level=9, zdict=None):
        if zdict is None:
            z = zlib.compress(data, level)
        else:
            co = zlib.compressobj(level, zlib.DEFLATED, 15, 8, 0, zdict)
            z = co.compress(data) + co.flush()
        open(os.path.join(INFD, name), "wb").write(z)
        return len(data), len(z)

    # ① 短文本（zlib 会选固定 Huffman）
    vec("small.z", b"hello hello hello hello\n")
    # ② 2KB 多样文本（动态 Huffman + 各种长度码）
    txt = (b"Penguins belong to birds. Penguins can swim. Penguins live in Antarctica.\n" * 8 +
           bytes(range(32, 127)) * 4)
    vec("text2k.z", txt)
    open(os.path.join(INFD, "text2k.txt"), "wb").write(txt)
    # ③ 100KB 长重复（长回指距离，练 dist 码；明文不入库，靠 Adler-32 自证）
    big = (b"ABCDEFGH" * 64 + b"0123456789" * 32) * 12
    vec("long.z", big, level=9)
    # ④ 全零（高压缩比；用来测"输出上限"拒绝）
    vec("zeros.z", b"\x00" * (256 * 1024))
    # ⑤ 坏的 Adler-32（解压能成功但校验必须失败）
    good = bytearray(zlib.compress(b"adler check matters\n", 9))
    good[-1] ^= 0xFF
    open(os.path.join(INFD, "badadler.z"), "wb").write(bytes(good))
    # ⑥ 要预置字典（FDICT）——必须拒绝，不许瞎猜
    vec("fdict.z", b"dictionary based compression test\n" * 4, zdict=b"dictionary")

    for f in sorted(os.listdir(PDFD)):
        p = os.path.join(PDFD, f)
        print("%-16s %6d" % (f, os.path.getsize(p)))
    for f in sorted(os.listdir(INFD)):
        p = os.path.join(INFD, f)
        print("%-16s %6d" % (f, os.path.getsize(p)))


if __name__ == "__main__":
    main()
