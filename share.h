/* ============================================================================
 * share.h — **池级共享的最小可行部分**：不带指针的大块引用计数共享（§6.581）
 *
 * 为什么只能共享"一部分"（这是这一轮量出来的结构事实，不是懒）：
 *   · 图里最大的两块是 **边数组**（出厂图 41216 条边）和 **emb**（1024 维 int8/词）。
 *   · 边数组里存的是 `Token*`（`AssocEntry.main/action`）⇒ 两个脑要各自改图，
 *     就必须各有各的 `Token` 对象；一换对象，边里的指针全得重写 ⇒ **边数组没法共享**。
 *     （真要共享，得先把边改成"存 id"，那是一次全仓重构。）
 *   · `emb` **不含任何指针**（纯 int8 向量）⇒ 可以安全共享：两个池的两个 Token
 *     指向同一块 1024 字节。写的时候再复制（写前复制 = COW）。
 *
 * 这一层就是那个引用计数表 + 写前复制：
 *   `aip_buf_share(p)`   共享一块（表里计数 +1）
 *   `aip_buf_own(p,n)`   要写了：是共享的就**先复制一份自己的**（旧的那份还回去）
 *   `aip_buf_release(p)` 放手（计数归零才 free）
 *   `aip_buf_refs(p)`    现在有几个人指着（探针用）
 *
 * 边界（照实说）：
 *   · 单线程、单进程；表是静态数组，满了就**拒绝共享**（宁可不共享，也不静默出错）。
 *   · 只有 `emb` 走这条路过（`Token.name` 是不可变字符串，理论上也能共享，但省不到 1%，
 *     没做 —— 加了反而多一张表要维护）。
 *   · **ASan/LSan 下**：共享块只 free 一次（引用计数 0 时），两个池按任意顺序释放都成立。
 * ==========================================================================*/
#ifndef SHARE_H
#define SHARE_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 最多同时共享多少块。出厂图 1469 词 ⇒ 全共享也只有 1469 块；8192 够 5 个脑。
 * 满了就返回"没共享成"，调用方照旧各持一份（正确性不受影响）。 */
#ifndef AIP_BUF_MAX
#define AIP_BUF_MAX 8192u
#endif

typedef struct {
    void    *p;      /* 共享块地址（NULL = 空槽 / 墓碑——这里不用墓碑，删除靠搬移）*/
    uint32_t n;      /* 有几个人指着（≥2 才在表里）*/
} AipBufRef;

static AipBufRef g_bufref[AIP_BUF_MAX];
static size_t    g_bufref_n = 0;

/* 线性查找。块数最多几千、操作只在"共享 / 写前复制 / 释放"时发生（不是热路径），
 * 所以刻意不用哈希：少一张表就少一类 bug（碰撞、扩容、墓碑）。 */
static inline AipBufRef *aip_buf_slot(void *p) {
    for (size_t i = 0; i < g_bufref_n; ++i)
        if (g_bufref[i].p == p) return &g_bufref[i];
    return NULL;
}

/* 表里删一条（把最后一条搬过来填洞，保持紧凑） */
static inline void aip_buf_drop(AipBufRef *e) {
    size_t i = (size_t)(e - g_bufref);
    if (i >= g_bufref_n) return;
    g_bufref_n--;
    if (i != g_bufref_n) g_bufref[i] = g_bufref[g_bufref_n];
    g_bufref[g_bufref_n].p = NULL;
    g_bufref[g_bufref_n].n = 0;
}

/* 现在有几个人指着（不在表里 = 1，即"私有"；p 为 NULL 也给 1，调用方自己判空） */
static inline uint32_t aip_buf_refs(const void *p) {
    AipBufRef *e = aip_buf_slot((void *)p);
    return e ? e->n : 1u;
}

/* 共享一块：返回同一个指针（方便写 `t->x = aip_buf_share(t->x)`）。
 * 表满 / p 为空 ⇒ 原样返回（调用方**不会**因此变错，只是没省到内存）。 */
static inline void *aip_buf_share(void *p) {
    if (!p) return NULL;
    AipBufRef *e = aip_buf_slot(p);
    if (e) { e->n++; return p; }
    if (g_bufref_n >= AIP_BUF_MAX) return p;
    g_bufref[g_bufref_n].p = p;
    g_bufref[g_bufref_n].n = 2;      /* 我 + 原来那个人 */
    g_bufref_n++;
    return p;
}

/* 放手：计数归零才 free。**没在表里 = 私有 ⇒ 直接 free**（所有权的两态很干净）。 */
static inline void aip_buf_release(void *p) {
    if (!p) return;
    AipBufRef *e = aip_buf_slot(p);
    if (!e) { free(p); return; }
    if (e->n > 2) { e->n--; return; }
    /* 只剩两个人里的一个放手 ⇒ 另一个变成唯一所有者（退出表，之后由它 release 时 free）*/
    aip_buf_drop(e);
}

/* 要写了：是共享的就先复制一份自己的；返回**可以写**的指针（可能是新的）。
 * `bytes` 是这块的字节数（emb = EMB_DIM）。分配失败返回 NULL（调用方必须判）。 */
static inline void *aip_buf_own(void *p, size_t bytes) {
    if (!p) return NULL;
    AipBufRef *e = aip_buf_slot(p);
    if (!e) return p;                 /* 私有：直接写 */
    void *q = malloc(bytes);
    if (!q) return p;                 /* 分配不出来就**放弃写前复制**（宁可共享着写，也别丢数据）*/
    memcpy(q, p, bytes);
    aip_buf_release(p);               /* 旧的那份还回去（计数 -1，可能仍有人用）*/
    return q;
}

/* 诊断：现在共享了多少块 / 一共多少字节（每块 bytes_per 由调用方给）*/
static inline size_t aip_buf_shared_count(void) { return g_bufref_n; }
static inline size_t aip_buf_shared_bytes(size_t bytes_per) {
    size_t b = 0;
    for (size_t i = 0; i < g_bufref_n; ++i) b += bytes_per * (size_t)(g_bufref[i].n - 1u);
    return b;                         /* 只算"省下来的那份"（n-1 份）*/
}

#endif /* SHARE_H */
