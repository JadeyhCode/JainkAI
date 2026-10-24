/* tools/multiagent.c — 多智能体 / 局部加载的**地基探针**（C 级，不经过 CLI）
 *
 * 用户 2026-10-24 的目标：
 *   「打开多个智能体，每个智能体只维护自己的修改，并且可以暂时只加载一部分图，
 *     至于怎么实现，就是指针不是 8 字节对齐吗，不是 0 就是在磁盘或其他地方」
 *
 * 这一轮（§6.564）钉住三件事，每件都配反例：
 *   ① **多个智能体在一个进程里共存**：两个 `ai_new()`，各教各的知识，互不串台；
 *   ② **每个智能体只认自己的修改**：修改账（`AIModLog`）在 `AI` 里，不是全局的 ——
 *      各记各的、条数不同、内容不同；清掉一个不影响另一个；
 *   ③ **指针标签的地基**（`ptrtag.h`）：tag 0/1/2 往返、strip 回原指针、
 *      **tag==0 与"没打标签"逐字节等价**（这是"接进来零行为变化"的机器检查）。
 *
 * 用法：gcc -std=c11 -O2 -Wall -Wextra -o build_multiagent tools/multiagent.c -lm && ./build_multiagent
 */
#include "../ai.h"
#include "../rel.h"
#include "../ptrtag.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

/* 在一个智能体里教一条关系（走统一写入通道：这样修改账也会记上） */
static int teach(AI *ai, const char *a, const char *rel, const char *b) {
    Token *A = pool_token_intern(ai->pool, a);
    Token *B = pool_token_intern(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 1);
    if (!A || !B || !rid) return -1;
    return ai_write_rel(ai, A, B, rid, 0, AI_WSRC_RELATION);
}

/* 这个智能体知不知道 A R B（只有它自己的图能回答） */
static int knows(AI *ai, const char *a, const char *rel, const char *b) {
    Token *A = pool_find(ai->pool, a);
    Token *B = pool_find(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 0);
    if (!A || !B || !rid) return 0;
    return rel_direct(A, rid, B);
}

int main(void) {
    /* ---------- ① 两个智能体共存 ---------- */
    AI *A = ai_new();
    AI *B = ai_new();
    ok(A && B && A != B, "① 一个进程里能开两个智能体（两个独立的 AI）");
    ok(A->pool != B->pool, "① 两个智能体各有各的池（不是共享一份图）");

    ok(teach(A, "猫", "属于", "动物") == 0, "① A 学到「猫 属于 动物」");
    ok(teach(B, "狗", "属于", "植物") == 0, "① B 学到「狗 属于 植物」");

    ok(knows(A, "猫", "属于", "动物") == 1, "① A 查得到自己学的");
    ok(knows(A, "狗", "属于", "植物") == 0, "① **A 查不到 B 学的**（不串台）");
    ok(knows(B, "狗", "属于", "植物") == 1, "① B 查得到自己学的");
    ok(knows(B, "猫", "属于", "动物") == 0, "① **B 查不到 A 学的**（不串台）");

    /* ---------- ② 修改账各记各的 ---------- */
    ok(ai_modlog_count(A) == 1, "② A 的修改账只有 1 条（自己那条）");
    ok(ai_modlog_count(B) == 1, "② B 的修改账只有 1 条（自己那条）");
    ok(ai_modlog_at(A, 0) && ai_modlog_at(B, 0), "② 两本账都能读第一条");
    {   /* 池的 id 是**各池自己**排的，两个池里同一个 id 可能指不同词 ⇒ 按"id → 名字"比 */
        const AIModEntry *ea = ai_modlog_at(A, 0), *eb = ai_modlog_at(B, 0);
        Token *ta = ea ? pool_token_by_id(A->pool, ea->b_id) : NULL;
        Token *tb = eb ? pool_token_by_id(B->pool, eb->b_id) : NULL;
        ok(ta && tb && ta->name && tb->name && strcmp(ta->name, tb->name) != 0,
           "② 两条记录指向不同的词（A=动物 / B=植物，各记各的）");
        ok(ta && ta->name && !strcmp(ta->name, "动物"),
           "② 账里的 id 能反查回名字（跨池也认得：记的是 id 不是指针）");
    }
    ok(ai_modlog_at(A, 0) && ai_modlog_at(A, 0)->rid != 0, "② 记录里带关系 id");
    ok(ai_modlog_at(A, 0) && ai_modlog_at(A, 0)->src == (uint8_t)AI_WSRC_RELATION,
       "② 记录里带写入来源");

    teach(A, "猫", "会", "游泳");
    ok(ai_modlog_count(A) == 2 && ai_modlog_count(B) == 1,
       "② A 再写一条 ⇒ **只有 A 的账涨**，B 的不动");
    ok(ai_modlog_at(A, 0) && ai_modlog_at(A, 0)->seq == 2,
       "② 序号单调（最新那条 seq=2）");

    /* 溢出：写满之后只丢最旧的，`n` 继续涨（账不能把内存吃干） */
    for (int i = 0; i < (int)AI_MODLOG_MAX + 3; ++i) teach(A, "猫", "属于", "动物"); /* 重复写也算一次修改 */
    ok(ai_modlog_count(A) == (size_t)AI_MODLOG_MAX + 5,
       "② 账是**有界环形**：写超上限后总数继续涨");
    ok(ai_modlog_at(A, AI_MODLOG_MAX) == NULL, "② 被覆盖的最旧条目读不到（不是假数据）");

    /* ---------- ③ 指针标签的地基 ---------- */
    void *p = malloc(64);
    ok(p && aip_ptr_taggable(p), "③ malloc 出来的指针是 8 字节对齐的（低 3 位为 0）");
    ok(aip_ptr_tag(p) == AIP_PTR_RAM, "③ 没打标签 = tag 0（内存）");
    ok(aip_ptr_untagged_identity(p), "③ **tag==0 与没打标签逐字节等价**（接进来零行为变化）");
    for (unsigned t = 0; t < 8; ++t) {
        void *q = aip_ptr_tagged(p, t);
        ok(aip_ptr_tag(q) == t, t == 0 ? "③ 打 tag 0 仍是内存"
                                       : (t == 1 ? "③ 打 tag 1 = 磁盘（往返正确）"
                                                 : (t == 2 ? "③ 打 tag 2 = 别处（往返正确）"
                                                           : "③ 保留 tag 往返正确")));
        if (aip_ptr_strip(q) != p) { ok(0, "③ strip 回到原指针"); break; }
    }
    ok(aip_ptr_strip(aip_ptr_tagged(p, AIP_PTR_DISK)) == p, "③ strip 之后还是原来那个指针");
    ok(aip_ptr_strip(NULL) == NULL && aip_ptr_tag(NULL) == 0u, "③ 空指针不打标签、不崩");
    free(p);

    printf("\n%s（多智能体：共存 %d / 各记各的 / 指针标签地基）\n",
           fails == 0 ? "PASS" : "FAIL", passes);
    ai_free(A);
    ai_free(B);
    return fails == 0 ? 0 : 1;
}
