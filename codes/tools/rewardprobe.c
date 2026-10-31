/* tools/rewardprobe.c — **奖励机制探针**（§6.634）
 *
 * 盯的是"账目"而不是"情绪"：短期值夹紧、会话累计不夹、事件数、最近 8 个事件的环、
 * 以及**默认只计量**（调节口 off）与奖励门的三条判据。
 *
 * 用法：gcc -std=c11 -O2 -o build_rewardprobe tools/rewardprobe.c -lm && ./build_rewardprobe
 */
#include "../agent.h"
#include "../model_io.h"

#include <stdio.h>
#include <string.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

int main(void) {
    AI *a = ai_new();
    printf("== 奖励机制（账目 + 一个可开的调节口） ==\n");

    ok(a->reward == 0.0f && a->reward_cum == 0.0f && a->reward_n == 0, "初始：短期/累计/事件都是 0");
    ok(a->reward_gate == 0, "**默认只计量**（调节口 off）—— 不偷偷改行为");

    ai_reward(a, +1.0f, "学会一条");
    ok(a->reward > 0.99f && a->reward < 1.01f, "+1 ⇒ 短期 = 1");
    ok(a->reward_n == 1, "事件数 +1");

    ai_reward(a, +10.0f, "超大正奖励");
    ok(a->reward == 5.0f, "短期值**夹在 +5**（不无限涨）");
    ok(a->reward_cum > 10.9f, "会话累计**不夹**（累计是账，不是状态）");

    ai_reward(a, -20.0f, "超大负奖励");
    ok(a->reward == -5.0f, "短期值**夹在 −5**");

    int n_before = a->reward_n;
    ai_reward(a, 0.0f, "零奖励");
    ok(a->reward_n == n_before, "d == 0 ⇒ 什么都不记（不刷事件数）");

    /* 环：最近 8 条，最新的在 rhead-1 */
    for (int i = 0; i < 10; ++i) ai_reward(a, +0.1f, "填充");
    {
        size_t newest = (a->rhead + 8 - 1) % 8;
        ok(!strcmp(a->rlog[newest].what, "填充"), "环形账：最新那条在 rhead-1");
        size_t oldest = a->rhead % 8;
        ok(strcmp(a->rlog[oldest].what, "零奖励") != 0, "环形账只留最近 8 条（更旧的被覆盖）");
    }

    /* 奖励门（默认 off） */
    a->reward = 0.0f; a->ask_conflict = 0;
    ok(agent_conflict_ask_on(a) == 0, "门 off + 短期 0 ⇒ 不多问");
    a->ask_conflict = 1;
    ok(agent_conflict_ask_on(a) == 1, "用户自己开了 ask ⇒ 照问（奖励门不干扰）");
    a->ask_conflict = 0; a->reward_gate = 1; a->reward = -1.5f;
    ok(agent_conflict_ask_on(a) == 1, "门 on + 短期 ≤ −1（最近错得多）⇒ **多问一句要不要改**");
    a->reward = +2.0f;
    ok(agent_conflict_ask_on(a) == 0, "门 on + 短期为正 ⇒ 不多问（不烦人）");

    /* 每轮衰减 */
    a->reward = 4.0f; a->reward_gate = 0; a->ask_conflict = 0;
    ai_reward_tick(a);
    ok(a->reward > 3.5f && a->reward < 3.7f, "tick：×0.9（4 → 3.6）");
    a->reward = 0.01f;
    ai_reward_tick(a);
    ok(a->reward == 0.0f, "tick：|值| < 0.05 ⇒ 归零（不拖小尾巴）");

    /* 会话内：不进存档（存档是"知识"，奖励是"状态"） */
    {
        a->reward = 3.0f; a->reward_cum = 7.0f; a->reward_n = 5;
        size_t sz = ai_save_file(a, "_rewardprobe.bin");
        AI *b = sz ? ai_load_file("_rewardprobe.bin") : NULL;
        ok(b != NULL, "带奖励状态的脑能存能读");
        if (b) {
            ok(b->reward == 0.0f && b->reward_cum == 0.0f && b->reward_n == 0,
               "**奖励不进存档**（读回来是 0：它是会话状态，不是知识）");
            ai_free(b);
        }
        remove("_rewardprobe.bin");
    }

    ai_free(a);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
