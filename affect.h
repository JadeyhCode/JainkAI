/* affect.h — **情绪层**（在内核之外）
 *
 * 用户裁定：「情绪我并不认为应该作为内核的一部分」。
 * 所以情绪**不写进内核**：耦合表、驱动量取值、"折算成预算修正"全在这里。
 * 内核（token.h / rel.h / derive.h / micro.h / think.h / ai.h 的图与判定）
 * **不知道什么是情绪** —— 它只看到三个普通数字（`affect_d_steps / affect_d_gen /
 * affect_d_cutoff`），默认 0，谁都可以设；关掉这一层与"有情绪但没命中"在内核看来完全一样。
 *
 * 这一层由**代理循环**（`agent.h`）每轮调一次；`::drive` 也调它做预览。
 * 内部状态量（舒适/唤醒/疲劳/不适）仍在 `AI` 里 —— 那是**存档格式的一部分**（v9，动不得）；
 * 但它们的*语义*由这一层解释，内核只把它们当普通数字。
 *
 * 设计纪律（和别处一样）：**一行一个耦合**，可解释、可测量（`tools/affectprobe.c`），
 * 默认关，要改行为先 A/B。 */
#ifndef AFFECT_H
#define AFFECT_H

#include "ai.h"

/* ============================ 情绪 / 驱动参与决策（层） ============================
 *
 * 做成**一张表**（一行一个耦合），不是散落的 if：`驱动量 → 参数 → 区间与增减`。
 * 默认**关**（`::affect on` 打开）—— 它改的是思考预算/生成长度/激活阈值这类行为参数，
 * 按规矩先实现、先 A/B。 */
typedef struct {
    const char *drive;   /* 驱动量名 */
    const char *param;   /* 改哪个参数 */
    float  lo, hi;       /* 驱动量落在这个区间才起作用 */
    int    delta;        /* 作用量（步数/词数/百分之一阈值） */
    const char *note;
} AffectCoupling;

/* ⚠ 区间是**量出来的**，不是拍的（`tools/affectprobe.c` 回放两种输入）：
 *     硬输入（陌生语料 150 行）：舒适 p05/p50/p95 = 0.10/0.11/0.40，唤醒 0.73/0.90/0.90，
 *                                 疲劳 0.01/0.07/0.14，不适 0.40/0.93/0.99
 *     熟输入（关系库/帧库 100 行）：舒适 0.12/0.32/0.58，唤醒 0.46/0.80/0.90，
 *                                 疲劳 0.01/0.05/0.10，不适 0.12/0.63/0.95
 * 量出来的三件事（照实说）：
 *   ① 疲劳**到不了 0.60**（每轮 +0.001，睡眠清零 → 一个周期最多 ~0.2）→ 原来那行**永远不触发**；
 *   ② 唤醒**到不了 0.25**（它 = 预测对不对的低通，最低也 0.46）→「迷糊时少说两句」也**永不触发**；
 *   ③ 舒适与唤醒的相关系数 **r=0.91**（都是"猜得准不准"的函数）→ 两行其实在说同一件事。
 * 所以：把疲劳区间按实测下移到 p90 附近；把永不触发的"唤醒低"换成**真正会低**的量（不适高）；
 * 给舒适补一条对称的"舒服时敢想远一点"。 */
static const AffectCoupling AI_AFFECT[] = {
    { "疲劳", "思考步数", 0.08f, 1.00f, -4, "这一轮工作周期快到头了 → 少想几步（睡一觉就回来）" },
    { "唤醒", "生成长度", 0.70f, 1.00f, +2, "警醒时多说两句" },
    { "不适", "生成长度", 0.90f, 1.00f, -1, "完全没底时少说两句" },
    { "舒适", "激活阈值", 0.00f, 0.35f, +3, "不舒服时保守一点（阈值抬高 0.03）" },
    { "舒适", "激活阈值", 0.55f, 1.00f, -2, "舒服时敢想远一点（阈值降 0.02）" },
    { NULL, NULL, 0, 0, 0, NULL }
};
#define AI_AFFECT_N ((sizeof AI_AFFECT / sizeof AI_AFFECT[0]) - 1)

/* 取某个驱动量的当前值（0..1）—— 读的是 AI 里的**普通数字**，不是"情绪" */
static inline float ai_drive_value(const AI *ai, const char *drive) {
    if (!ai || !drive) return 0.0f;
    if (!strcmp(drive, "舒适")) return ai->inner.comfort;
    if (!strcmp(drive, "唤醒")) return ai->inner.arousal;
    if (!strcmp(drive, "疲劳")) return ai->inner.fatigue;
    if (!strcmp(drive, "不适")) return ai->inner.discomfort;   /* 惊讶水平的低通 */
    return 0.0f;
}

/* 每轮调一次：把驱动量折算成**三个数字**交给内核（层关着时三个都是 0）。 */
static inline void affect_policy_step(AI *ai) {
    if (!ai) return;
    ai->affect_d_steps = 0; ai->affect_d_gen = 0; ai->affect_d_cutoff = 0.0f;
    if (!ai->affect_on) return;
    for (size_t i = 0; i < AI_AFFECT_N; ++i) {
        const AffectCoupling *c = &AI_AFFECT[i];
        float v = ai_drive_value(ai, c->drive);
        if (v < c->lo || v > c->hi) continue;
        if (!strcmp(c->param, "思考步数"))      ai->affect_d_steps += c->delta;
        else if (!strcmp(c->param, "生成长度")) ai->affect_d_gen += c->delta;
        else if (!strcmp(c->param, "激活阈值")) ai->affect_d_cutoff += (float)c->delta / 100.0f;
    }
}

#endif /* AFFECT_H */
