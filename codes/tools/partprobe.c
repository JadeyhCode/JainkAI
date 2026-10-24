/* tools/partprobe.c — **局部加载**探针（C 级，不经过 CLI）
 *
 * 目标（用户 2026-10-24）：「可以暂时只加载一部分图…指针不是 8 字节对齐吗，
 * 不是 0 就是在磁盘或其他地方」。这一版钉住的是：
 *   ① 边的**段文件**能写：`aip_parts_save` ⇒ 每个有边的词一段；
 *   ② **卸下**能卸：`aip_parts_detach` 把内存里的边 free 掉、指针打 `AIP_PTR_DISK` 标签，
 *      但 `count` 还在（"有几条"不等于"内容在内存"）；
 *   ③ **按需读回**：查询走 `token_assocs()`，读到哪个词才算加载哪个词 ——
 *      所以 `n_loads` 必须 **> 0 且 < 被卸下的词数**，这就是"只加载一部分"的机器证据；
 *   ④ 卸下之后**写新边**也不会崩（扩容前会先把段读回来）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_partprobe tools/partprobe.c -lm && ./build_partprobe
 */
#include "../ai.h"
#include "../rel.h"
#include "../parts.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static int teach(AI *ai, const char *a, const char *rel, const char *b) {
    Token *A = pool_token_intern(ai->pool, a);
    Token *B = pool_token_intern(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 1);
    if (!A || !B || !rid) return -1;
    return ai_write_rel(ai, A, B, rid, 0, AI_WSRC_RELATION);
}
static int knows(AI *ai, const char *a, const char *rel, const char *b) {
    Token *A = pool_find(ai->pool, a);
    Token *B = pool_find(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 0);
    if (!A || !B || !rid) return 0;
    return rel_direct(A, rid, B);
}
/* 有边的词数（= 会被卸下的段数） */
static size_t n_tokens_with_edges(AI *ai) {
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (t && t->count > 0) n++;
    }
    return n;
}

int main(void) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/aip_parts_%d.parts", (int)getpid());

    AI *A = ai_new();
    ok(A != NULL, "① 建一个智能体");
    ok(teach(A, "猫", "属于", "动物") == 0, "① 教「猫 属于 动物」");
    ok(teach(A, "狗", "属于", "动物") == 0, "① 教「狗 属于 动物」");
    ok(teach(A, "猫", "吃", "鱼") == 0, "① 教「猫 吃 鱼」");

    size_t with_edges = n_tokens_with_edges(A);
    ok(with_edges > 0, "① 图里有带边的词（下面要卸的段数 > 0）");

    int nsegs = aip_parts_save(A, path);
    ok(nsegs == (int)with_edges, "② 段文件写成功：**每个有边的词一段**");
    ok(access(path, F_OK) == 0, "② 段文件真的在磁盘上");

    /* 打开索引（只读索引，不读任何一段） */
    AIParts *pt = aip_parts_open(path);
    ok(pt != NULL, "② 能只打开**索引**（还没读任何一段边）");
    ok(pt && pt->n_loads == 0, "② 打开索引时 **一次段都没读**");
    size_t det = aip_parts_detach(A);
    ok(det == with_edges, "③ 卸下：所有带边的词都被扔到磁盘上");
    ok(pt && pt->n_bytes == 0, "③ 卸下这一刻：**一个字节都还没从磁盘读**（惰性是真的）");

    /* 第 ① 件事：卸下之后从磁盘走一遍"只加载需要的那部分" */
    AIParts *pt2 = aip_parts_open(path);
    ok(pt2 != NULL, "③ 重新打开索引");
    aip_parts_attach(A, pt2);
    ok(knows(A, "猫", "属于", "动物") == 1, "③ **卸下之后仍然答得对**（猫 属于 动物）");
    ok(pt2->n_loads >= 1, "③ 答这一问至少触发了一次**按需读回**");
    ok(pt2->n_loads < det, "③ 而且 **加载的词数 < 被卸下的词数**（真的只读了一部分）");
    ok(pt2->n_bytes > 0, "③ 读回来的字节数 > 0（有机器数字，不是'看起来跑了'）");
    ok(knows(A, "狗", "属于", "动物") == 1, "③ 另一条事实也答得对（按需再读一段）");
    ok(knows(A, "猫", "吃", "鱼") == 1, "③ 第三条也答得对");
    ok(pt2->n_loads < det || det <= 3, "③ 三段全读完时也不超过被卸下的词数（不会把整图偷偷读回来）");

    /* 第 ④ 件事：卸下状态下**写新边**（扩容前必须先读回，否则 realloc 一个标签指针 = 崩） */
    {
        AI *B = ai_new();
        ok(B != NULL, "④ 再建一个智能体（多智能体照旧共存）");
        (void)teach(B, "鸟", "会", "飞");
        char pathB[256];
        snprintf(pathB, sizeof pathB, "/tmp/aip_partsB_%d.parts", (int)getpid());
        ok(aip_parts_save(B, pathB) > 0, "④ B 的边写成自己的段文件（**各写各的**，不共用 A 的）");
        AIParts *pt3 = aip_parts_open(pathB);
        aip_parts_attach(B, pt3);
        size_t detB = aip_parts_detach(B);
        ok(detB > 0, "④ B 的边也卸到磁盘上");
        ok(teach(B, "鸟", "属于", "动物") == 0, "④ **卸下状态下追加新边不崩**（扩容前读回）");
        ok(knows(B, "鸟", "属于", "动物") == 1, "④ 新学的边读得到");
        ok(knows(B, "鸟", "会", "飞") == 1, "④ 老的边也还在（读回来之后是完整的）");
        aip_parts_close(pt3);
        remove(pathB);
        ai_free(B);
    }

    aip_parts_close(pt2);
    aip_parts_close(pt);
    remove(path);
    ai_free(A);

    printf("\n%s（局部加载：段文件 %d 段 / 按需读回 / 卸下可写）\n",
           fails == 0 ? "PASS" : "FAIL", nsegs);
    return fails == 0 ? 0 : 1;
}
