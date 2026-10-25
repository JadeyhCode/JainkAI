/* tools/xbrain.c — 跨脑知识语义的探针（C 级，不经过 CLI）
 *
 * §6.580 的目标：**把跨脑搬运的语义定下来**，而且每个判断都能被机器钉住。
 * 三条规则（详见 `xbrain.h` 文件头）：
 *   A. 照抄 = 确定性迁移（不再"再学一遍"，结果不取决于目标脑当时的心情）；
 *   B. 本地优先 + 冲突可见（外来事实不静默覆盖本地知识，冲突记账、上屏）；
 *   C. `force` 明知冲突还是写：极性冲突**真覆盖**，独占冲突**并列不撤回**。
 *
 * 用法：
 *   gcc -std=c11 -O2 -Wall -Wextra -o build_xbrain tools/xbrain.c -lm && ./build_xbrain
 */
#include "../ai.h"
#include "../xbrain.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

/* 教一条事实（正/否定都支持；动态谓语会现场登记） */
static int teach(AI *ai, const char *a, const char *rel, const char *b, int neg) {
    Token *A = pool_token_intern(ai->pool, a);
    Token *B = pool_token_intern(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 1);
    if (!A || !B || !rid) return -1;
    return ai_write_rel(ai, A, B, rid, neg, AI_WSRC_RELATION);
}

static int knows(AI *ai, const char *a, const char *rel, const char *b, int neg) {
    Token *A = pool_find(ai->pool, a);
    Token *B = pool_find(ai->pool, b);
    if (!A || !B) return 0;
    uint8_t rid = rel_id_of(ai, rel, 0);
    if (!rid) return 0;
    return neg ? rel_direct_neg(A, rid, B) : rel_direct(A, rid, B);
}

/* 账里有没有某种动作（`::mods` 会显示的那几类） */
static int modlog_has(const AI *ai, unsigned kind) {
    for (size_t i = 0; i < ai_modlog_count(ai); ++i) {
        const AIModEntry *e = ai_modlog_at(ai, i);
        if (e && e->kind == (uint8_t)kind) return 1;
    }
    return 0;
}

int main(void) {
    AI *A = ai_new(), *B = ai_new();
    if (!A || !B) { printf("建不出两个 AI\n"); return 1; }
    const char *J = "_xbrain_tmp.j";
    size_t a_words0 = A->pool->count;

    /* ---------- ① 照抄：确定性迁移 ---------- */
    ok(teach(A, "窗户", "属于", "家具", 0) == 0, "① A 学到「窗户 属于 家具」");
    ok(A->pool->count == a_words0 + 2, "① A 的池只多了它自己学的两个词");
    int jn = xb_journal_save(A, J);
    ok(jn >= 1, "① 流水把这 1 条记下来了（只记自己改过的词）");

    size_t b_words0 = B->pool->count;
    size_t b_learned0 = B->n_rel_learned;
    XbResult r1;
    int w1 = xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r1);
    ok(w1 == 1 && r1.r.added == 1, "① 照抄进 B：落地 1 条");
    ok(r1.r.newwords == 2, "① B 原来不认识这两个词 ⇒ 现场建了 2 个（词级携带）");
    ok(B->pool->count == b_words0 + 2, "① 池里确实多了 2 个词");
    ok(knows(B, "窗户", "属于", "家具", 0) == 1, "① B 查得到（照抄成功）");
    ok(knows(A, "窗户", "属于", "家具", 0) == 1 && A->pool->count == a_words0 + 2,
       "① 源脑不受影响（没有因为别人照抄而多词/改图）");
    ok(B->n_rel_learned == b_learned0, "① **没走学习路径**（n_rel_learned 没涨 ⇒ 不是「再学一遍」）");
    ok(B->n_xcopy_added == 1, "① 照抄的账记上了（n_xcopy_added=1）");

    XbResult r2;
    xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r2);
    ok(r2.r.added == 0 && r2.r.known == 1, "① 再抄一遍是幂等的（已知 1 / 新增 0）");

    /* ---------- ② 极性冲突：本地优先 ---------- */
    ok(teach(A, "猫", "会", "飞", 0) == 0, "② A 学到「猫 会 飞」");
    ok(teach(B, "猫", "会", "飞", 1) == 0, "② B 学到的是反面「猫 不 会 飞」");
    xb_journal_save(A, J);
    XbResult r3;
    xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r3);
    ok(r3.r.conflicts == 1 && r3.r.overridden == 0,
       "② 极性打架 ⇒ **默认不写**（冲突 1、覆盖 0）");
    ok(knows(B, "猫", "会", "飞", 0) == 0 && knows(B, "猫", "会", "飞", 1) == 1,
       "② B 的知识没被外来事实改掉（本地优先）");
    ok(modlog_has(B, AIMOD_REL_CONFLICT), "② 冲突进了账（`::mods` 里能看到「冲突未写」）");
    ok(r3.n_lines >= 1, "② 冲突还上了屏（人能看到是哪条）");

    /* ---------- ③ force：极性冲突 = 真覆盖 ---------- */
    XbResult r4;
    xb_journal_replay(B, J, XB_MODE_FORCE, &r4);
    ok(r4.r.overridden == 1, "③ force ⇒ 覆盖 1 条");
    ok(knows(B, "猫", "会", "飞", 0) == 1 && knows(B, "猫", "会", "飞", 1) == 0,
       "③ 覆盖是真覆盖：肯定留下、对立极性被替掉（不是两条并存）");
    ok(modlog_has(B, AIMOD_REL_OVERRIDE), "③ 覆盖进了账（「强行覆盖」）");

    /* ---------- ④ 独占冲突：默认挡住，force 并列不撤回 ---------- */
    ok(teach(B, "桌子", "属于", "玩具", 0) == 0, "④ B 自己认为「桌子 属于 玩具」");
    ok(teach(A, "桌子", "属于", "家具", 0) == 0, "④ A 认为「桌子 属于 家具」");
    xb_journal_save(A, J);
    XbResult r5;
    xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r5);
    ok(r5.r.conflicts == 1 && r5.r.added == 0, "④ 独占关系已有别的值 ⇒ 冲突（默认不写）");
    ok(knows(B, "桌子", "属于", "玩具", 0) == 1 && knows(B, "桌子", "属于", "家具", 0) == 0,
       "④ B 仍说「桌子 属于 玩具」");
    XbResult r6;
    xb_journal_replay(B, J, XB_MODE_FORCE, &r6);
    ok(r6.r.overridden == 1, "④ force ⇒ 并列写入 1 条");
    ok(knows(B, "桌子", "属于", "玩具", 0) == 1 && knows(B, "桌子", "属于", "家具", 0) == 1,
       "④ **旧值没被撤回**（属于有嵌套：删旧知识比留两条更糟）");
    ok(r6.n_lines >= 1, "④ 输出里说清了「并列、没撤旧值」");

    /* ---------- ⑤ check：干跑，什么都不写 ---------- */
    ok(teach(A, "椅子x", "属于", "家具x", 0) == 0, "⑤ A 学到一条 B 完全没有的事实");
    xb_journal_save(A, J);
    size_t b_words1 = B->pool->count, b_mods1 = ai_modlog_count(B);
    XbResult r7;
    xb_journal_replay(B, J, XB_MODE_CHECK, &r7);
    ok(r7.r.added >= 1, "⑤ 干跑报了「会新增 N 条」");
    ok(B->pool->count == b_words1, "⑤ 干跑**没建词**");
    ok(ai_modlog_count(B) == b_mods1, "⑤ 干跑**没记账**");
    ok(knows(B, "椅子x", "属于", "家具x", 0) == 0, "⑤ 干跑之后 B 还是不知道");

    /* ---------- ⑥ 新词真的带过去了 ---------- */
    XbResult r8;
    int w8 = xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r8);
    ok(w8 >= 1 && r8.r.newwords == 2 && r8.r.added == 1,
       "⑥ 照抄把新词一起带过去（新建 2 个词 + 1 条事实）");
    ok(knows(B, "椅子x", "属于", "家具x", 0) == 1, "⑥ B 现在知道了");

    /* ---------- ⑦ 动态谓语：按名字带（id 跨脑不可比） ---------- */
    ok(teach(A, "我", "想念", "家", 0) == 0, "⑦ A 学到动态谓语「想念」");
    xb_journal_save(A, J);
    XbResult r9;
    xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r9);
    ok(rel_dyn_find(B, "想念") != 0, "⑦ 目标脑登记了「想念」这个谓语（按名字，不是 id）");
    ok(knows(B, "我", "想念", "家", 0) == 1, "⑦ 动态谓语的肯定事实也搬过去了");

    /* ---------- ⑧ 动态否定：15 位旗子也要带 ---------- */
    ok(teach(A, "猫", "喵", "狗", 1) == 0, "⑧ A 学到动态谓语的**否定**「猫 不 喵 狗」");
    xb_journal_save(A, J);
    XbResult r10;
    xb_journal_replay(B, J, XB_MODE_LOCAL_WINS, &r10);
    {
        Token *ta = pool_find(B->pool, "猫"), *tb = pool_find(B->pool, "狗");
        uint8_t rid = rel_id_of(B, "喵", 0);
        ok(ta && tb && rid && xb_pair_state(ta, rid, tb, 1) == 1,
           "⑧ 否定搬过去了（B 里「猫 不 喵 狗」为真、肯定那条为假）");
        /* ⚠ 这里只能断言 `xb_pair_state` 的口径：引擎自己的 `rel_direct` 对**动态**关系
         * 不区分极性（它只看 `e->dyn`，不看 15 位否定旗子；`rel_direct_as` 才区分）。
         * 那是引擎的老毛病，不在这一轮改（会改行为），照实记在审计里。 */
        ok(ta && tb && rid && xb_pair_state(ta, rid, tb, 0) == -1,
           "⑧ 照抄没有顺手写成肯定（照抄用的极性判据是对的）");
    }

    /* ---------- ⑨ 坏行只计数，不许乱写 ---------- */
    {
        FILE *f = fopen("_xbrain_bad.j", "wb");
        if (f) {
            fputs("R\t猫\t会\n", f);            /* 少一列 */
            fputs("R\t\t属于\t家具\t1\n", f);    /* 空主语 */
            fputs("这不是一行流水\n", f);
            fclose(f);
        }
        size_t mods0 = ai_modlog_count(B);
        XbResult r11;
        xb_journal_replay(B, "_xbrain_bad.j", XB_MODE_LOCAL_WINS, &r11);
        ok(r11.r.bad == 2 && r11.r.added == 0, "⑨ 坏行 2 条，一条都没落地");
        ok(ai_modlog_count(B) == mods0, "⑨ 坏行也没往账里写");
        remove("_xbrain_bad.j");
    }

    /* ---------- ⑩ 两个脑的账互不串台 ---------- */
    {
        size_t mods_a = ai_modlog_count(A), mods_b = ai_modlog_count(B);
        ok(mods_a >= 2 && mods_b > mods_a, "⑩ A 只记自己学的、B 另有照抄的账（各记各的）");
        ok(!modlog_has(A, AIMOD_REL_COPY) && modlog_has(B, AIMOD_REL_COPY),
           "⑩ 「照抄」只记在**接收方**的账上（源脑不会假装自己改过）");
    }

    /* ---------- ⑪ 反向也一样：冲突规则不偏向"谁抄谁" ---------- */
    {
        xb_journal_save(B, "_xbrain_back.j");
        XbResult r12;
        xb_journal_replay(A, "_xbrain_back.j", XB_MODE_LOCAL_WINS, &r12);
        /* B 的流水到这一步有 7 条：A 已经知道的 6 条（③ force 之后 B 的「猫 会 飞」与 A 一致）
         * + 1 条真的冲突（桌子：B 说玩具、A 说家具）⇒ 默认只挡那 1 条。 */
        ok(r12.r.conflicts == 1 && r12.r.added == 0 && r12.r.overridden == 0,
           "⑪ B→A 也是本地优先（桌子 属于 玩具 被 A 的「家具」挡住；其余已知）");
        ok(knows(A, "桌子", "属于", "玩具", 0) == 0 && knows(A, "桌子", "属于", "家具", 0) == 1,
           "⑪ A 的知识没被反方向抄写改掉");
        XbResult r13;
        xb_journal_replay(A, "_xbrain_back.j", XB_MODE_FORCE, &r13);
        ok(r13.r.overridden == 1, "⑪ B→A 加 force：那 1 条冲突写了");
        ok(knows(A, "桌子", "属于", "玩具", 0) == 1 && knows(A, "桌子", "属于", "家具", 0) == 1,
           "⑪ 独占冲突并列（两条都在），极性冲突被覆盖");
        remove("_xbrain_back.j");
    }

    remove(J);
    ai_free(A);
    ai_free(B);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
