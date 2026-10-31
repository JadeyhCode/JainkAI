/* tools/predprobe.c — **谓词（多元关系）探针**（阶段二 WP1；§6.633）
 *
 * 盯的是：谓词实例能造、参数顺序对、元数对、找不到就是找不到、存读不丢、
 * 与现有三元关系互不污染、以及"拒绝"的那几条（0 参数/9 参数/拿参数关系当谓词名）。
 *
 * 用法：gcc -std=c11 -O2 -o build_predprobe tools/predprobe.c -lm && ./build_predprobe [model.bin]
 *   给了模型文件就在**真图**上再跑一遍（多一个"和现有知识共存"的检查）；不给就用空脑。
 */
#include "../agent.h"
#include "../model_io.h"
#include "../pred.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

int main(int argc, char **argv) {
    const char *model = (argc > 1) ? argv[1] : NULL;
    AI *a = model ? ai_load_file(model) : ai_new();
    if (!a) { printf("读不到 %s\n", model); return 1; }
    printf("== 谓词（多元关系）：%s ==\n", model ? model : "空脑");

    /* ① 造 + 读：二元 */
    char *e2[2] = { "甲", "乙" };
    Token *p = pred_make(a, "敌人", e2, 2);
    ok(p != NULL, "pred_make 能造二元谓词实例");
    ok(p && !strcmp(p->name, "[谓词:敌人(甲,乙)]"),
       "实例节点名是 `[谓词:敌人(甲,乙)]`（**参数进名字** ⇒ 实例=节点）");
    int aok = 0;
    ok(p && pred_arity(a, p, &aok) == 2 && aok, "元数 = 2，且编号连续（1..2 都在）");
    ok(p && pred_arg(a, p, 1) && !strcmp(pred_arg(a, p, 1)->name, "甲"), "第 1 个参数 = 甲（顺序对）");
    ok(p && pred_arg(a, p, 2) && !strcmp(pred_arg(a, p, 2)->name, "乙"), "第 2 个参数 = 乙");
    ok(pred_arg(a, p, 3) == NULL, "第 3 个参数不存在 ⇒ NULL（不返回垃圾）");

    /* ② 三元 */
    char *e3[3] = { "丙", "丁", "戊" };
    Token *p3 = pred_make(a, "给", e3, 3);
    int aok3 = 0;
    ok(p3 && pred_arity(a, p3, &aok3) == 3 && aok3, "三元谓词元数 = 3");
    ok(p3 && pred_arg(a, p3, 3) && !strcmp(pred_arg(a, p3, 3)->name, "戊"), "第 3 个参数 = 戊");

    /* ③ 拒绝的几条（不猜、不截断） */
    char *nine[9] = { "a", "b", "c", "d", "e", "f", "g", "h", "i" };
    ok(pred_make(a, "九元", nine, 9) == NULL, "9 个参数 ⇒ 拒绝（上限 8）");
    ok(pred_make(a, "零元", nine, 0) == NULL, "0 个参数 ⇒ 拒绝");
    ok(pred_make(a, "参数1", e2, 2) == NULL, "拿「参数1」当谓词名 ⇒ 拒绝（别嵌套自身）");
    ok(pred_make(a, "敌人", NULL, 2) == NULL, "args == NULL ⇒ 拒绝");
    char *bad[2] = { "甲", "" };
    ok(pred_make(a, "空参", bad, 2) == NULL, "有空参数 ⇒ 拒绝");

    /* ④ 同名不同实例：按参数全等找 */
    char *e2b[2] = { "甲", "丙" };
    Token *pb = pred_make(a, "敌人", e2b, 2);
    ok(pb != NULL, "同名谓词的**另一个实例**也能造（敌人(甲,丙)）");
    ok(pred_find(a, "敌人", e2, 2) != NULL && pred_find(a, "敌人", e2b, 2) != NULL,
       "pred_find 两个实例（甲乙/甲丙）都找得到");
    ok(pred_find(a, "敌人", e2, 2) != pred_find(a, "敌人", e2b, 2), "两个实例是**不同节点**（不互相覆盖）");
    ok(pred_find(a, "敌人", e2b, 2) == pb, "pred_find 返回的是**参数全等**的那一个");
    char *wrong[2] = { "甲", "庚" };
    ok(pred_find(a, "敌人", wrong, 2) == NULL, "参数对不上 ⇒ NULL（不瞎认）");
    ok(pred_find(a, "敌人", e2, 3) == NULL, "**多给一个参数**就对不上 ⇒ NULL（元数必须一致）");

    /* ⑤ 与三元关系互不污染 */
    Token *A = pool_token_intern(a->pool, "甲");
    Token *B = pool_token_intern(a->pool, "乙");
    ai_write_rel(a, A, B, REL_IS_A, 0, AI_WSRC_RELATION);
    ok(rel_direct(A, REL_IS_A, B), "普通三元关系（甲 属于 乙）照常可写可查");
    int aok_p = 0;
    ok(pred_arity(a, p, &aok_p) == 2 && aok_p, "给普通词加三元边**不影响**谓词元数");

    /* ⑥ 结构坏掉要如实说：三元实例里把**第 2 条**参数边清掉（名字还说 3 个） */
    {
        char *e3b[3] = { "己", "庚", "辛" };
        Token *pbad = pred_make(a, "坏例", e3b, 3);
        char rn[24];
        uint8_t rid = rel_id_of(a, pred_arg_relname(2, rn, sizeof rn), 0);
        if (pbad && rid) {
            for (size_t i = 0; i < pbad->count; ++i) {
                AssocEntry *e = &token_assocs(pbad)[i];
                if (e->dyn == (uint8_t)(rid - REL_DYN_BASE + 1)) { e->dyn = 0; e->rel = 0; break; }
            }
            int aok_bad = 1;
            ok(pred_arity(a, pbad, &aok_bad) == 3 && !aok_bad,
               "名字说 3 个参数、边上缺一个 ⇒ 元数仍是 3 但 ok=0（如实报坏，不改口）");
            ok(pred_find(a, "坏例", e3b, 3) == NULL, "结构坏的实例 ⇒ pred_find 不认（不当成完整事实）");
        } else {
            ok(0, "造「坏例」失败（探针本身有问题）");
        }
    }

    /* ⑦ 存读往返：谓词与参数随图存盘（动态关系表 v7 起就进存档） */
    {
        size_t sz = ai_save_file(a, "_predprobe.bin");
        AI *b = sz ? ai_load_file("_predprobe.bin") : NULL;
        ok(b != NULL, "带谓词的图能存能读");
        if (b) {
            char *fm[2] = { "甲", "乙" };
            Token *q = pred_find(b, "敌人", fm, 2);
            int bok = 0;
            ok(q != NULL && pred_arity(b, q, &bok) == 2 && bok, "读回来之后元数与参数都还在");
            ok(q && pred_arg(b, q, 2) && !strcmp(pred_arg(b, q, 2)->name, "乙"), "读回来之后第 2 个参数还是 乙");
            ai_free(b);
        }
        remove("_predprobe.bin");
    }

    /* ⑧ 名字判定 */
    ok(pred_is_arg_rel("参数1") && pred_is_arg_rel("参数12"), "参数关系名识别（参数N）");
    ok(!pred_is_arg_rel("参数") && !pred_is_arg_rel("参数X"), "「参数」「参数X」不算参数关系");
    ok(pred_is_pred_name("[谓词:敌人]") && !pred_is_pred_name("敌人"), "谓词节点名识别（必须带前缀）");

    /* ⑨ 列出不同名字 */
    {
        char names[16][64];
        size_t n = pred_list_names(a, names, 16);
        ok(n >= 2, "pred_list_names 能列出 ≥2 个不同谓词名（敌人/给）");
    }

    /* ⑩ 真模型上：和现有知识共存 */
    if (model) {
        Token *cat = pool_find(a->pool, "猫");
        Token *ani = pool_find(a->pool, "动物");
        ok(cat && ani && rel_direct(cat, REL_IS_A, ani), "真模型原本的知识（猫 属于 动物）没被破坏");
        ok(pred_find(a, "敌人", e2, 2) != NULL, "真模型上也找得到刚造的谓词实例");
    }

    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
