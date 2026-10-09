/* drive.h — **驱动层**（在内核之外）
 *
 * 名字的来历（2026-10-07 用户裁定）：这一层原来叫 `affect.h` / "情绪层"，
 * 但它的语义一直是「内部状态 → 预算修正」—— 叫"情绪"名不副实，所以**只改名，语义一个字没动**。
 * 用户原话：「情绪我并不认为应该作为内核的一部分」。
 * 所以内部状态**不写进内核**：耦合表、驱动量取值、"折算成预算修正"全在这里。
 * 内核（token.h / rel.h / derive.h / micro.h / think.h / ai.h 的图与判定）
 * **不知道什么是驱动量** —— 它只看到三个普通数字（`drive_d_steps / drive_d_gen /
 * drive_d_cutoff`），默认 0，谁都可以设；关掉这一层与"开着但一行都没命中"在内核看来完全一样。
 *
 * 这一层由**代理循环**（`agent.h`）每轮调一次；`::drive` 也调它做预览（`::drive on|off` 是开关）。
 * 内部状态量（舒适/唤醒/疲劳/不适）仍在 `AI` 里 —— 那是**存档格式的一部分**（v9，动不得）；
 * 但它们的*语义*由这一层解释，内核只把它们当普通数字。
 *
 * 设计纪律（和别处一样）：**一行一个耦合**，可解释、可测量（`tools/driveprobe.c`），
 * 默认关，要改行为先 A/B。 */
#ifndef DRIVE_H
#define DRIVE_H

#include "ai.h"

/* ============================ 驱动参与决策（层） ============================
 *
 * 做成**一张表**（一行一个耦合），不是散落的 if：`驱动量 → 参数 → 区间与增减`。
 * 默认**关**（`::drive on` 打开）—— 它改的是思考预算/生成长度/激活阈值这类行为参数，
 * 按规矩先实现、先 A/B。 */
typedef struct {
    const char *drive;   /* 驱动量名 */
    const char *param;   /* 改哪个参数 */
    float  lo, hi;       /* 驱动量落在这个区间才起作用 */
    int    delta;        /* 作用量（步数/词数/百分之一阈值） */
    const char *note;
} DriveCoupling;

/* ⚠ 区间是**量出来的**，不是拍的（`tools/driveprobe.c` 回放两种输入）：
 *     硬输入（陌生语料 150 行）：舒适 p05/p50/p95 = 0.10/0.11/0.40，唤醒 0.73/0.90/0.90，
 *                                 疲劳 0.01/0.07/0.14，不适 0.40/0.93/0.99
 *     熟输入（关系库/帧库 100 行）：舒适 0.12/0.32/0.58，唤醒 0.46/0.80/0.90，
 *                                 疲劳 0.01/0.05/0.10，不适 0.12/0.63/0.95
 * 量出来的三件事（照实说）：
 *   ① 疲劳**到不了 0.60**（每轮 +0.001，睡眠清零 → 一个周期最多 ~0.2）→ 原来那行**永远不触发**；
 *   ② 唤醒**到不了 0.25**（它 = 预测对不对的低通，最低也 0.46）→「迷糊时少说两句」也**永不触发**；
 *   ③ 舒适与唤醒**高度相关** —— 而且 2026-10-07 把探针的相关算法修对之后（原来"各自排序再算"
 *      算的是分位曲线相似度，不是配对相关），真数是 **r=−0.94**；更狠的是**舒适×不适 r=−0.99**
 *      （它们本来就是"1 减"的关系）→ 四个状态量**其实只有一个自由度**。
 * 所以：把疲劳区间按实测下移到 p90 附近；把永不触发的"唤醒低"换成**真正会低**的量（不适高）；
 * 给舒适补一条对称的"舒服时敢想远一点"；再加两个**派生**维度（好奇/亲近，见下面那节）。 */
/* ============================ 两个**派生**维度（不新增存档字段） ============================
 *
 * 局限登记（§四.A.5）：「驱动耦合表共线」。2026-10-07 把探针算相关的方式修对之后
 * （原来"各自排序再算"——那算的是分位曲线的相似度，不是配对相关），真矩阵是：
 *     舒适×不适 **r=−0.99**、舒适×唤醒 **−0.94**、不适×唤醒 +0.95、舒适×疲劳 −0.82 …
 * 也就是说：**四个状态量其实只有一个自由度**（都是"我最近猜得准不准"的函数）。
 * 所以这一轮加的是**从别的来源派生**的维度 —— 各自有各自的信号，谁都不看"猜得准不准"：
 *     `好奇` = 图里的**缺口感**（薄 token 占比：边数 < 3 的词有多少）—— 来自**图**；
 *     `亲近` = 他者模型的**信任 × 互动**归一 —— 来自**社交**（没人 = 0）。
 * 两个都是**现算**（不写 AI、不写存档），所以重载后同一个问题给同一个数（确定性）。
 * ⚠ 只有"共线被量掉"才算数：装完要看 `tools/driveprobe.c` 的相关矩阵，|r| 得低。 */
static inline float ai_drive_curiosity(const AI *ai) {
    /* ⚠ 第一版算的是**整个词池**的薄词占比 —— 量出来在两种场景里都几乎是常数
     * （语料回放 0.19~0.20、对话 0.19 恒定）：池子太大，一轮动不了它。
     * 改成只看**当前注意环**（这一轮谈到的那些词）：「这轮说的词里有多少是我的缺口」，
     * 才会逐轮变化。（那一版的量测留在 §6.46 里，别重复踩。） */
    if (!ai || !ai->focus_ring || ai->focus_n == 0) return 0.0f;
    size_t thin = 0, tot = 0;
    for (size_t i = 0; i < ai->focus_n; ++i) {
        const Token *t = ai->focus_ring[i];
        if (!t || t->is_garbage) continue;
        if (t->name && t->name[0] == '[') continue;      /* 内核标记不算 */
        tot++;
        if (t->count < 3) thin++;                        /* 薄：还没接上几条边 */
    }
    return tot ? (float)((double)thin / (double)tot) : 0.0f;
}
static inline float ai_drive_closeness(const AI *ai) {
    if (!ai || ai->others_n == 0) return 0.0f;
    double s = 0.0;
    for (size_t i = 0; i < ai->others_n; ++i) {
        const OtherModel *om = &ai->others[i];
        double w = (double)om->n_interactions;
        if (w > 20.0) w = 20.0;
        s += (double)om->trust * (1.0 + w / 20.0);       /* 信任 × 熟络度 */
    }
    double v = s / 3.0;                                  /* 三个"熟人"差不多就满了 */
    if (v > 1.0) v = 1.0;
    if (v < 0.0) v = 0.0;
    return (float)v;
}

static const DriveCoupling AI_DRIVE[] = {
    { "疲劳", "思考步数", 0.08f, 1.00f, -4, "这一轮工作周期快到头了 → 少想几步（睡一觉就回来）" },
    { "唤醒", "生成长度", 0.70f, 1.00f, +2, "警醒时多说两句" },
    { "不适", "生成长度", 0.90f, 1.00f, -1, "完全没底时少说两句" },
    { "舒适", "激活阈值", 0.00f, 0.35f, +3, "不舒服时保守一点（阈值抬高 0.03）" },
    { "舒适", "激活阈值", 0.55f, 1.00f, -2, "舒服时敢想远一点（阈值降 0.02）" },
    /* 下面两行用的是**派生维度**（上面那节）：区间同样按实测分布取（p95 附近），
     * 而且它们在**一阶差分**上跟旧四个量几乎不相关（|Δr| ≤ 0.19）—— 是真·另一个维度。 */
    { "好奇", "思考步数", 0.20f, 1.00f, +2, "这轮谈到的词里缺口不少（注意环薄词 ≥p90）→ 多想两步去接上" },
    { "亲近", "激活阈值", 0.30f, 1.00f, -1, "跟熟人（≥2 个有来往的）说话放松一点（阈值降 0.01）" },
    { NULL, NULL, 0, 0, 0, NULL }
};
#define AI_DRIVE_N ((sizeof AI_DRIVE / sizeof AI_DRIVE[0]) - 1)

/* 取某个驱动量的当前值（0..1）—— 读的是 AI 里的**普通数字**（名字的来历见文件头） */
static inline float ai_drive_value(const AI *ai, const char *drive) {
    if (!ai || !drive) return 0.0f;
    if (!strcmp(drive, "舒适")) return ai->inner.comfort;
    if (!strcmp(drive, "唤醒")) return ai->inner.arousal;
    if (!strcmp(drive, "疲劳")) return ai->inner.fatigue;
    if (!strcmp(drive, "不适")) return ai->inner.discomfort;   /* 惊讶水平的低通 */
    if (!strcmp(drive, "好奇")) return ai_drive_curiosity(ai);  /* 派生：图里的缺口感 */
    if (!strcmp(drive, "亲近")) return ai_drive_closeness(ai);  /* 派生：社交（信任×互动） */
    return 0.0f;
}

/* 每轮调一次：把驱动量折算成**三个数字**交给内核（层关着时三个都是 0）。 */
static inline void drive_policy_step(AI *ai) {
    if (!ai) return;
    ai->drive_d_steps = 0; ai->drive_d_gen = 0; ai->drive_d_cutoff = 0.0f;
    if (!ai->drive_on) return;
    for (size_t i = 0; i < AI_DRIVE_N; ++i) {
        const DriveCoupling *c = &AI_DRIVE[i];
        float v = ai_drive_value(ai, c->drive);
        if (v < c->lo || v > c->hi) continue;
        if (!strcmp(c->param, "思考步数"))      ai->drive_d_steps += c->delta;
        else if (!strcmp(c->param, "生成长度")) ai->drive_d_gen += c->delta;
        else if (!strcmp(c->param, "激活阈值")) ai->drive_d_cutoff += (float)c->delta / 100.0f;
    }
}

#endif /* DRIVE_H */
