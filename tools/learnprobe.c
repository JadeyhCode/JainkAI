/* tools/learnprobe.c — **交互学习可用性**验收（§6.585，C 级）
 *
 * 用户目标第三阶段：「解决新旧知识冲突等学习方面的问题，完善学习的机制，
 * 使得在交互中学习的能力可用」。
 *
 * 这一套钉住"学得进、用得上、改得动、说不含糊、存得住"五件事：
 *   ① 教新词/新事实 → 立刻问得出来（含现场造词）；
 *   ② 嵌套的 `属于` **不被误撤**（猫 ⊂ 猫科 ⊂ 动物：三条都真）；
 *   ③ 互不包含的独占值：新的落地、旧的**被撤**，而且撤了哪条要**说出来**；
 *   ④ 极性改口（`不` 反过来）与再次改回；
 *   ⑤ 存读往返之后：答案、`[修正:…]` 标记、账目都还在。
 *
 * 用法：
 *   gcc -std=c11 -O2 -Wall -Wextra -o build_learnprobe tools/learnprobe.c -lm && ./build_learnprobe model.bin
 */
#include "../agent.h"
#include "../model_io.h"
#include "../aicheck.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

static Agent *g_ag;
static char   g_out[1024];

/* 说一句（走完整流水线：分句 → 关系学习/答复）*/
static const char *say(AI *ai, const char *s) {
    if (!g_ag) return "";
    if (g_ag->brain != ai) { g_ag->brain = ai; }
    memset(g_out, 0, sizeof g_out);
    agent_step(g_ag, s, g_out, sizeof g_out);
    return g_out;
}

static int knows(AI *ai, const char *a, const char *rel, const char *b, int neg) {
    Token *A = pool_find(ai->pool, a), *B = pool_find(ai->pool, b);
    uint8_t rid = rel_id_of(ai, rel, 0);
    if (!A || !B || !rid) return 0;
    return neg ? rel_direct_neg(A, rid, B) : rel_direct(A, rid, B);
}

int main(int argc, char **argv) {
    const char *model = argc > 1 ? argv[1] : "model.bin";
    g_ag = agent_new(stdin_perceive, stdout_act, NULL);
    if (!g_ag) { printf("建不出 Agent\n"); return 1; }
    ai_free(g_ag->brain);

    /* ---------- A. 空白脑子：嵌套不误报、互不包含的冲突**可见可撤**、极性改口 ---------- */
    {
        AI *ai = ai_new();
        g_ag->brain = ai;
        ok(ai_check_ok(ai), "A 空白脑子通过不变式");
        say(ai, "猫 属于 猫科");
        say(ai, "猫科 属于 动物");
        const char *rn = say(ai, "猫 属于 动物");
        ok(strstr(rn, "冲突") == NULL, "A 嵌套的 `属于` 不当成冲突（猫 ⊂ 猫科，两条都真）");
        ok(knows(ai, "猫", "属于", "猫科", 0) && knows(ai, "猫", "属于", "动物", 0),
           "A 嵌套两条都真");
        ok(ai->n_conflict_shown == 0, "A 嵌套场景没有冲突提示（n_conflict_shown=0）");

        const char *r = say(ai, "猫 属于 植物");
        ok(strstr(r, "冲突") != NULL, "A 互不包含的新值：回复**说清冲突**（不会静默）");
        ok(strstr(r, "要 撤") != NULL, "A 回复里给出撤法（「猫 不 属于 动物」）");
        ok(knows(ai, "猫", "属于", "植物", 0) && knows(ai, "猫", "属于", "动物", 0),
           "A 学到的与旧的**都在**（不静默删知识 —— §6.199 的老判定）");
        ok(ai->n_conflict_shown >= 1, "A 冲突计数记上了（n_conflict_shown>=1）");

        /* 显式撤：说「不 属于 动物」—— 走极性改口，可追溯 */
        const char *rr = say(ai, "猫 不 属于 动物");
        ok(strstr(rr, "改 过来 了") != NULL, "A 显式撤旧值：回复说「改 过来 了」");
        ok(knows(ai, "猫", "属于", "动物", 1) && !knows(ai, "猫", "属于", "动物", 0),
           "A 撤之后旧值答「不 对」（旧值为假、否定为真）");
        ok(knows(ai, "猫", "属于", "植物", 0), "A 新的那条不受影响");

        say(ai, "猫 不 属于 植物");
        ok(knows(ai, "猫", "属于", "植物", 1) && !knows(ai, "猫", "属于", "植物", 0),
           "A 极性改口：后说的否定替掉先说的肯定");
        say(ai, "猫 属于 植物");
        ok(knows(ai, "猫", "属于", "植物", 0), "A 再改回来也生效");

        /* 存读往返：知识/标记都在 */
        size_t sz = ai_save_file(ai, "_learnprobe.bin");
        AI *back = sz ? ai_load_file("_learnprobe.bin") : NULL;
        ok(back != NULL, "A 存读往返：文件能读回来");
        if (back) {
            ok(knows(back, "猫", "属于", "植物", 0) && knows(back, "猫", "属于", "动物", 1),
               "A 存读之后答案还在（植物真、动物假）");
            ok(pool_find(back->pool, "[修正:猫_不_属于_动物]") != NULL ||
               pool_find(back->pool, "[修正:猫_属于_植物]") != NULL,
               "A 存读之后 `[修正:…]` 标记还在（改过的事记得住）");
            ai_free(back);
        }
        remove("_learnprobe.bin");
        ai_free(ai);
        g_ag->brain = NULL;
    }

    /* ---------- B. 真模型 + 自然语言：一次"教错再改口"的完整会话 ---------- */
    {
        AI *ai = ai_load_file(model);
        if (!ai) { printf("读不到 %s\n", model); return 1; }
        g_ag->brain = ai;
        ai_check_dump_if_bad(ai, "B 载入");

        ok(say(ai, "狗 属于 动物")[0] != 0, "B 教一条（模型已知道）有答复");
        const char *r = say(ai, "狗 属于 植物");
        ok(r[0] != 0 && strstr(r, "冲突") != NULL,
           "B 教冲突的独占事实：回复说清跟哪条冲突");
        ok(knows(ai, "狗", "属于", "植物", 0) && knows(ai, "狗", "属于", "动物", 0),
           "B 两条都在（先不删知识，等用户明确撤）");

        /* 明确撤旧值：说「不 属于 动物」*/
        const char *r2 = say(ai, "狗 不 属于 动物");
        ok(strstr(r2, "改 过来 了") != NULL && !knows(ai, "狗", "属于", "动物", 0),
           "B 显式撤：说「狗 不 属于 动物」之后旧值为假");

        /* 账：至少要有「加关系」（撤是通过极性改口记的，不是静默删边）*/
        int has_add = 0;
        for (size_t i = 0; i < ai_modlog_count(ai); ++i) {
            const AIModEntry *e = ai_modlog_at(ai, i);
            if (e && e->kind == AIMOD_REL) has_add = 1;
        }
        ok(has_add, "B 账里有「加关系」（每次学都记账）");

        /* 再教一遍同一条：幂等（回「我知道」）*/
        const char *r3 = say(ai, "狗 属于 植物");
        ok(strstr(r3, "我 知道") != NULL, "B 重复教同一条：回「我知道」");

        ok(ai_check_ok(ai), "B 整个会话之后不变式干净");
        ai_free(ai);
        g_ag->brain = NULL;
    }

    free(g_ag);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
