/* tools/poolshare.c — **池级共享**的探针（§6.581，C 级，不经过 CLI）
 *
 * 用户 2026-10-24：「池级共享可以干一下」。
 *
 * 这一轮诚实结论先写在最前面（探针也照着它验收）：
 *   · **边数组不能跨池共享**：`AssocEntry.main/action` 存的是 `Token*`，两个脑要各自改图就得各有各的
 *     `Token` 对象 ⇒ 边里的指针全要重写 ⇒ 共享不了（真要共享得把边改成"存 id"，那是全仓重构）。
 *   · 能共享的是**不带指针的大块**：`emb`（1024 维 int8/词，出厂图 ≈1.5MB/脑）。
 *     机制 = 引用计数 + **写前复制**（`share.h`）。
 *
 * 这个探针钉住五件事：
 *   ① 共享真的发生了（指针同一、计数=2、省下的字节数 = 词数×1024）；
 *   ② 共享**不改答案**（同一批关系问答在共享前后一致）；
 *   ③ 写前复制（重建 emb）之后两边各走各的，**没把兄弟脑的向量改掉**；
 *   ④ 释放顺序随便（先释放源脑，另一个脑照样能用、emb 内容不变）；
 *   ⑤ 图不一样时**拒绝共享**（错位共享比不共享危险）。
 *
 * 用法：
 *   gcc -std=c11 -O2 -Wall -Wextra -o build_poolshare tools/poolshare.c -lm && ./build_poolshare model.bin
 */
#include "../agent.h"
#include "../model_io.h"
#include "../share.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

/* 所有词 emb 的字节和（用来证明"没被兄弟脑改掉"） */
static long long emb_sum(const AI *ai) {
    long long s = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        if (!t || !t->emb) continue;
        for (size_t d = 0; d < EMB_DIM; ++d) s += t->emb[d];
    }
    return s;
}

/* 同一批关系问答：共享前后必须一致（这是"共享不影响行为"的机器证据） */
static int relation_ok(AI *ai, const char *a, const char *rel, const char *b) {
    Token *A = pool_find(ai->pool, a), *B = pool_find(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 0);
    if (!A || !B || !rid) return -1;
    return rel_direct(A, rid, B);
}

int main(int argc, char **argv) {
    const char *model = argc > 1 ? argv[1] : "model.bin";
    AI *A = ai_load_file(model);
    AI *B = ai_load_file(model);
    if (!A || !B) { printf("读不到 %s\n", model); return 1; }

    /* 共享前先记一份"行为基线" */
    int f1_before = relation_ok(A, "猫", "属于", "动物");
    int f2_before = relation_ok(A, "猫", "会", "游泳");
    long long sum_a_before = emb_sum(A);

    /* ---------- ① 共享真的发生了 ---------- */
    size_t n = pool_share_emb(B->pool, A->pool);
    ok(n == A->pool->count, "① 整池共享（每个词的 emb 都是同一块内存）");
    {
        int all_same = 1;
        for (size_t i = 0; i < A->pool->count; ++i)
            if (A->pool->tokens[i]->emb != B->pool->tokens[i]->emb) { all_same = 0; break; }
        ok(all_same, "① 指针逐个相同（不是「复制了一份看起来一样的数据」）");
    }
    ok(aip_buf_refs(A->pool->tokens[0]->emb) == 2, "① 引用计数 = 2");
    {
        size_t saved = aip_buf_shared_bytes(EMB_DIM);
        size_t expect = n * EMB_DIM;
        printf("       省下 %zu 字节（%zu 个词 × %d 维）；实测表里 %zu 块\n",
               saved, n, EMB_DIM, aip_buf_shared_count());
        ok(saved == expect, "① 省下的字节数 = 词数 × 1024（这块内存是真的只有一份）");
    }

    /* ---------- ② 共享不改答案 ---------- */
    ok(relation_ok(B, "猫", "属于", "动物") == f1_before && f1_before == 1,
       "② 共享之后 B 对「猫 属于 动物」的答案和 A 一样（而且还是「对」）");
    ok(relation_ok(B, "猫", "会", "游泳") == f2_before,
       "② 否定那条（猫 不 会 游泳）也一致");

    /* ---------- ③ 写前复制：重建 B 的 emb 不许动到 A ---------- */
    ai_embed_build(B);
    ok(B->pool->tokens[0]->emb != A->pool->tokens[0]->emb,
       "③ 重建 emb 之后两边不再是同一块内存（写前复制生效）");
    ok(aip_buf_refs(A->pool->tokens[0]->emb) == 1,
       "③ A 那份退回「私有」（表里不再有它）");
    ok(aip_buf_shared_count() == 0, "③ 共享表清空了");
    ok(emb_sum(A) == sum_a_before, "③ **A 的向量一个字节没变**（这是写前复制存在的理由）");
    ok(relation_ok(A, "猫", "属于", "动物") == f1_before, "③ 复制之后 A 的答案照旧");

    /* ---------- ④ 再共享一次 + 任意顺序释放 ---------- */
    size_t n2 = pool_share_emb(B->pool, A->pool);
    ok(n2 == A->pool->count && aip_buf_refs(A->pool->tokens[0]->emb) == 2,
       "④ 可以再共享（复制掉的那份照样能再共享回去）");
    {
        long long sum_b = emb_sum(B);
        ai_free(A);                      /* 先释放"源脑" */
        A = NULL;
        ok(emb_sum(B) == sum_b, "④ 源脑释放之后，另一个脑的向量完好（引用计数没提前 free）");
        ok(relation_ok(B, "猫", "属于", "动物") == f1_before, "④ 另一个脑照样能答（没被 UAF）");
    }

    /* ---------- ⑤ 图不一样 ⇒ 拒绝共享（错位共享比不共享危险） ---------- */
    {
        AI *C = ai_new();                /* 空白脑：词数/名字都对不上 */
        size_t r = pool_share_emb(C->pool, B->pool);
        ok(r == 0, "⑤ 图不一样时**拒绝**共享（返回 0，不是「共享了一半」）");
        ok(aip_buf_shared_count() == 0, "⑤ 拒绝时连表都没动");
        ai_free(C);
    }

    /* ---------- ⑥ 三个脑：省的是"每多一个脑一份" ---------- */
    {
        AI *X = ai_load_file(model), *Y = ai_load_file(model), *Z = ai_load_file(model);
        size_t nx = pool_share_emb(Y->pool, X->pool);
        size_t ny = pool_share_emb(Z->pool, X->pool);
        size_t saved = aip_buf_shared_bytes(EMB_DIM);
        printf("       三脑：共享 %zu + %zu 个词 ⇒ 省 %zu 字节（≈ %zu KB）\n",
               nx, ny, saved, saved / 1024);
        ok(nx == X->pool->count && ny == X->pool->count, "⑥ 两个兄弟脑都共享到同一个源");
        ok(saved == 2u * X->pool->count * EMB_DIM, "⑥ 省下的正是「两个兄弟各一份」那么多");
        ai_free(Z); ai_free(Y); ai_free(X);
    }

    ai_free(B);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
