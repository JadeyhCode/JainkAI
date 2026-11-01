# 联网检索 与 PDF 取文字（阶段二收口后的两个"外部世界"入口）

> 这两件事在 §6.676 结项时是**明写没做**的两个缺口。这一轮（§6.677–§6.681）把它们补上，
> 口径不变：**默认关、白名单、硬上限、每一次都记账、拿回来的东西一律先"待证"**。
> 本文只说"怎么做、边界在哪、怎么验"，不讲故事。

---

## 一、结论先说（三行）

| 能力 | 形态 | 默认 | 证据 |
|---|---|---|---|
| **读 PDF** | `::paper read x.pdf` / `::paper text x.pdf`（自带 inflate + 内容流解析，**不调外部程序**） | 开（本地文件） | 探针 16/16；真论文 2.2 MB，与 `pdftotext` 的**词种召回 96.0%**，耗时 0.20 s |
| **联网检索** | `::net search <词>` / `::net get <1-5\|网址>`（只用 arXiv 的公开 API，`curl` 子进程，不走 shell） | **关**（`::net on`） | 离线 12/12（该拒的逐条拒）+ 真联网 16/16（检索到 5 条、抓 1.1 MB、抽出 22 672 字节文字） |
| **压缩解码** | `inflate.h`（DEFLATE + zlib 外壳，**不引 `-lz`**） | — | 探针 19/19（真 zlib 向量 + Adler-32 自证 + 200 组随机垃圾不崩） |

---

## 二、PDF 取文字

### 2.1 为什么不调 `pdftotext`

这一层是**内核的一部分**。走 `::act` 那条白名单路子等于把"读论文"变成"跑外部命令"：
输出算谁的观测？进不进图？谁的错误？—— 边界全糊。所以自己实现，只用标准库与 `-lm`：

```
pdf.h     xref-free 扫描 → 对象表 → 每个字体自己的 ToUnicode → 内容流的文本算子
inflate.h DEFLATE（RFC1951）+ zlib 外壳（RFC1950），带 Adler-32 核对
```

### 2.2 认什么、不认什么

| 项 | 认？ | 说明 |
|---|---|---|
| 不压缩的流 | ✅ | |
| `FlateDecode` | ✅ | 自己解，输出上限 4 MB/流 |
| `LZW` / `ASCII85` / `RunLength` / `DCTDecode` | ❌ | **计数上报**（`跳过 N 个流`），不静默跳过 |
| xref 表 | 不用 | 直接扫 `N G obj` 与 `stream…endstream`：对坏 xref / 增量更新 / 对象流免疫 |
| `/Encrypt` | ❌ | 明说"加密了我不读"，一个字节都不猜 |
| `ToUnicode`（含 `beginbfchar`/`beginbfrange`） | ✅ | **按字体**（`Tf` 切），不是合并成一张表 |
| `/Differences` 编码表 | ❌ | 没有 ToUnicode 的字体退回 Latin-1 直译，并统计"N 段文字没有字体映射" |
| 文本算子 | `BT/ET`、`Tj/TJ/'/"`、`Td/TD/T*/Tm`、`Tf` | 别的算子不影响取文字（`Do`、`gs`、路径绘制一律跳过） |
| 版面（分栏/表格/公式） | ❌ | 输出是**按出现顺序的一行行文字**，不是排版还原 |
| 扫描件（纯图片） | ❌ | 抽到 0 字节并明说；不做 OCR |
| 断词连字（`transduc-` + `tion`） | ❌ | 所以真论文的召回率是 96% 而不是 100%（差的多半在这） |

**关键取舍：`Td` 的两个操作数要分开看。** `dy≠0` 才是换行，`dy=0 且 dx>0` 只是词间距。
第一版"见到 `Td` 就换行"，结果 TeX 生成的论文被切成"一行一个词"，下游 `paper_extract`
一句断言都抽不到 —— 这不是"少抽两条"，是整条链哑火。

### 2.3 验证（三层，全都可以自己跑）

```sh
python3 tests/fixtures/mkfix.py     # 夹具怎么来的（字节已入库，平时不用跑）
sh tests/pdf.sh                     # 12 项：探针 + CLI 长链 + 坏文件不撒谎 + pdftotext 逐词对照
sh tests/pdf_real.sh 论文.pdf       # 手动：真论文与 pdftotext 的逐词召回率
```

真论文实测（arXiv **1706.03762v7**，2 215 244 字节，"Attention Is All You Need"）：

```
内容流 20 个（FlateDecode 141 个流），跳过 6 个流（都是图片/DCTDecode），字体映射 23 个
抽出文字 38 816 字节；词种 1 383（pdftotext 1 417）⇒ 召回 96.0%
耗时 0.20 s（含两遍扫描：ToUnicode 先找、正文后抽）
```

差的那 4% 长这样（照实说是这些，不是"神秘损失"）：
`attentionbased`、`dmodel`、`headi`、`eos`、`epos`（上下标/数学体拼在一起）、
`transduc-`+`tion`（跨行断词）、`american`/`governments`（脚注里的小型大写）。

### 2.4 一路到"待证"

`::paper read x.pdf` 与 `::paper read x.txt` **是同一条下游**（`paper_ingest` → `paper_extract`
→ `paper_to_claims`）：PDF 只是多了一步"先取文字"。所以"读论文"这条链的既有纪律一条没松：

- 抽出来的**一律进待证**（来源 `论文`，可信度 50/100），**不写事实边**；
- 用户 `::claim accept N` 之后才算数（并记成"是你教的"）；
- 抽不到就说抽不到（`broken.pdf` 那种：`跳过 1 个流 —— 一个字都没读出来（我不猜，也不编）`）。

---

## 三、压缩解码（`inflate.h`）

- **范围**：块类型 0/1/2（存块 / 固定 Huffman / 动态 Huffman）、规范 Huffman 构造与解码、
  zlib 头校验（`%31`）、`FDICT` **拒绝**（不猜字典）、**尾部 Adler-32 必须核对**。
- **契约**：`inf_unzlib()` 返回 0 ⇒ **Adler-32 已经核对过**（所以工具可以拿"返回 0"当"内容正确"）。
  尾部被截断一律算失败 —— 没有校验就不算成功。
- **上限**：单流解压 ≤ 8 MB（`INF_MAXOUT`），解压炸弹到点报错，不把进程吃穿。
- **不做**：压缩、gzip 外壳、流式喂数据、预置字典。

验证（`tools/inflateprobe.c`，19 项）：

| 组 | 内容 |
|---|---|
| 真 zlib 向量 | `small.z`（固定 Huffman，逐字节比）/ `text2k.z`（动态 Huffman，明文入库逐字节比）/ `long.z`（近 10 KB 长回指）/ `zeros.z`（256 KB 全零，压缩比 1:946） |
| 自证 | 向量的明文不必入库：zlib 尾部的 Adler-32 就是原文的校验和，核对通过 ⇔ 内容正确 |
| 存块往返 | 探针现造 DEFLATE 流（2 个存块）+ 100 KB 随机数据逐字节比 |
| 该拒的 | 截断 3 字节 / 只有 5 字节 / 空输入 / 篡改 Adler / `FDICT` / 输出超上限（4 KB 缓冲拒绝 256 KB 全零） |
| 不崩 | 200 组随机字节：0 崩溃，200 组全被拒 |

踩过的坑（写下来免得再踩）：`inf_inflate_ex` 里循环条件第一版写成 `r <= 0` 就 break ——
**正好反了**（`r==0` 是"还有块"，`r==1` 才是"最后一块"）。症状是单块流去解析"第二块"
（指针已在 Adler 上，报"LEN/NLEN 被截断"），多块流只解第一块。探针第一条就抓到。

---

## 四、联网检索（`::net`）

### 4.1 门是怎么收窄的（七条，缺一条都不算数）

1. **默认关**：`::net on` 或 `AIP_NET=1`，否则一步都不出去（和 `::act` 同一条纪律）；
2. **不用 shell**：`fork` + `execv` + argv 数组，`;` `|` `$()` 只是普通字符；
3. **域名白名单**：`net/allow.txt`，缺省只有 `export.arxiv.org` / `arxiv.org`；
   **精确匹配**（`arxiv.org.evil.com`、`evil.arxiv.org` 都过不了）；
4. **只走 https**：`--proto =https --proto-redir =https`，`http://`、`file://` 一律拒；
   网址里不许带 `用户名@` 或 `:端口`；
5. **不跟重定向**：`--max-redirs 0`。重定向是"别人带你去别处"，那一步不在白名单语义里；
   另外还会再核一遍 `url_effective` 的主机（双保险）；
6. **硬上限**：每次 ≤ 2 MB、≤ 15 s（到点 `SIGKILL`）、每次会话 ≤ 8 次；
7. **账本**：`papers/cache/_net.log`，每次**抓取/被拒/失败**都一行（时间、主机、字节、退出码、理由）。

### 4.2 命令面（只有四件事）

```
::net status                开关 / 白名单 / 上限 / 本次抓了几次
::net on | off
::net search <关键词>        arXiv 公开 API（max_results=5，按相关度），列出编号 + 标题
::net get <1-5 | https://…>  抓正文 → papers/cache/ → 若是 PDF 直接取文字 → 抽断言 → **待证**
::net log                   看账（最后 10 行；全量就在那个文件里）
```

抓回来的东西**不会**变成知识：`papers/cache/` 只是缓存（`.gitignore` 里，正文多半有版权），
要进图必须走"待证 → 用户确认"这条既有的路。

### 4.3 实测（现场，2026-11-01）

```
::net search attention is all you need   → 5 条（API 返回 9 711 字节 Atom）
::net get 2                              → papers/cache/2512.19700v1.pdf 1 121 039 字节
                                           PDF：13 个内容流（FlateDecode 46），字体映射 18 个
                                           抽出 22 672 字节文字 ⇒ **0 条待证**（见下）
实际耗时：检索 ~1 s，抓 1.1 MB 正文 ~3 s（本机网络）
```

**为什么 0 条待证**：断言抽取用的是**中文关系词表**（`relations.txt`：属于/有/会/住/吃…），
英文论文"取得到文字、抽不出三段式断言"。这是真缺口，不是 bug —— 见第六节第 1 条。

---

## 五、复现（三分钟）

```sh
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -o build_aip main.c -lm

# ① PDF（离线）
./build_aip   # ::paper text tests/fixtures/pdf/unicode.pdf
              # ::paper read tests/fixtures/pdf/unicode.pdf
              # ::claim            → 两条待证
              # ::claim accept 1   → 企鹅 属于 鸟类
              # 企鹅 属于 鸟类 吗   → 对

# ② 联网（默认关）
./build_aip   # ::net                → 关 + 白名单 + 上限
              # ::net search xxx     → 拒（先 ::net on）
              # ::net on
              # ::net search attention is all you need
              # ::net get 1
              # ::net log

# ③ 套件
sh tests/pdf.sh                      # 12 项
sh tests/net.sh                      # 12 项（离线）
AIP_NET_LIVE=1 sh tests/net.sh       # 16 项（真联网，几十秒）
sh tests/pdf_real.sh 论文.pdf        # 真论文 vs pdftotext
```

---

## 六、局限（照实说，一条不藏）

1. **英文论文抽不出待证**：断言抽取只认中文关系词表。取文字没问题（96%），
   但"把论文变成知识"这一步对英文无效。下一步要么扩英文关系别名，要么扩中英混合词表。
2. **中文 PDF 只在有 `ToUnicode` 时才对**：CID 字库若没有 ToUnicode、只有内嵌 CMap，
   我读出来是乱码（不是崩溃，是错字）。`/Differences` 编码表也没做。
3. **多字体同名冲突**：资源名（`/F1`）在不同页可能指向不同字体对象，我取**第一个**登记，
   后面的不再覆盖。跨页共用名且字体不同的 PDF 会错字。
4. **版面不还原**：分栏会按"先左后右"的顺序串起来，表格会变成一串词，公式是数学字体的符号
   （`log`、`dmodel` 这种碎片就是这么来的）。
5. **跨行断词不合并**（`transduc-` / `tion`）。所以 96%，不是 100%。
6. **大文件上限**：PDF ≤ 6 MB、单个流解压 ≤ 4 MB、抽出文字 ≤ 2 MB；超了明说，不静默截断。
   （arXiv 的 2 MB 级论文没问题；带大量彩图的 10 MB 综述会被拒。）
7. **联网只有 arXiv**：没有通用搜索引擎，也没有 HTML 正文抽取 —— `::net get` 一个 HTML 页面
   只会把原始 HTML 存下来（抽不出断言）。加站必须先加白名单，且自己承担风险。
8. **不跟重定向**：有的站点/短链会 301/302，那些抓不到（这是刻意的）。
9. **账本是进程级的**：`::net` 的计数器在进程里，不随模型存盘；重开会话就清零（账本文件不清）。
10. **`curl` 是外部依赖**：本机没有 `/usr/bin/curl` 就用不了（明说，不偷偷换别的下载器）；
    PDF 那条路**没有**外部依赖。
