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
    g_ag->brain = ai;
    /* ⚠ 这里**不**镜像 `[lead]`：空脑子（A~F 组）不该被塞进外壳标记词（实测会让 12 项失败）。
     * 需要引导语的组（G 组，真模型）在加载之后自己调一次 `aip_shell_mirror_lead()`。 */
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
        ok(strstr(r, "改 掉 旧 的 吗") != NULL || strstr(r, "要 撤") != NULL,
           "A 回复里**问了一句**要不要改（否则用户不知道还能撤）");
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

    /* ---------- C. 追问式改口：问一句「要改吗」，回「对」就撤、回「不」就留着 ---------- */
    {
        AI *ai = ai_new();
        g_ag->brain = ai;
        say(ai, "狗 属于 动物");
        const char *r = say(ai, "狗 属于 植物");
        ok(strstr(r, "要 我 改 掉 旧 的 吗") != NULL,
           "C 学到冲突时**问了一句**「要 我 改 掉 旧 的 吗」（不是只报错）");
        ok(agent_retract_pending(ai), "C 待确认状态挂上了（下一轮短回话会被接住）");
        const char *r2 = say(ai, "对");
        ok(strstr(r2, "改 过来 了") != NULL && strstr(r2, "动物") != NULL,
           "C 回「对」⇒ 撤掉旧值，并说清撤的是哪条");
        ok(!knows(ai, "狗", "属于", "动物", 0) && knows(ai, "狗", "属于", "植物", 0),
           "C 撤之后：旧值假、新值真");
        ok(!agent_retract_pending(ai), "C 用过的待确认状态清掉了");

        /* 回「不」：两条都留着 */
        AI *b = ai_new();
        g_ag->brain = b;
        say(b, "猫 属于 动物");
        say(b, "猫 属于 植物");
        const char *r3 = say(b, "不");
        ok(strstr(r3, "两 条 都 留") != NULL, "C 回「不」⇒ 明说两条都留着");
        ok(knows(b, "猫", "属于", "动物", 0) && knows(b, "猫", "属于", "植物", 0),
           "C 回「不」之后两条都还在");

        /* 说别的：不当作回答，待确认状态作废，且**不改知识** */
        AI *c = ai_new();
        g_ag->brain = c;
        say(c, "鸟 属于 动物");
        say(c, "鸟 属于 植物");
        const char *r4 = say(c, "今天 天气 怎么样");
        ok(strstr(r4, "两 条 都 留") == NULL && strstr(r4, "改 过来 了") == NULL,
           "C 说别的：不把别的话当成「回答」");
        ok(!agent_retract_pending(c), "C 说别的之后待确认状态作废");
        ok(knows(c, "鸟", "属于", "动物", 0) && knows(c, "鸟", "属于", "植物", 0),
           "C 说别的之后两条都还在（没偷偷改知识）");
        ok(ai_check_ok(c), "C 全程不变式干净");
        ai_free(ai); ai_free(b); ai_free(c);
        g_ag->brain = NULL;
    }

    /* ---------- D. 更正句：「不 对 ，猫 属于 猫科」/「错了，…」 ---------- */
    {
        AI *ai = ai_new();
        g_ag->brain = ai;
        say(ai, "猫 属于 动物");
        const char *r = say(ai, "不 对 ，猫 属于 猫科");
        ok(strstr(r, "改 过来 了") != NULL && strstr(r, "动物") != NULL,
           "D 「不 对 ，X」⇒ 学 X 并**撤掉**被否认的旧值，说清撤的是哪条");
        ok(knows(ai, "猫", "属于", "猫科", 0) && !knows(ai, "猫", "属于", "动物", 0),
           "D 更正之后：新的真、旧的假");
        ok(!agent_retract_pending(ai), "D 更正模式不留待确认（已经撤了）");

        /* 不带逗号、带「错 了」前缀 */
        say(ai, "狗 属于 动物");
        say(ai, "不 对 狗 属于 犬科");
        ok(knows(ai, "狗", "属于", "犬科", 0) && !knows(ai, "狗", "属于", "动物", 0),
           "D 「不 对 X」（无逗号）也认得出是更正");
        const char *r3 = say(ai, "错 了 ，猫 属于 猫科");
        ok(strstr(r3, "我 知道") != NULL || strstr(r3, "改 过来 了") != NULL,
           "D 「错 了 ，…」不炸（重复教同一条 ⇒ 回「我 知道」）");

        /* 不是更正句的说法不能被剥：`不是 什么` 仍按问句走 */
        const char *r4 = say(ai, "猫 属于 什么");
        ok(strstr(r4, "我 记 的 是") != NULL || strstr(r4, "动物") != NULL ||
           strstr(r4, "猫科") != NULL,
           "D 「猫 属于 什么」照旧答（前缀剥离不误伤疑问句）");
        ok(ai_check_ok(ai), "D 全程不变式干净");
        ai_free(ai);
        g_ag->brain = NULL;
    }

    /* ---------- E. 「你 怎么 知道」：出处（教的是教、语料是语料、推的是推、没有就说没有） ---------- */
    {
        AI *ai = ai_new();
        g_ag->brain = ai;
        say(ai, "麒麟 属于 神兽");
        const char *r = say(ai, "你 怎么 知道 麒麟 属于 神兽");
        ok(strstr(r, "你 教 我 的") != NULL, "E 教过的 ⇒ 「这 条 是 你 教 我 的」");
        const char *r2 = say(ai, "你 怎么 知道 麒麟 属于 动物");
        ok(strstr(r2, "我 不 知道") != NULL || strstr(r2, "没 有 这 条") != NULL,
           "E 没教过、也推不出来 ⇒ 老实说不知道");
        say(ai, "神兽 属于 动物");
        const char *r3 = say(ai, "你 怎么 知道 麒麟 属于 动物");
        ok(strstr(r3, "推 出来 的") != NULL && strstr(r3, "神兽") != NULL,
           "E 推得出来的 ⇒ 「推 出来 的 ： 链」");
        const char *r4 = say(ai, "谁 说 的 恐龙 属于 鸟");
        ok(r4[0] != 0, "E 「谁 说 的 …」也有答复（不炸）");
        ok(ai_check_ok(ai), "E 全程不变式干净");
        ai_free(ai);
        g_ag->brain = NULL;
    }

    /* ---------- G. 一次**完整的交互学习会话**（顺序执行，像真人一样一路教下来） ----------
     * 这一组**必须用真模型**：教学意图（`我 教 你 …`）走的是数据层 `[lead]`，
     * `ai_new()` 不加载 patterns.txt ⇒ 那些说法不会被剥（A~D 组不依赖数据，所以能用空脑子）。 */
    {
        AI *ai = ai_load_file(model);
        if (!ai) { printf("读不到 %s\n", model); return 1; }
        /* ⚠ `[lead]` 引导语要**镜像成图上的外壳词**才生效（main.c 换模型时也调这一处）。 */
        aip_shell_mirror_lead(ai);
        g_ag->brain = ai;
        const char *r1 = say(ai, "我 教 你 麒麟 属于 神兽");
        ok(strstr(r1, "记下 了") != NULL, "G1 「我 教 你 …」⇒ 学下来");
        ok(strstr(say(ai, "麒麟 属于 神兽 吗"), "对 ，") != NULL, "G2 立刻问得出来");
        ok(strstr(say(ai, "你 怎么 知道 麒麟 属于 神兽"), "你 教 我 的") != NULL,
           "G3 问出处 ⇒ 「你 教 我 的」");
        say(ai, "神兽 属于 动物");
        ok(strstr(say(ai, "麒麟 属于 动物 吗"), "对 ，") != NULL, "G4 继承：麒麟 属于 动物 也对");
        say(ai, "麒麟 有 三 个 角");
        ok(strstr(say(ai, "麒麟 有 角 吗"), "对 ，") != NULL, "G5 数量蕴含：有 角 对");
        ok(strstr(say(ai, "麒麟 有 几 个 角"), "三 个 角") != NULL, "G6 问数目 ⇒ 三 个 角");
        say(ai, "狗 属于 动物");
        const char *rc = say(ai, "狗 属于 植物");
        ok(strstr(rc, "冲 突") != NULL || strstr(rc, "冲突") != NULL, "G7 冲突当场说清");
        ok(agent_retract_pending(ai), "G8 挂上待确认（可以一个字改口）");
        ok(strstr(say(ai, "对"), "撤 掉 了") != NULL, "G9 回「对」⇒ 撤掉旧值");
        ok(strstr(say(ai, "狗 属于 动物 吗"), "不 对 ，") != NULL, "G10 撤完之后旧值答「不 对」");
        say(ai, "不 对 ， 狗 属于 犬科");
        ok(knows(ai, "狗", "属于", "犬科", 0), "G11 「不 对 ，X」⇒ 学到 X");
        const char *r12 = say(ai, "我刚才教了你什么");
        ok(strstr(r12, "教 了") != NULL && strstr(r12, "麒麟") != NULL,
           "G12 「我刚才教了你什么」列得出来");
        /* 存读：教的东西要还在 */
        size_t sz = ai_save_file(ai, "_session.bin");
        AI *back = sz ? ai_load_file("_session.bin") : NULL;
        ok(back != NULL, "G13 会话可以存盘再读回");
        if (back) {
            ok(knows(back, "麒麟", "属于", "神兽", 0) && knows(back, "麒麟", "属于", "动物", 0) &&
               knows(back, "麒麟", "有", "三个角", 0),
               "G14 读回来：会话里教的事实都在");
            ai_free(back);
        }
        remove("_session.bin");
        ok(ai_check_ok(ai), "G15 整个会话之后不变式干净");
        ai_free(ai);
        g_ag->brain = NULL;
    }

    /* ---------- H. 「你教我的」要**跨会话**认（教学留痕随图存盘） ---------- */
    {
        AI *ai = ai_load_file(model);
        if (!ai) { printf("读不到 %s\n", model); return 1; }
        aip_shell_mirror_lead(ai);   /* G 组的理由同款：引导语要落成外壳词 */
        g_ag->brain = ai;
        say(ai, "麒麟 属于 神兽");
        ok(strstr(say(ai, "你 怎么 知道 麒麟 属于 神兽"), "你 教 我 的") != NULL,
           "H1 刚教完 ⇒ 出处是「你教我的」");
        ok(pool_find(ai->pool, "[教:麒麟_属于_神兽]") != NULL,
           "H2 图上留了教学痕迹 `[教:麒麟_属于_神兽]`（普通标记节点，随图存盘）");
        size_t sz = ai_save_file(ai, "_prov.bin");
        AI *back = sz ? ai_load_file("_prov.bin") : NULL;
        ok(back != NULL, "H3 存盘再读回");
        if (back) {
            /* 关键：**新脑子**里没有本会话的修改账 ⇒ 靠留痕认 */
            ok(pool_find(back->pool, "[教:麒麟_属于_神兽]") != NULL, "H4 读回来留痕还在");
            g_ag->brain = back;
            aip_shell_mirror_lead(back);
            ok(strstr(say(back, "你 怎么 知道 麒麟 属于 神兽"), "你 教 我 的") != NULL,
               "H5 重开之后仍答「你教我的」（**跨会话出处**）");
            ok(strstr(say(back, "你 怎么 知道 猫 属于 动物"), "不 是 你 教 的") != NULL,
               "H6 语料里的事实不冒充「你教我的」");
            ai_free(back);
        }
        remove("_prov.bin");
        ok(ai_check_ok(ai), "H7 全程不变式干净");
        ai_free(ai);
        g_ag->brain = NULL;
    }

    free(g_ag);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
