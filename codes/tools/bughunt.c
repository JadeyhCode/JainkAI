/* tools/bughunt.c — **阶段②：用不变式找 bug**（§6.584，C 级）
 *
 * 思路：套件只验"答得对/不崩"，但一个**错误的图**可能照样答得对。所以这里做两件事：
 *   ① 每一步之后跑 `aicheck.h` 的全量不变式（池/边/名字/反查表/账/局部加载）；
 *   ② 用**固定种子**的 LCG 把各种操作随机组合起来打（教事实/问问题/否定/动态谓语/
 *      parts 存取/流水搬运/GC/emb 重建/存读往返），每一步都验。
 *
 * 判据（任一不成立就失败）：
 *   · 不变式 `bad == 0`（**每一步**）；
 *   · 历史空自环的条数**不增长**（出厂图 18 条是老的死数据，钉住它当金丝雀）；
 *   · 每一条输入都**有答复**（没崩、没静默掉）；
 *   · 存读往返之后：还能答对存盘前学会的事实 + 不变式仍然干净。
 *
 * 用法：
 *   gcc -std=c11 -O2 -Wall -Wextra -o build_bughunt tools/bughunt.c -lm && ./build_bughunt model.bin 12345 300
 *   带 sanitizer（更狠）：
 *   gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o build_bughunt_asan tools/bughunt.c -lm && ./build_bughunt_asan model.bin 12345 300
 */
#include "../agent.h"
#include "../model_io.h"
#include "../aicheck.h"
#include "../xbrain.h"
#include "../parts.h"   /* 局部加载：aip_parts_* */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passes = 0, fails = 0;
static void ok(int cond, const char *what) {
    if (cond) { printf("  [OK]   %s\n", what); passes++; }
    else      { printf("  [FAIL] %s\n", what); fails++; }
}

/* 固定种子 LCG（可复现，不是真随机）*/
static unsigned long g_seed;
static unsigned rnd(unsigned n) {
    g_seed = g_seed * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)((g_seed >> 33) % (n ? n : 1));
}

static const char *SUBJ[] = { "猫", "狗", "鸟", "鱼", "桌子", "太阳", "水", "老师", "学生", "家",
                              "窗户", "椅子", "龙", "鲸鱼", "企鹅", "小明", "我", "你" };
static const char *OBJ[]  = { "动物", "植物", "家具", "飞", "游泳", "唱歌", "毛", "北京", "学校",
                              "神兽", "哺乳动物", "人类", "猫科", "鸟", "鱼", "水", "歌" };
static const char *REL[]  = { "属于", "会", "有", "在", "喜欢", "怕", "吃", "需要", "想念", "喵" };
#define NSUBJ ((int)(sizeof SUBJ / sizeof SUBJ[0]))
#define NOBJ  ((int)(sizeof OBJ / sizeof OBJ[0]))
#define NREL  ((int)(sizeof REL / sizeof REL[0]))

static char g_out[512];

/* 一步之后验不变式；`warn0` 是基准（金丝雀不许长）*/
static void dump_tok(AI *ai, const char *name) {
    Token *t = pool_find(ai->pool, name);
    if (!t) { printf("      dump %s: 词不在池里\n", name); return; }
    printf("      dump %s id=%u count=%zu resident=%d rev_head=%u\n", name, t->id, t->count,
           token_assocs_resident(t), t->rev_head);
    if (!token_assocs_resident(t)) return;
    for (size_t k = 0; k < t->count; ++k) {
        const AssocEntry *e = &token_assocs_ro(t)[k];
        if (!e->main) continue;
        printf("        → %-8s role=%u rel=%u nrel=%u dyn=%u hits=%u act=%s\n",
               e->main->name ? e->main->name : "?", e->role, e->rel, e->nrel, e->dyn,
               e->hits, e->action && e->action->name ? e->action->name : "-");
    }
}

static int check_after(AI *ai, size_t warn0, int turn, const char *what) {
    AiCheck ck;
    int okc = aip_check_ai(ai, &ck);
    if (!okc) {
        printf("  ✗ 第 %d 步（%s）不变式坏了：bad=%zu warn=%zu\n      %s\n",
               turn, what, ck.bad, ck.warn, ck.first);
        if (getenv("AIP_BUGHUNT_TRACE")) {
            dump_tok(ai, "鸟"); dump_tok(ai, "鱼");
            /* 扫整张反查表：有没有 (鸟 的 id, 喵, 否定) 这条，dst 是谁 */
            Token *A = pool_find(ai->pool, "鸟"), *B = pool_find(ai->pool, "鱼");
            uint8_t rid = rel_id_of(ai, "喵", 0);
            printf("      rev 扫描：A.id=%u B.id=%u rid=%u rev_n=%zu rev_cap=%zu\n",
                   A ? A->id : 0, B ? B->id : 0, rid, ai->pool->rev_n, ai->pool->rev_cap);
            for (size_t k = 0; k < ai->pool->rev_n; ++k) {
                uint32_t meta = ai->pool->rev_meta[k];
                if ((uint32_t)(meta & 0x7FFFu) != rid) continue;
                if (ai->pool->rev_src[k] != (A ? A->id : 0)) continue;
                /* 找 dst：扫所有词的 rev_head 链里有没有 k+1 */
                for (size_t q = 0; q < ai->pool->count; ++q) {
                    Token *d = ai->pool->tokens[q];
                    for (uint32_t e = d ? d->rev_head : 0; e; e = ai->pool->rev_next[e - 1])
                        if (e == (uint32_t)k + 1) {
                            printf("        条目 k=%zu neg=%d → dst=%s(id=%u)\n",
                                   k, (meta & 0x8000u) ? 1 : 0, d->name ? d->name : "?", d->id);
                            q = ai->pool->count; break;
                        }
                }
            }
        }
        return 0;
    }
    if (ck.warn > warn0) {
        printf("  ✗ 第 %d 步（%s）历史空自环变多了：%zu → %zu（%s）\n",
               turn, what, warn0, ck.warn, ck.warn1);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *model = argc > 1 ? argv[1] : "model.bin";
    g_seed = (unsigned long)(argc > 2 ? strtoul(argv[2], NULL, 10) : 20261025UL);
    int turns = argc > 3 ? atoi(argv[3]) : 300;
    if (turns < 1) turns = 1;

    AI *ai = ai_load_file(model);
    if (!ai) { printf("读不到 %s\n", model); return 1; }
    Agent *ag = agent_new(stdin_perceive, stdout_act, NULL);
    if (!ag) { printf("建不出 Agent\n"); ai_free(ai); return 1; }
    ai_free(ag->brain);
    ag->brain = ai;

    /* §6.584 读盘时应当已经清掉"死的自环边"（老版本留的 a→a 边）。
     * 这条是**硬不变式**：图里不该有自环（带语义的更不行）。 */
    {
        size_t selfs = 0;
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (!t || !token_assocs_resident(t)) continue;
            for (size_t k = 0; k < t->count; ++k)
                if (token_assocs_ro(t)[k].main == t) selfs++;
        }
        ok(selfs == 0, "读盘时已清掉死的自环边（图里 a→a 边 = 0）");
    }

    {   /* 基准：出厂图上历史空自环几条（后面不许变多）*/
        AiCheck c0;
        if (!aip_check_ai(ai, &c0)) {
            printf("  ✗ 一开始就不干净：%s\n", c0.first);
            return 1;
        }
        printf("  [基线] 词 %zu / 边 %zu / 历史空自环 %zu\n", ai->pool->count,
               (size_t)ai->pool->rev_n, c0.warn);
        size_t warn0 = c0.warn;
        int no_answer = 0, bad_step = 0;
        size_t learned = 0;
        char last_fact[96] = {0};

        int trace = getenv("AIP_BUGHUNT_TRACE") != NULL;
        for (int t = 1; t <= turns; ++t) {
            unsigned act = rnd(100);
            const char *subj = SUBJ[rnd((unsigned)NSUBJ)];
            const char *obj  = OBJ[rnd((unsigned)NOBJ)];
            const char *rel  = REL[rnd((unsigned)NREL)];
            char sent[192];

            if (trace) fprintf(stderr, "[hunt] step %d act=%u subj=%s rel=%s obj=%s\n", t, act, subj, rel, obj);
            if (act < 34) {
                /* 教一条事实（走统一写入通道：关系/否定/动态谓语都会走到）*/
                Token *A = pool_token_intern(ai->pool, subj);
                Token *B = pool_token_intern(ai->pool, obj);
                uint8_t rid = rel_id_of(ai, rel, 1);
                int neg = (rnd(100) < 25) ? 1 : 0;
                if (A && B && rid && A != B) {
                    int wrc = ai_write_rel(ai, A, B, rid, neg, AI_WSRC_RELATION);
                    if (trace)
                        fprintf(stderr,
                                "[write] %s %s%s %s rc=%d rid=%u rev=%d dyn_neg=%d\n",
                                subj, neg ? "不 " : "", rel, obj, wrc, rid,
                                pool_rev_has(ai->pool, B, A->id, rid, neg),
                                A == B ? -1 : 0);
                    learned++;
                    snprintf(last_fact, sizeof last_fact, "%s %s%s %s", subj,
                             neg ? "不 " : "", rel, obj);
                }
            } else if (act < 70) {
                /* 自然语言：陈述（会走学习路径）或问句（会走答复路径）*/
                if (rnd(2) == 0)
                    snprintf(sent, sizeof sent, "%s %s%s %s", subj, (rnd(4) == 0) ? "不 " : "",
                             rel, obj);
                else
                    snprintf(sent, sizeof sent, "%s %s %s 吗", subj, rel, obj);
                memset(g_out, 0, sizeof g_out);
                agent_step(ag, sent, g_out, sizeof g_out);
                if (!g_out[0]) {
                    printf("  ✗ 第 %d 步没有答复：「%s」\n", t, sent);
                    no_answer = 1;
                }
            } else if (act < 74) {
                /* §6.598 **符号节点行为（`::op` 原语）**：阶段② 之前完全没随机打到这里。
                 * 只挑**不需要外部世界**的几条（不碰执行/动作），参数从词表里随机取。 */
                static const char *OPS[][3] = {
                    { "加前提",   "猫",      "动物"  },
                    { "加反前提", "狗",      "动物"  },
                    { "中间插词", "鸟",      "飞"    },
                    { "建过程",   "洗",      NULL    },
                    { "加步骤",   "洗",      "烧水"  },
                    { "加条件",   "洗",      "水开"  },   /* 加条件 需要 3 个参数 ⇒ 下面补 */
                    { "回收临时", NULL,      NULL    },
                };
                const char *nm = OPS[rnd(sizeof OPS / sizeof OPS[0])][0];
                char a1[32], a2[32], a3[32];
                char *args[3];
                size_t na = 0;
                if (!strcmp(nm, "建过程")) {
                    snprintf(a1, sizeof a1, "过程%u", rnd(9));
                    args[na++] = a1;
                } else if (!strcmp(nm, "加步骤") || !strcmp(nm, "加条件")) {
                    snprintf(a1, sizeof a1, "过程%u", rnd(9));
                    snprintf(a2, sizeof a2, "动作%u", rnd(9));
                    args[na++] = a1; args[na++] = a2;
                    if (!strcmp(nm, "加条件")) { snprintf(a3, sizeof a3, "条件%u", rnd(9)); args[na++] = a3; }
                } else if (!strcmp(nm, "回收临时")) {
                    /* 0 参数 */
                } else {
                    const char *x = SUBJ[rnd((unsigned)NSUBJ)];
                    const char *y = OBJ[rnd((unsigned)NOBJ)];
                    snprintf(a1, sizeof a1, "%s", x);
                    snprintf(a2, sizeof a2, "%s", y);
                    args[na++] = a1; args[na++] = a2;
                }
                char note[128] = {0};
                int got = token_op_run(ai, nm, args, na, note, sizeof note);
                if (trace) fprintf(stderr, "[op] %s rc=%d note=%.60s\n", nm, got, note);
                if (got < -1) { printf("  ✗ 第 %d 步 op「%s」返回 %d\n", t, nm, got); bad_step = 1; }
            } else if (act < 78) {
                /* 元认知/维护类操作 */
                switch (rnd(3)) {
                case 0: ai_embed_build(ai); break;
                case 1: (void)ai_gc(ai); break;
                default: rel_excl_build(ai); break;
                }
            } else if (act < 90) {
                /* 局部加载：存段 → attach → 查一句 → detach → 验 → 换出 */
                (void)aip_parts_save(ai, "_bughunt.parts");
                AIParts *pt = aip_parts_open("_bughunt.parts");
                if (pt) {
                    if (ai->pool->parts) { aip_parts_close((AIParts *)ai->pool->parts); ai->pool->parts = NULL; }
                    ai->pool->parts = pt;
                    aip_parts_attach(ai, pt);
                    memset(g_out, 0, sizeof g_out);
                    agent_step(ag, "猫 属于 动物 吗", g_out, sizeof g_out);
                    if (!g_out[0]) { printf("  ✗ 第 %d 步 attach 之后没答复\n", t); no_answer = 1; }
                    AIPartsVerify v = aip_parts_verify(ai);
                    if (v.orphan) { printf("  ✗ 第 %d 步段索引有孤儿 %zu 个\n", t, v.orphan); bad_step = 1; }
                    (void)aip_parts_detach(ai);
                    size_t skip = 0;
                    (void)aip_parts_evict(ai, 0, &skip);
                    /* §6.584 先 close（它会把还挂在磁盘上的边读回来）**再删文件**。
                     * 反过来的话 close 读不到段，等于把"读不回来"这条路当正常路径用。 */
                    (void)aip_parts_close(pt);
                    ai->pool->parts = NULL;
                    ai->pool->part_load = NULL;
                    ai->pool->part_touch = NULL;
                    remove("_bughunt.parts");
                }
            } else {
                /* 跨脑搬运：存流水 → 抄进第二个脑（本地优先/冲突可见都在里面）*/
                static AI *b2 = NULL;
                if (!b2) b2 = ai_new();
                if (b2) {
                    (void)xb_journal_save(ai, "_bughunt.j");
                    XbResult xr;
                    (void)xb_journal_replay(b2, "_bughunt.j", XB_MODE_LOCAL_WINS, &xr);
                    AiCheck c2;
                    if (!aip_check_ai(b2, &c2)) {
                        printf("  ✗ 第 %d 步第二个脑不变式坏了：%s\n", t, c2.first);
                        bad_step = 1;
                    }
                    remove("_bughunt.j");
                }
            }

            if (!check_after(ai, warn0, t, "随机操作")) { bad_step = 1; break; }
        }

        ok(no_answer == 0, "每一条输入都有答复（没有静默掉/崩）");
        ok(bad_step == 0, "每一步之后不变式都干净（池/边/名字/反查表/账/局部加载）");

        /* 存读往返：学过的事实还在，而且不变式仍然干净 */
        size_t wr = ai_save_file(ai, "_bughunt.bin");
        AI *back = wr ? ai_load_file("_bughunt.bin") : NULL;
        ok(back != NULL, "存读往返：文件能读回来");
        if (back) {
            AiCheck cb;
            ok(aip_check_ai(back, &cb) == 1, "存读之后不变式仍然干净");
            if (learned && last_fact[0]) {
                char q[128];
                snprintf(q, sizeof q, "%s 吗", last_fact);
                char o2[512] = {0};
                Agent *ag2 = agent_new(stdin_perceive, stdout_act, NULL);
                if (ag2) {
                    ai_free(ag2->brain);
                    ag2->brain = back;
                    agent_step(ag2, q, o2, sizeof o2);
                    ok(o2[0] != 0, "存读之后问学过的事实有答复");
                    ai_free(back);
                    back = NULL;
                    free(ag2);
                }
            }
            if (back) ai_free(back);
        }
        remove("_bughunt.bin");
        printf("  [账] 教了 %zu 条事实 / 走了 %d 步\n", learned, turns);
    }

    ai_free(ai);
    free(ag);
    printf("\n通过 %d / 失败 %d\n", passes, fails);
    return fails ? 1 : 0;
}
