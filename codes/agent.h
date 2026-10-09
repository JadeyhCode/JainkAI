/* agent.h — 感知-行动循环（含增量睡眠 + 空闲调度）
 * 依赖：ai.h
 */
#ifndef AGENT_H
#define AGENT_H

#include "ai.h"
#include "drive.h"
#include "action.h"      /* 受审的外部执行 + 写文件（层；默认关） */
#include "code.h"        /* 写-跑-看错-改 的机械闭环（降级版；默认关） */
#include "frame_io.h"

typedef const char *(*AgentPerceiveFn)(void *user);
typedef void        (*AgentActFn)(const char *output, void *user);

typedef struct {
    AI              *brain;
    AgentPerceiveFn  perceive;
    AgentActFn       act;
    void            *user;
    size_t           cycle;
    size_t           n_learned_lines;
    size_t           n_spoke;
    int              verbose;
} Agent;

/* ============================ 关系层（L2）的两步 ============================
 *
 * 都写成独立函数而不是塞在 agent_step 里：之前内联版本用了 goto 绕，
 * 结果思维链的顺序都乱了（推理排在联想前面）。独立函数还有一个好处 ——
 * **解析器绝不碰图**这条护栏能写在一处、看得见。
 */

/* **修正的记忆 / 来源监控**：改口时留一个标记节点的名字，如 `[修正:猫_不_有_毛]`。
 * 名字要按**字符边界**截断（截半个汉字会让 ::revise 打出乱码 —— 这个项目踩过）。 */
static inline void agent_revise_name(char *dst, size_t cap, const char *a, int neg,
                                     const char *rel, const char *b) {
    char full[256];
    snprintf(full, sizeof full, MICRO_REVISE_TOKEN_PREFIX "%s_%s%s_%s]",
             a ? a : "?", neg ? "不_" : "", rel ? rel : "?", b ? b : "?");
    think_utf8_clip(dst, cap, full);
}

/* 步骤一：用户**陈述**了一条关系事实吗？是就当场学进图里（并写进模型）。
 * 支持肯定（`猫 属于 动物`）和否定（`猫 不 会 飞`）两种陈述。
 * 返回要说的确认话（调用方 free），不是陈述就返回 NULL。 */
static inline char *agent_learn_relation(AI *ai, char *const *words, size_t n,
                                        float *out_conf) {
    if (!ai || ai->rel_off || !words || n < 3 || n > 6) return NULL;
    /* 「你 是 谁」也是三个词、也用 是 —— 但它显然不是事实陈述。
     * 先挡疑问词：只要句子里有疑问词，一律不当陈述。 */
    /* 疑问词表只有一张（ai.h 的 AI_QUESTION_WORDS）——
     * 以前这里抄了一份、检索和拒答各抄一份，三份还各不相同。 */
    if (ai_has_question_word(words, n)) return NULL;

    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    /* 登记新谓语是**有副作用**的，所以先只验形状（不碰关系表），
     * 形状对了才允许 rel_parse 去登记 —— 否则「你 觉得 天气 怎么样」
     * 会把「觉得」污染进动态关系表，还写进模型。 */
    if (!rel_shape_ok(words, n)) return NULL;
    if (!rel_parse(ai, words, n, jbuf, sizeof jbuf, &aw, &rid, &is_neg,
                   &bw, &is_q, &rw, 1)) return NULL;
    if (is_q) return NULL;
    Token *A = pool_token_intern(ai->pool, aw);
    Token *B = pool_token_intern(ai->pool, bw);
    if (!A || !B || A == B) return NULL;

    /* 动态关系（清单外的谓语）：只支持肯定 */
    char resp0[192];
    if (rid >= REL_DYN_BASE) {          /* 边界是 REL_DYN_BASE：核心关系现在有 9 个 */
        if (is_neg) return NULL;
        ai_write_count(ai, AI_WSRC_RELATION);       /* 记录：这条是我现场学的 */
        if (token_tag_dyn(A, B, (uint8_t)(rid - REL_DYN_BASE + 1), REL_BOOST) != 0)
            return NULL;
        snprintf(resp0, sizeof resp0, "记下 了 ： %s %s %s", aw, rw, bw);
        ai->n_rel_learned++;
        cot_push(ai, COT_LEARN, 1, 90, "现场学一条关系事实（新谓语）", resp0);
        ai->n_cot++;
        if (out_conf) *out_conf = 0.7f;
        char *r = (char *)malloc(strlen(resp0) + 1);
        if (r) memcpy(r, resp0, strlen(resp0) + 1);
        return r;
    }

    /* 否定事实的措辞取 AI_RELS 里声明的否定形式：
     * 「没有」比「不 有」像人话，而「不 会」本来就是声明里的写法。 */
    const RelDef *rdef = rel_by_id(rid);
    const char *rshow = rw;
    if (is_neg && rdef && rdef->neg) rshow = rdef->neg;
    int known = is_neg ? rel_direct_neg(A, rid, B) : rel_direct(A, rid, B);
    if (known) {
        /* 已经知道的：回一句「我知道」，别装没听见。不算新学，不涨计数。 */
        snprintf(resp0, sizeof resp0, "我 知道 ： %s %s %s", aw,
                 is_neg ? rshow : rw, bw);
        cot_push(ai, COT_LEARN, 1, 70, "这条关系我本来就知道", resp0);
        ai->n_cot++;
    } else {
        /* 统一写入通道：关系事实是**知识**，开关打开时改走受审通道（默认直写）。 */
        int rc = ai_write_rel(ai, A, B, rid, is_neg, AI_WSRC_RELATION);
        if (rc != 0) return NULL;
        /* **改口**：这一句把同一条边上对立极性的旧观测替掉了（token.h 里做的）。
         * 那是「用户改了主意」，不是「系统里有两份互相矛盾的依据」——
         * 回答要说清楚，否则用户不知道自己教的修正到底生效没有。 */
        if (g_obs_revised) {
            snprintf(resp0, sizeof resp0,
                     "改 过来 了 ： %s %s %s（你 先前 说 的 是 反 的）", aw,
                     is_neg ? rshow : rw, bw);
            ai->n_rel_learned++;
            ai->n_learned++;
            ai->n_revised++;
            cot_push(ai, COT_LEARN, 1, 85, "改口：后说的观测替换先说的（同一条边只留一个极性）",
                     resp0);
            ai->n_cot++;
            /* **修正的记忆 / 来源监控**：留一个 [修正:…] 标记节点（连回主语），
             * 这样「这件事被改口过」**记得住**，而旧观测不会冒充知识复活。
             * 走的是同一条受审通道（安全内核 + 预算 + 审计），并且**幂等**：
             * 同一件事改口多次只会有一个标记（MOD_ADD_TOKEN 同名 intern）。 */
            if (!micro_of(ai)->safe.mod_off) {
                ModIntent in;
                memset(&in, 0, sizeof in);
                in.type = MOD_ADD_TOKEN;
                in.target = A;
                agent_revise_name(in.name, sizeof in.name, aw, is_neg, rel_name_of(ai, rid), bw);
                snprintf(in.source, sizeof in.source, "来源:改口");
                (void)mod_propose(&micro_of(ai)->queue, &in);
            }
        } else {
            snprintf(resp0, sizeof resp0, "记下 了 ： %s %s %s", aw,
                     is_neg ? rshow : rw, bw);
            ai->n_rel_learned++;
            ai->n_learned++;
            cot_push(ai, COT_LEARN, 1, 90, "现场学一条关系事实", resp0);
            ai->n_cot++;
        }
    }
    if (out_conf) *out_conf = 0.7f;
    char *r = (char *)malloc(strlen(resp0) + 1);
    if (r) memcpy(r, resp0, strlen(resp0) + 1);
    return r;
}

/* 步骤一点五：**进入假设世界**（反事实）。
 *
 *   「如果 猫 会 飞」 / 「假设 猫 属于 鸟」 / 「要是 猫 不 会 飞」
 *
 * 做法：把这条事实放进 `ai->hypo` 覆盖层 —— **不碰图、不存盘**。
 * 之后的提问都在这个假设世界里推理，链上对应的那一步标成「假设」，
 * 答复也写明「在 假设 下」，不会说得像真的。
 *
 * 铁律：假设绝不 tag 进边里。用户只是随口「如果」，系统却从此以为真 ——
 * 那是最恶劣的一类 bug。
 * 返回要说的确认话（调用方 free），不是假设句就返回 NULL。 */
static inline char *agent_set_hypothesis(AI *ai, char *const *words, size_t n,
                                        float *out_conf) {
    if (!ai || ai->rel_off || !words || n == 0) return NULL;
    /* 先判「撤回」—— 它只有一两个词，放在长度门槛后面就永远轮不到（踩过）。 */
    static const char *drop0[] = {"算了", "不说 了", "不想 了", NULL};
    /* 切词器把「算了」切成 算+了（它不是一个词典词），所以还要认这个形状 ——
     * 光比对整串是不行的（实测「算了」根本匹配不上）。 */
    int want_drop = 0;
    for (size_t i = 0; drop0[i]; ++i)
        if (strcmp(words[0], drop0[i]) == 0) { want_drop = 1; break; }
    if (!want_drop && strcmp(words[0], "算") == 0 && n >= 2 &&
        (strcmp(words[1], "了") == 0 || strcmp(words[1], "啦") == 0))
        want_drop = 1;
    if (want_drop) {
        if (ai->hypo.depth == 0) return NULL;
        /* 「算了」弹掉**最深**那一层（最近设的那条），不是一次清空 ——
         * 这样多层嵌套才能一层层退出来。 */
        Token *who = hypo_cur_who(&ai->hypo);
        hypo_pop(&ai->hypo);
        char note[96];
        snprintf(note, sizeof note, "撤回一层%s（还剩 %zu 层）",
                 who && who->name ? "（他 以为 那层）" : "",
                 ai->hypo.depth);
        cot_push(ai, COT_HYP, 1, 80, "撤回一层假设", note);
        ai->n_cot++;
        char resp0[128];
        snprintf(resp0, sizeof resp0, "好 的%s", ai->hypo.depth ? "（还有 别的 假设）" : "");
        if (out_conf) *out_conf = 0.7f;
        char *r = (char *)malloc(strlen(resp0) + 1);
        if (r) memcpy(r, resp0, strlen(resp0) + 1);
        return r;
    }
    /* ---- 作用域回溯：撤掉**指定**那一层 ----
     * 「算了」只能从最深的往下弹；要抽掉中间那层（「如果 猫 属于 鸟」作废，
     * 但后压的「他 以为 猫 会 飞」还得留着），就得能指名道姓地撤。
     * 三种说法都认：
     *   撤回 第 2 个 假设          （按序号，从 1 起）
     *   撤回 关于 猫 的 假设       （按主语：哪层提到它就撤哪层，从深往浅找）
     *   撤回 猫 属于 鸟            （按内容）
     *   猫 属于 鸟 不算 了         （按内容，另一种语序 —— 最常用）
     * 找不到就如实说找不到，不能默默撤错一层。 */
    {
        static const char *rev[] = {"撤回", "撤掉", "撤销", "取消", "撤", NULL};
        int is_rev = 0;
        size_t f0 = 0;               /* 标记词占几个词位：按内容时是 0 */
        for (size_t i = 0; rev[i]; ++i)
            if (strcmp(words[0], rev[i]) == 0) { is_rev = 1; f0 = 1; break; }
        if (is_rev && strcmp(words[0], "撤") == 0) {
            /* 切词器把「撤回」切成 撤 + 回（和「算了」被切成 算 + 了 一个道理），
             * 光一个「撤」不算撤回标记。 */
            if (n >= 2 && (strcmp(words[1], "回") == 0 || strcmp(words[1], "掉") == 0 ||
                           strcmp(words[1], "销") == 0)) f0 = 2;
            else { is_rev = 0; f0 = 0; }
        }
        /* 「… 不算 了」的尾巴：同样可能被切成 不 / 算 / 了 */
        size_t tail_at = n;
        if (!is_rev) {
            for (size_t k = 0; k < n; ++k) {
                if (strcmp(words[k], "不算") == 0 || strcmp(words[k], "作废") == 0) {
                    tail_at = k; break;
                }
                if (k + 1 < n && strcmp(words[k], "不") == 0 &&
                    strcmp(words[k + 1], "算") == 0) { tail_at = k; break; }
            }
        }
        if (is_rev || tail_at < n) {
            size_t f1 = is_rev ? n : tail_at;
            int target = -1;
            const char *how = NULL;
            if (is_rev && f1 >= f0 + 2 && strcmp(words[f0], "第") == 0) {
                /* 中文/阿拉伯数字都认 */
                const char *d = words[f0 + 1];
                int v = -1;
                if (d[0] >= '1' && d[0] <= '9' && d[1] == 0) v = d[0] - '0';
                else if (strcmp(d, "一") == 0) v = 1;
                else if (strcmp(d, "二") == 0 || strcmp(d, "两") == 0) v = 2;
                else if (strcmp(d, "三") == 0) v = 3;
                else if (strcmp(d, "四") == 0) v = 4;
                if (v >= 1 && (size_t)v <= ai->hypo.depth) target = v - 1;
                how = "按序号";
            } else if (is_rev && f1 >= f0 + 2 &&
                       (strcmp(words[f0], "关于") == 0 ||
                        /* 「关于」也会被切成 关 / 于 */
                        (f1 >= f0 + 3 && strcmp(words[f0], "关") == 0 &&
                         strcmp(words[f0 + 1], "于") == 0))) {
                size_t si = (strcmp(words[f0], "关于") == 0) ? f0 + 1 : f0 + 2;
                Token *t = (si < f1) ? pool_find(ai->pool, words[si]) : NULL;
                if (t) target = hypo_layer_of_subject(&ai->hypo, t);
                how = "按主语";
            } else if (f1 > f0) {
                const char *aw = NULL, *bw = NULL, *rw = NULL;
                uint8_t rid = 0;
                int is_neg = 0, is_q = 0;
                char jbuf[64];
                if (rel_parse(ai, words + f0, f1 - f0, jbuf, sizeof jbuf,
                              &aw, &rid, &is_neg, &bw, &is_q, &rw, 0) && !is_q) {
                    Token *A = pool_find(ai->pool, aw);
                    Token *B = pool_find(ai->pool, bw);
                    if (A && B) target = hypo_layer_of_fact(&ai->hypo, A, rid, B);
                }
                how = "按内容";
            }
            char what[160];
            if (target >= 0) {
                hypo_frame_text(ai, (size_t)target, what, sizeof what);
                char note[200];
                snprintf(note, sizeof note, "第 %d 层：%s（还剩 %zu 层）",
                         target + 1, what, ai->hypo.depth - 1);
                hypo_drop(&ai->hypo, (size_t)target);
                cot_push(ai, COT_HYP, 1, 80, "撤回指定的一层假设（作用域回溯）", note);
                ai->n_cot++;
                char resp0[240];
                snprintf(resp0, sizeof resp0, "撤了 第 %d 层（%s）", target + 1, what);
                if (out_conf) *out_conf = 0.7f;
                char *r = (char *)malloc(strlen(resp0) + 1);
                if (r) memcpy(r, resp0, strlen(resp0) + 1);
                return r;
            }
            if (ai->hypo.depth == 0) return NULL;      /* 没有假设：交给别的层 */
            /* 找不着就别乱撤 —— 如实说 */
            char resp0[200];
            snprintf(resp0, sizeof resp0, "没 找到 那条 假设（%s；现在 有 %zu 层）",
                     how ? how : "没听清", ai->hypo.depth);
            if (out_conf) *out_conf = 0.5f;
            char *r = (char *)malloc(strlen(resp0) + 1);
            if (r) memcpy(r, resp0, strlen(resp0) + 1);
            return r;
        }
    }
    if (n < 3 || n > 9) return NULL;
    /* 假设引导词 */
    static const char *lead[] = {"如果", "假设", "要是", "假如", "若", NULL};
    size_t off = 0;
    for (size_t i = 0; lead[i]; ++i)
        if (strcmp(words[0], lead[i]) == 0) { off = 1; break; }
    if (off == 0) return NULL;
    /* 「如果 他 以为 猫 会 飞」——把「以为」前面那个人当这层假设的归属 */
    Token *who = NULL;
    if (n - off >= 2) {
        static const char *verb_think[] = {"以为", "认为", "觉得", "想", NULL};
        for (size_t i = 0; verb_think[i]; ++i) {
            if (strcmp(words[off + 1], verb_think[i]) == 0) {
                who = pool_token_intern(ai->pool, words[off]);
                off += 2;
                break;
            }
        }
    }
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    if (off >= n) return NULL;
    if (!rel_parse(ai, words + off, n - off, jbuf, sizeof jbuf, &aw, &rid,
                   &is_neg, &bw, &is_q, &rw, 0)) return NULL;
    if (is_q) return NULL;                     /* 「如果…吗」不是设假设 */
    Token *A = pool_token_intern(ai->pool, aw);
    Token *B = pool_token_intern(ai->pool, bw);
    if (!A || !B || A == B) return NULL;

    /* 和图里的已知事实冲突吗？冲突也要如实说，而不是假装没看见。 */
    int conflict = is_neg ? rel_direct(A, rid, B) : rel_direct_neg(A, rid, B);
    /* 和**已有假设**冲突吗？同一层里对同一对词的相反极性要报出来。 */
    const HypoFact *old = hypo_find(&ai->hypo, NULL, A, rid, B, NULL);
    int hypo_conflict = old && ((int)old->neg != (is_neg ? 1 : 0));
    char note[160], resp0[320];
    /* **追加一层**，不再替换 —— 后一个「如果」是一次新的假设，
     * 平时两者并存；这正是「多层」的意思。同一层里同一对词则覆盖。 */
    if (!hypo_push(&ai->hypo, who)) {
        /* 层数满了：退化成「在同一层里再加一条」 */
        if (!hypo_add(&ai->hypo, A, B, rid, is_neg)) return NULL;
    } else if (!hypo_add(&ai->hypo, A, B, rid, is_neg)) {
        hypo_pop(&ai->hypo);
        return NULL;
    }
    ai->n_hypo++;
    snprintf(note, sizeof note, "%s%s %s %s%s",
             is_neg ? "不 " : "", aw, rw, bw,
             who && who->name ? "（这层算在「他」头上）" : "");
    cot_push(ai, COT_HYP, 1, 80, "进入假设世界（不写进知识库）", note);
    ai->n_cot++;
    if (hypo_conflict)
        snprintf(resp0, sizeof resp0,
                 "好 ， 就 当 %s %s%s %s —— 注意 这 和 你 刚才 说 的 相反 ，"
                 "我 按 最新 这条 算", aw, is_neg ? "不 " : "", rw, bw);
    else if (conflict)
        snprintf(resp0, sizeof resp0,
                 "好 ， 就 当 %s %s%s %s —— 不过 我 知道 实际 上 不是 这样 ，"
                 "下面 说 的 都 是 假设 下 的",
                 aw, is_neg ? "不 " : "", rw, bw);
    else if (who && who->name)
        snprintf(resp0, sizeof resp0, "好 ， 就 当 %s 以为 %s %s%s %s",
                 who->name, aw, is_neg ? "不 " : "", rw, bw);
    else
        snprintf(resp0, sizeof resp0, "好 ， 就 当 %s %s%s %s",
                 aw, is_neg ? "不 " : "", rw, bw);
    if (out_conf) *out_conf = 0.7f;
    char *r = (char *)malloc(strlen(resp0) + 1);
    if (r) memcpy(r, resp0, strlen(resp0) + 1);
    return r;
}

/* 步骤一点七：**站到别人的立场上问**。
 *
 *   「他 觉得 猫 属于 鸟 吗」/「他 以为 猫 会 飞 吗」
 *
 * 语义：把我（AI）自己的假设层**排除掉**，只留归在「他」头上的那几层。
 * 于是同一个问题换个立场答案可以不同 —— 而不同的原因很清楚：
 * 我的假设他看不见。这就是最小可用的视角推理。
 *
 * 设置 ai->view_who，只在**这一轮**有效（下一轮开头清掉）。
 * 返回 1 表示认出了这个形状（调用方接着照常走推理流程）。 */
static inline int agent_set_perspective(AI *ai, char *const *words, size_t n,
                                       Token **out_who) {
    if (!ai || !words || n < 2) return 0;
    /* 只认**看法类**动词。别把「知道」「想」放进来 ——
     * `你 知道 量子 吗` 会被当成「站到『你』的立场」，于是把「你 知道」剥掉，
     * 剩下「量子 吗」去匹配帧，全盘皆错（实测把 cot/segmentation/framegate 三套
     * 回归一起打挂了）。视角标记必须是**不会和普通问法混淆**的那几个。 */
    static const char *verbs[] = {"觉得", "以为", "认为", NULL};
    int found = -1;
    for (size_t i = 0; verbs[i] && found < 0; ++i)
        if (strcmp(words[1], verbs[i]) == 0) found = (int)i;
    if (found < 0) return 0;
    /* 剥掉两个词之后至少要剩 3 个，否则不剥（避免把短句拆散） */
    if (n < 5) return 0;
    /* 「我 觉得 X」= 我自己的视角 —— 标记照样剥掉，只是不换立场。
     * 不剥的话「我 觉得 猫 会 飞 吗」整句会掉到检索层去（实测）。 */
    if (strcmp(words[0], "我") == 0) {
        if (out_who) *out_who = NULL;
        return 1;
    }
    ai->view_who = pool_token_intern(ai->pool, words[0]);
    if (out_who) *out_who = ai->view_who;
    return 1;
}

/* 步骤二：用户在问「<A> [不] <关系词> <B> 吗」吗？是就推理回答。
 *
 * **这一版把链变成一等公民**：先构造 Derivation（目标 → 事实/规则 → 结论），
 * 交给验证器**独立重推每一步**，验过了才渲染成话。
 *   真   → 「对 ， <带规则名的链>」
 *   假   → 「不 对 ， <否定依据>」
 *   未知 → 先试类比推测（标注为推测）；推不出来就返回 NULL，
 *          交给检索/帧/降级去说「不确定」，**绝不靠闭合世界假设硬答「不对」**。
 * 语言只出现在最后一步（der_say），换 ::style 就能换种说法说同一件事。 */
static inline char *agent_answer_relation_inner(AI *ai, char *const *words, size_t n,
                                               float *out_conf, int as_other) {
    if (!ai || ai->rel_off || !words || n < 4 || n > 7) return NULL;
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    /* allow_new=0：**问句只查不建**。早先这里也会登记，于是随便问一句
     * 就把「觉得」这类词塞进动态关系表（还写进模型）—— 真泄漏。 */
    if (!rel_parse(ai, words, n, jbuf, sizeof jbuf, &aw, &rid, &is_neg,
                   &bw, &is_q, &rw, 0)) return NULL;
    if (!is_q) return NULL;
    /* **只查不建**：解析阶段绝不往池子里塞新 Token（造出过 `火锅了` 那种垃圾）。 */
    Token *A = pool_find(ai->pool, aw);
    Token *B = pool_find(ai->pool, bw);
    if (!A || !B) return NULL;
    int want = is_neg ? 1 : 0;

    Derivation *d = &ai->last_der;
    char resp0[400], line[400];

    /* 先试肯定，再试否定 —— 三态里的真/假 */
    Proof pf;
    int solved = 0;
    rel_solve(ai, A, rid, B, want, &pf);
    if (pf.status != 0) solved = 1;
    else rel_solve(ai, A, rid, B, !want, &pf);
    if (!solved && pf.status != 0) solved = 1;

    if (solved) {
        der_from_proof(ai, d, A, rid, B, &pf);
        /* 结论与问法是否一致：问「不 R」而推出「假」也是「对」。 */
        int agree = (pf.status == +1) != (want == 1);
        d->has_claim = 1;
        der_conclude(d, agree ? +1 : -1, 85);
        ai->n_der++;
        int gaps = der_verify(ai, d);
        if (gaps > 0) {
            /* 链上缺步 → **不下断言**，退回原来的路 */
            cot_push(ai, COT_INFER, 0, 20, "逻辑链有缺口，拒绝下结论",
                     d->step[0].note[0] ? "" : "");
            ai->n_cot++;
            return NULL;
        }
        der_chain_text(ai, d, line, sizeof line);
        cot_push(ai, COT_INFER, 1, 85,
                 pf.n > 2 ? "逻辑链（多跳，已验证）" : "逻辑链（直接事实，已验证）", line);
        ai->n_cot++;
        der_say(ai, d, ai->der_style, resp0, sizeof resp0);
        if (agree) ai->n_rel_yes++;
        else       ai->n_rel_no++;
        if (pf.n > 2) ai->n_rel_deep++;
        if (out_conf) *out_conf = 0.9f;
        char *r = (char *)malloc(strlen(resp0) + 1);
        if (r) memcpy(r, resp0, strlen(resp0) + 1);
        return r;
    }

    /* 未知 → 类比推测（明确标注），同样先构造再验证 */
    der_reset(d);
    d->has_claim = 1;
    if (rel_guess(ai, A, rid, B, &pf)) {
        /* 推测链的形状（rel_guess 给的 Proof 是 [A, 依据词, B]）：
         *   目标 → 推测（A 借 依据词 的意见）→ 事实（依据词确实知道这件事）→ 结论
         * 验证器会独立查：知识库里确实没有、相似度够、依据词确实有态度。 */
        Token *src = (pf.n >= 2 && pf.step[1].tok) ? pf.step[1].tok : A;
        int pol = (pf.n >= 1) ? (pf.step[pf.n - 1].neg ? -1 : +1) : +1;
        int sim = (pf.n >= 2) ? pf.step[1].sim : 0;
        der_goal(d, A, rid, B);
        der_default(d, A, src, rid, B, sim, pol);
        der_fact(d, src, rid, B, pol < 0 ? 1 : 0, pol);
        der_conclude(d, pol, 45);
        ai->n_der++;
        if (der_verify(ai, d) == 0) {
            der_chain_text(ai, d, line, sizeof line);
            cot_push(ai, COT_INFER, 1, 45,
                     "类比推测（知识库里没有，借最像的那个词；已验证推测合法）", line);
            ai->n_cot++;
            der_say(ai, d, ai->der_style, resp0, sizeof resp0);
            ai->n_rel_guess++;
            if (out_conf) *out_conf = 0.45f;
            char *r = (char *)malloc(strlen(resp0) + 1);
            if (r) memcpy(r, resp0, strlen(resp0) + 1);
            return r;
        }
    }

    /* 真的什么都不知道：留一条带缺口的链作证据，并**说清为什么**推不出来。
     * 「推不出来」有很多种原因，笼统说不知道等于没解释。 */
    der_reset(d);
    d->has_claim = 1;
    der_goal(d, A, rid, B);
    {
        const RelDef *rd2 = rel_by_id(rid);
        char why[200];
        if (ai->closed_world)
            snprintf(why, sizeof why, "闭合世界下也判不出来（主语不在知识库里）");
        else if (rd2 && rd2->excl)
            snprintf(why, sizeof why,
                     "独占规则也定不了：可能存在的关系带与它互相包含（比如「猫 是 动物」"
                     "不排除「猫 是 鸟」）");
        else
            snprintf(why, sizeof why,
                     "否定它需要完备性 —— 得知道所有可能，我的知识是开放的"
                     "（想按「没写就是假」算，打开 ::world closed）");
        der_gap(d, why);
    }
    der_conclude(d, 0, 0);
    der_verify(ai, d);
    ai->n_der++;
    cot_push(ai, COT_INFER, 0, 30, "逻辑链：推不出来", d->step[1].note);
    ai->n_cot++;
    ai->n_rel_unknown++;
    /* **反事实/知识层的拒绝要说出来**：假设开着的时候不能把问题悄悄丢给
     * 检索和帧层（它们看不到假设，会答得像没有假设一样）。 */
    if (hypo_visible_n(&ai->hypo, ai->view_who) > 0) {
        /* 答复用**短句**，长理由留在链里（::der 看得到）——
         * 长理由塞进答复还会被表达式前缀拼得又长又乱。
         * 注意判的是**这个视角下看得见**的假设条数：
         * 站在「他」的立场时我的假设他看不见，就不能说「在假设下」。 */
        char resp0[200];
        /* 站在别人的立场上时，「在假设下」这个说法会让人以为是**我**的假设 ——
         * 那其实是他名下的信念，所以说清「我没听他说过这个」。 */
        if (as_other) snprintf(resp0, sizeof resp0, "我 没 听 他 说 过 这 个 ， 推 不 出 来");
        else          snprintf(resp0, sizeof resp0, "在 假设 下 我 推不出来");
        if (out_conf) *out_conf = 0.25f;
        char *r = (char *)malloc(strlen(resp0) + 1);
        if (r) memcpy(r, resp0, strlen(resp0) + 1);
        return r;
    }
    return NULL;
}

/* 帧层给的是**文本**：把它拆回 Token 序列，再用同一套冲突检测。
 * （帧的答复本来就是空格分隔的词，和 ai_tokens_to_text 一个口径。） */
static inline size_t resp_conflicts_hypo(AI *ai, const char *text,
                                         char *note, size_t cap) {
    if (note && cap) note[0] = 0;
    if (!ai || !text || !*text) return 0;
    if (hypo_visible_n(&ai->hypo, ai->view_who) == 0) return 0;
    size_t nw = 0;
    char **w = ai_tokenize(ai, text, &nw);
    if (!w) return 0;
    Token *seq[64];
    size_t n = 0;
    for (size_t i = 0; i < nw && n < 64; ++i) {
        Token *t = w[i] ? pool_find(ai->pool, w[i]) : NULL;
        if (t) seq[n++] = t;
    }
    size_t nc = ai_hypo_conflicts(ai, ai->view_who, seq, n, note, cap);
    for (size_t i = 0; i < nw; ++i) free(w[i]);
    free(w);
    return nc;
}

/* ---- 「他 以为 …」：把他说的存到**他**名下 ----
 *
 * 和「如果 他 以为 …」的区别（这一条是「归属」从标注变成真东西的地方）：
 *   如果 他 以为 …  = 反事实假设（假设层，轮末就没了，不存盘）
 *   他 以为 …       = **他告诉我的看法**（他者信念表，持久，存进模型 v8）
 * 两者都**不进我的图** —— 区别只是「临时的假设」和「他名下的事实」。
 * 他信什么不影响我知道什么；问他的想法只查他的表。 */
static inline char *agent_learn_belief(AI *ai, char *const *words, size_t n,
                                       float *out_conf) {
    if (!ai || !words || n < 4 || n > 8) return NULL;
    static const char *verbs[] = {"以为", "认为", "觉得", "说", NULL};
    int vi = -1;
    for (size_t i = 0; verbs[i]; ++i)
        if (strcmp(words[1], verbs[i]) == 0) { vi = (int)i; break; }
    if (vi < 0) return NULL;
    if (strcmp(words[0], "我") == 0) return NULL;   /* 我自己的看法不是「别人」的 */
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    /* 只认**陈述**；「他 觉得 X 吗」是问他的立场，交给视角那条路 */
    if (!rel_parse(ai, words + 2, n - 2, jbuf, sizeof jbuf, &aw, &rid, &is_neg,
                   &bw, &is_q, &rw, 0)) return NULL;
    if (is_q) return NULL;
    Token *who = pool_token_intern(ai->pool, words[0]);
    Token *A = pool_token_intern(ai->pool, aw);
    Token *B = pool_token_intern(ai->pool, bw);
    if (!who || !A || !B || A == B) return NULL;
    OtherModel *om = other_intern(ai, who);
    if (!om || !other_belief_learn(ai, om, A, B, rid, is_neg)) return NULL;
    {
        char note[160];
        snprintf(note, sizeof note, "%s %s %s%s %s（存在他名下，不进我的知识）",
                 who->name ? who->name : "他", vi == 3 ? "说" : "以为",
                 aw, is_neg ? "不 " : "", bw);
        cot_push(ai, COT_HYP, 1, 75, "记下他的看法（归属给他，不是我的知识）", note);
        ai->n_cot++;
    }
    char resp0[240];
    /* 他的看法和我的知识相反时如实说出来 —— 这正是「归属」的意义所在 */
    int mine_pos = rel_direct(A, rid, B), mine_neg = rel_direct_neg(A, rid, B);
    const char *wv = (vi == 3) ? "说" : "以为";   /* 他说的是「说」，就别说成「以为」 */
    if (is_neg ? mine_pos : mine_neg)
        snprintf(resp0, sizeof resp0,
                 "记下 了 ： %s %s %s %s%s %s —— 可 我 知道 实际 上 不 是 这样",
                 who->name ? who->name : "他", wv, aw, is_neg ? "不 " : "", rw, bw);
    else
        snprintf(resp0, sizeof resp0, "记下 了 ： %s %s %s %s%s %s",
                 who->name ? who->name : "他", wv, aw, is_neg ? "不 " : "", rw, bw);
    if (out_conf) *out_conf = 0.75f;
    char *r = (char *)malloc(strlen(resp0) + 1);
    if (r) memcpy(r, resp0, strlen(resp0) + 1);
    return r;
}

/* ============================ 他者视角：多层合并 + 四态判定 ============================
 *
 * 借 lingshu 的两件事（`docs/对照-lingshu与五个局限.md` 局限 4）：
 *   ① **同类别并进滑动窗**：所有人名下的说法，只要与所问**同类**就并进一个窗里看。
 *      「同类」= 宾语相同，**或他的宾语是所问宾语的下位**（他说「学生 属于 人」，
 *      我问「学生 属于 动物」——`人 ⊂ 动物` 蕴含所问，算一条支持）；
 *   ② **冲突交四态判定**：都说认 / 都说不认 / **有分歧（待验，不是否定）** / 没人说过。
 *
 * 两条铁律没松：
 *   · 他的信念**不进我的图**：窗口是**查询时并出来的**，不落任何持久字段
 *     （存档格式一个字节没动）；
 *   · **不许拿别人的想法冒充答案**：窗口只报「谁说过什么 + 合并结论」，
 *     而且只有**蕴含所问**（下位宾语）的说法才算支持；上位宾语只标成
 *     「相关但不蕴含」，不参与判定。
 *
 * 为什么现算：`OtherModel.bel[]` 里每人只留**最新**的一版极性（人会说反话，
 * `other_belief_learn` 是覆盖语义）—— 那是设计，不是缺陷；窗口不存"历史"，
 * 所以重载之后同一个问题给出的是同一个答案（确定性）。 */
typedef enum {
    OBV_NONE   = 0,   /* 没人说过 */
    OBV_YES    = 1,   /* 都说认 */
    OBV_NO     = 2,   /* 都说不认 */
    OBV_DEFER  = 3,   /* 有人认、有人不认 —— **待验，不是否定** */
} OtherBeliefVerdict;

typedef struct {
    const Token    *who;
    const HypoFact *fact;
    int             entail;   /* 1 = 这条**蕴含**所问（下位宾语/同一个）；0 = 只是相关 */
} OtherBeliefObs;

typedef struct {
    OtherBeliefObs     obs[AI_MAX_OTHERS * 4];
    size_t             n;
    size_t             n_yes, n_no, n_related;   /* n_related = 相关但不蕴含 */
    size_t             n_who;                    /* 涉及到几个人 */
    OtherBeliefVerdict verdict;
} OtherBeliefView;

static inline const char *other_belief_verdict_name(OtherBeliefVerdict v) {
    switch (v) {
    case OBV_YES:   return "都说认";
    case OBV_NO:    return "都说不认";
    case OBV_DEFER: return "有分歧（待验，不是否定）";
    default:        return "没人说过";
    }
}

/* 他的宾语是不是**蕴含**所问（同一个，或是所问的下位）：`x ⊂ b`。
 * 两条判据都用现成的：类别链（`parent`）与关系层的 `属于`（含传递）——
 * 不新造"相似"口径。 */
static inline int other_obj_narrows(AI *ai, const Token *x, const Token *b) {
    if (!x || !b) return 0;
    if (x == b) return 1;
    if (token_is_descendant(x, b)) return 1;
    {
        Proof pf;
        memset(&pf, 0, sizeof pf);
        rel_solve(ai, x, REL_IS_A, b, 0, &pf);
        if (pf.status > 0) return 1;
    }
    return 0;
}

/* 把所有人名下与 (a, rid, b) 同类的说法并进一个窗，再交四态判定。 */
static inline void other_belief_view(AI *ai, const Token *a, uint8_t rid, const Token *b,
                                     OtherBeliefView *v) {
    if (!v) return;
    memset(v, 0, sizeof *v);
    v->verdict = OBV_NONE;
    if (!ai || !a || !b || rid == 0) return;
    size_t cap = sizeof v->obs / sizeof v->obs[0];
    for (size_t i = 0; i < ai->others_n && v->n < cap; ++i) {
        const OtherModel *om = &ai->others[i];
        for (size_t k = 0; k < om->n_bel && v->n < cap; ++k) {
            const HypoFact *f = &om->bel[k];
            if (f->a != a || f->rid != rid) continue;
            /* 同类判据：宾语相同 → 蕴含；他的宾语是所问的**下位** → 蕴含；
             * 他的宾语是所问的**上位** → 只是相关（不参与判定）。 */
            int entail = 0, related = 0;
            if (f->b == b) { entail = 1; related = 1; }
            else if (other_obj_narrows(ai, f->b, b)) { entail = 1; related = 1; }
            else if (other_obj_narrows(ai, b, f->b)) { related = 1; }
            if (!related) continue;
            OtherBeliefObs *o = &v->obs[v->n++];
            o->who = om->who; o->fact = f; o->entail = entail;
            if (!entail) { v->n_related++; continue; }
            if (f->neg) v->n_no++; else v->n_yes++;
        }
    }
    /* 涉及几个人（只数**蕴含**的那些，才代表"他们对此表过态"） */
    for (size_t i = 0; i < v->n; ++i) {
        if (!v->obs[i].entail) continue;
        int seen = 0;
        for (size_t j = 0; j < i; ++j)
            if (v->obs[j].entail && v->obs[j].who == v->obs[i].who) { seen = 1; break; }
        if (!seen) v->n_who++;
    }
    if (v->n_yes && v->n_no)     v->verdict = OBV_DEFER;
    else if (v->n_yes)           v->verdict = OBV_YES;
    else if (v->n_no)            v->verdict = OBV_NO;
    else                         v->verdict = OBV_NONE;
}

/* 打印一个窗口（`::others <A> <关系> <B>` 用）。返回四态判定，便于调用方再加工。 */
static inline OtherBeliefVerdict other_belief_view_print(AI *ai, const Token *a,
                                                         uint8_t rid, const Token *b) {
    const char *rn = rel_name_of(ai, rid);
    OtherBeliefView v;
    other_belief_view(ai, a, rid, b, &v);
    printf("[他者视角：%s %s %s —— %zu 个人表过态]\n",
           a && a->name ? a->name : "?", rn ? rn : "?",
           b && b->name ? b->name : "?", v.n_who);
    for (size_t i = 0; i < v.n; ++i) {
        const OtherBeliefObs *o = &v.obs[i];
        const HypoFact *f = o->fact;
        printf("    %s：%s %s%s %s", o->who && o->who->name ? o->who->name : "?",
               f->a && f->a->name ? f->a->name : "?",
               f->neg ? "不 " : "", rn ? rn : "?",
               f->b && f->b->name ? f->b->name : "?");
        if (!o->entail) printf("   ← 相关，但**不蕴含**所问（上位说法），不算依据");
        else if (f->b != b) printf("   ← **同类**（%s ⊂ %s），算依据",
                                   f->b && f->b->name ? f->b->name : "?",
                                   b && b->name ? b->name : "?");
        printf("\n");
    }
    printf("  合并判定：**%s**", other_belief_verdict_name(v.verdict));
    if (v.verdict == OBV_DEFER)
        printf("（%zu 认 / %zu 不认 —— 两条都留着，谁也不盖谁）", v.n_yes, v.n_no);
    if (v.verdict == OBV_NONE && v.n_related)
        printf("（只有 %zu 条相关但不蕴含的说法，不算答案）", v.n_related);
    printf("\n");
    printf("  我的知识没被改：他的信念只在**他的**名下（这条判定是现算的，不落库）\n");
    return v.verdict;
}

/* 站在别人的立场上回答：先把**他名下的信念**临时铺成一层假设（who = 他），
 * 让原来的求解器 / 验证器 / 链式渲染原样用上 —— 他的信念和归他的假设因此
 * 走**同一条**代码路径，不需要为它开特例。答完立刻原样撤回：
 * 恢复点用 depth 记，所以中间从哪儿 return 都不会漏（这就是要包一层的原因）。 */
static inline char *agent_answer_relation(AI *ai, char *const *words, size_t n,
                                          float *out_conf) {
    if (!ai || ai->rel_off || !words) return NULL;
    size_t save = ai->hypo.depth;
    if (ai->view_who) {
        OtherModel *om = other_find(ai, ai->view_who);
        if (om && om->n_bel && save < AI_MAX_HYPO_DEPTH &&
            hypo_push(&ai->hypo, ai->view_who)) {
            for (size_t i = 0; i < om->n_bel; ++i)
                hypo_add(&ai->hypo, om->bel[i].a, om->bel[i].b,
                         om->bel[i].rid, om->bel[i].neg);
        }
    }
    char *r = agent_answer_relation_inner(ai, words, n, out_conf,
                                          ai->view_who ? 1 : 0);
    while (ai->hypo.depth > save) hypo_pop(&ai->hypo);

    /* 再解析一次，用于两条**归属**特有的答复：
     *   ① 我的视角：我不知道，但他说过 → **引用**他的话（绝不当成事实）；
     *   ② 他的视角：他没说过这个 → 老老实实说不知道他怎么想，
     *      不许拿我的知识冒充他的想法（原来就是这么答的，那不算视角推理）。 */
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    Token *A = NULL, *B = NULL;
    if (n >= 4 && n <= 7 &&
        rel_parse(ai, words, n, jbuf, sizeof jbuf, &aw, &rid, &is_neg,
                  &bw, &is_q, &rw, 0) && is_q) {
        A = pool_find(ai->pool, aw);
        B = pool_find(ai->pool, bw);
    }
    if (A && B) {
        if (!r && ai->view_who) {
            OtherModel *om = other_find(ai, ai->view_who);
            if (!om || !other_has_subject(om, A)) {
                char resp0[160];
                snprintf(resp0, sizeof resp0, "我 不 知道 %s 怎么 想",
                         ai->view_who->name ? ai->view_who->name : "他");
                if (out_conf) *out_conf = 0.30f;
                char *q = (char *)malloc(strlen(resp0) + 1);
                if (q) memcpy(q, resp0, strlen(resp0) + 1);
                return q;
            }
        } else if (!r && !ai->view_who) {
            for (size_t i = 0; i < ai->others_n; ++i) {
                const OtherModel *om = &ai->others[i];
                const HypoFact *f = other_belief_find(om, A, rid, B);
                if (!f) continue;
                const RelDef *rd = rel_by_id(rid);
                char resp0[260];
                snprintf(resp0, sizeof resp0,
                         "%s 说 过 ： %s %s%s %s —— 我 不 知道 实情",
                         om->who && om->who->name ? om->who->name : "他",
                         aw, f->neg ? "不 " : "",
                         rd && rd->name ? rd->name : (rw ? rw : "?"), bw);
                if (out_conf) *out_conf = 0.35f;
                char *q = (char *)malloc(strlen(resp0) + 1);
                if (q) memcpy(q, resp0, strlen(resp0) + 1);
                return q;
            }
        } else if (r && ai->view_who) {
            /* 他的看法和我的知识相反 → 补一句，别让人以为我也这么看 */
            const OtherModel *om = other_find(ai, ai->view_who);
            const HypoFact *f = other_belief_find(om, A, rid, B);
            int mine_pos = rel_direct(A, rid, B), mine_neg = rel_direct_neg(A, rid, B);
            int disagree = f && ((f->neg && mine_pos) || (!f->neg && mine_neg));
            if (disagree) {
                size_t need = strlen(r) + 160;   /* 中文一个字 3 字节，别抠 */
                char *q = (char *)malloc(need);
                if (q) {
                    snprintf(q, need, "%s （这 是 他 的 看法 ； 我 知道 实际 不 是 这样）", r);
                    free(r);
                    r = q;
                }
            }
        }
    }
    return r;
}

static inline Agent *agent_new(AgentPerceiveFn perceive, AgentActFn act, void *user) {
    Agent *a = (Agent *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->brain = ai_new();
    if (!a->brain) { free(a); return NULL; }
    a->perceive = perceive; a->act = act; a->user = user;
    a->verbose = 1;
    return a;
}
static inline void agent_free(Agent *a) {
    if (!a) return;
    frames_free(a->brain);
    ai_free(a->brain);
    free(a);
}

static inline size_t agent_step(Agent *a, const char *input, char *out, size_t out_cap) {
    if (!a || !input || !out || out_cap == 0) return 0;
    out[0] = 0;

    AI *ai = a->brain;
    ai->pool->current_tick++;
    a->cycle++;
    /* 驱动参与决策（默认关）：先把驱动量折算成这一轮的参数修正，再走后面的流程。 */
    drive_policy_step(ai);            /* 驱动层（内核之外）折算预算修正 */
    if (ai->drive_d_gen) {
        int g = 6 + ai->drive_d_gen;                  /* 6 = 默认 max_gen */
        ai->max_gen = (g < 1) ? 1 : g;
    }
    if (ai->drive_d_cutoff != 0.0f) {
        float c = 0.05f + ai->drive_d_cutoff;         /* 0.05 = 默认 cutoff */
        ai->cutoff = (c < 0.01f) ? 0.01f : (c > 0.30f ? 0.30f : c);
    }

    /* 感知 */
    size_t n;
    char **words = ai_tokenize(ai, input, &n);
    /* 视角切换会把 words/n 往后挪两个词，但**释放必须用原始的指针** ——
     * 不然 free 的是数组中间，是堆破坏（不是崩一下就完的那种）。 */
    char **words_alloc = words;
    size_t n_alloc = n;
    if (!words || n == 0) {
        snprintf(out, out_cap, "……");
        ai_free_words(words_alloc, n_alloc);
        return strlen(out);
    }

    /* 反馈 + 表达自举 + 校准 */
    int fb = ai_feedback_hint(input);
    if (fb != 0) {
        if (ai->last_expr_mode >= 0)
            ai_learn_expression(ai, (ExprMode)ai->last_expr_mode, fb);
        ai_calibrate(ai, fb);
    }

    /* 他者识别 + 社会反馈 */
    OtherModel *speaker = ai_identify_speaker(ai, words, n);
    if (fb != 0 && speaker) ai_social_feedback(ai, speaker, fb);

    /* 反事实 */
    ai_learn_counterfactual(ai, words, n);
    /* 因果边 */
    ai_learn_causal(ai, words, n);

    /* 预测 + 惊讶 + 逐对学习 */
    size_t n_surprises = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        Token *prev   = pool_token_intern(ai->pool, words[i]);
        Token *actual = pool_token_intern(ai->pool, words[i + 1]);
        Token *pred   = ai_predict_readonly(ai, prev);
        if (pred && pred != actual) n_surprises++;
        ai_learn_pair(ai, prev, actual);
        ai_push_trace(ai, prev);
        ai->n_learned++;
    }
    float surprise = (n > 1) ? (float)n_surprises / (float)(n - 1) : 0.0f;

    /* 内生状态 + 自我模型 */
    ai_inner_update(ai, surprise, n_surprises == 0);
    ai_self_update(ai);

    /* 时间：把输入挂到当前时刻 */
    char tname[64];
    snprintf(tname, sizeof tname, "[时刻:%llu]",
             (unsigned long long)ai->pool->current_tick);
    Token *now = pool_token_intern(ai->pool, tname);
    for (size_t i = 0; i < n; ++i) {
        Token *t = pool_token_intern(ai->pool, words[i]);
        (void)ai_write_assoc(ai, now, t, AI_WSRC_PERCEIVE);   /* [时刻:] 链也是感知写入 */
    }

    /* 焦点 */
    for (size_t i = 0; i < n; ++i) {
        Token *t = pool_token_intern(ai->pool, words[i]);
        ai_focus_push(ai, t, 1.0f);
    }
    ai_focus_decay(ai, 0.85f);

    /* ---- 微内核：补丁注入 ----
     * 补丁把外部信号变成 Token 交进来，内核只当它们是一批 Token
     * （放进焦点 + 挂到当前时刻，剩下的靠关联学习自己长）。
     * **没挂补丁时这里是空转** —— 内核不依赖任何补丁。 */
    micro_patch_sense(ai);

    /* 内部对话 + 想象 */
    ai_inner_dialogue(ai);
    ai_imagine(ai);

    /* 生成 */
    Token *seed = pool_token_intern(ai->pool, words[n - 1]);
    ai_set_focus(ai, seed);
    token_touch(seed);

    /* 先给输入词投票选出话题，再让生成只在该话题的边里走 ——
     * 这就是「按意图约束解码」：锁定话题后跨话题的边被排除。 */
    {
        Token **toks = (Token **)malloc(n * sizeof *toks);
        if (toks) {
            for (size_t i = 0; i < n; ++i) toks[i] = pool_token_intern(ai->pool, words[i]);
            uint8_t tp = ai_detect_topic(ai, toks, n);
            ai->cur_topic = tp;
            free(toks);
        } else {
            ai->cur_topic = ASSOC_TOPIC_NONE;
        }
    }

    float conf = 0.5f;
    char *resp = NULL;

    /* ---- 思维链：这一轮从「读入」开始记 ---- */
    cot_reset(ai);
    ai->view_who = NULL;      /* 视角是「问法」的修饰，每轮开头清掉 */
    {
        char note[80];
        snprintf(note, sizeof note, "%zu 个词%s%s%s%s",
                 n,
                 ai->n_split ? "（有拆解）" : "",
                 ai->n_merge ? "（有合并）" : "",
                 ai->n_coined ? "（有造词）" : "",
                 ai->n_relay ? "（有中转容错）" : "");
        cot_push(ai, COT_READ, 1, 100, "分词 + token 代数", note);
        ai->n_cot++;
    }

    /* ---- 思维循环（前段）：**先想，再说** ----
     * 与下面「联想 / 关系层 / 帧 / 检索」的关系是：思维负责**追一个目标**
     * （成不成立、为什么、缺什么），输出流水线负责**把话说明白**。
     * 默认它只做内部思考并留痕（::delib），不改输出；
     * 打开 ::thinkout 之后它只在两种既有流水线给不出的情况下接管：
     * 知识自相矛盾、以及上次没想通的问题现在想通了。
     * 放在这里（而不是更后面）是因为它要用到「这一轮学到的知识」——
     * 那部分由下面的学习与收尾阶段补上，见 think_settle。 */
    think_run(ai, words, n);


    /* ---- 自由联想：内部思考，不直接决定输出 ----
     * 混合设计的关键一步：**先让它沿关联链想一遍**，把轨迹与候选留下来，
     * 然后再由「检索 / 结构化帧」决定实际说什么。
     * 这样「联想」这个能力一直活着（可以 ::think 看，也可以在
     * walk_output 打开时直接采用），但默认不会把碎片当回复吐出来。 */
    {
        char *idea = ai_plan_and_act(ai, seed, NULL, &conf, words, n);
        if (idea) {
            free(ai->last_idea);
            ai->last_idea = idea;              /* 留着给 ::think 看 */
            ai->n_walks++;
        }
        {
            char note[80];
            snprintf(note, sizeof note, "%s", idea ? idea : "（没联想出东西）");
            cot_push(ai, COT_ASSOC, idea ? 1 : 0, (int)(conf * 100.0f),
                     "沿关联链想一遍", note);
            ai->n_cot++;
        }
    }

    /* ---- 关系层（L2）----
     * 顺序：先看是不是在**陈述事实**（那就学下来），再看是不是在**问关系**
     * （那就推理回答）。两条都在联想之后 —— 先想一遍，再动确定性推理。 */
    if (!resp) {
        float rc = 0.5f;
        resp = agent_set_hypothesis(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    /* 「他 以为 X」= **他告诉我的看法**，存在他名下（持久，存盘）；
     * 必须排在 agent_learn_relation 前面 —— 否则它会被当成「我」的知识学进图里。 */
    if (!resp) {
        float rc = 0.5f;
        resp = agent_learn_belief(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    if (!resp) {
        float rc = 0.5f;
        resp = agent_learn_relation(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    /* 「他 觉得 <问题>」→ 站到他的立场（只留归他的假设层），
     * 然后把「他 觉得」摘掉，剩下的当普通问题继续走。 */
    {
        Token *who = NULL;
        if (agent_set_perspective(ai, words, n, &who)) {
            words += 2;
            n -= 2;
            char note[96];
            if (who)
                snprintf(note, sizeof note, "换到「%s」的立场：我自己的假设他看不见",
                         who->name ? who->name : "?");
            else
                snprintf(note, sizeof note, "「我 觉得」—— 还是我自己的视角，只把标记剥掉");
            cot_push(ai, COT_VIEW, 1, 70, "视角", note);
            ai->n_cot++;
        }
    }
    if (!resp) {
        float rc = 0.5f;
        resp = agent_answer_relation(ai, words, n, &rc);
        if (resp) {
            conf = rc;
            /* 在他人的立场上答出来的话，得写明“在 他 看来” */
            if (ai->view_who) {
                char pre[256];
                snprintf(pre, sizeof pre, "在 %s 看来 ： %s",
                         ai->view_who->name ? ai->view_who->name : "他", resp);
                free(resp);
                resp = (char *)malloc(strlen(pre) + 1);
                if (resp) memcpy(resp, pre, strlen(pre) + 1);
            }
        }
    }

    /* ---- 整句检索优先 ----
     * 把用户输入和语料里存下来的「输入句」比，找到最像的那条，
     * 直接把它对应的「回复句」整句吐出来。匹配够好就不走逐词游走了。 */
    if (ai->use_retrieval && ai->pairs_n > 0 && n > 0) {
        Token **toks = (Token **)malloc(n * sizeof *toks);
        if (toks) {
            for (size_t i = 0; i < n; ++i) toks[i] = pool_token_intern(ai->pool, words[i]);
            int sc = 0;
            int cov = 0;
            const AISentPair *hit = ai_sent_lookup(ai, toks, n, ai->cur_topic, &sc, &cov);
            ai->last_retrieval_cov = cov;
            /* 接受条件（三关都要过）：
             *   ① 总分够（话题命中门槛低些）；
             *   ② **输入覆盖率够高** —— 这是主关。总分里混了字符 bigram，
             *      短输入共享几个字就能到 60 分（「地球为什么是圆的」sc=63 就是这么来的）；
             *   ③ 输入不像「要答案的问题」。问句（带吗/什么/为什么/几/哪…）
             *      如果只是为了寒暄（「你好吗」）覆盖率会很高，不会走到这里；
             *      覆盖率低的问句就是真的答不上来 —— 宁可说不知道。
             * 三关不过就**拒答**，而不是硬塞一个最像的句子。 */
            int is_question = ai_has_question_word(words, n);
            int need = (ai->cur_topic == ASSOC_TOPIC_NONE) ? 85 : 60;
            /* cov 阈值按实测分布定（tools/dbg5.c 量的）：
             *   对话类探针的问句 cov 最小 45~69；
             *   知识类问题（地球为什么是圆的/1加1等于几…）cov 全是 0~23。
             * 所以 45 是一条很干净的线。另外 cov 太低（<25）时不管是不是问句都别硬答。 */
            /* 55 是按实测曲线选的折中点（tools/dbg6.c 量的）：
             *   45 → 对话保留 99.4%，知识误答 12/15
             *   55 → 对话保留 98.4%，知识误答 10/15
             *   65 → 对话保留 97.8%，知识误答  5/15
             * 注意：**cov 分不开这两类**，再往上调只是用对话质量换一点误答减少。
             * 真正的分界需要语义层面（是不是在问事实），不是这个阈值能做到的。 */
            int cov_need = 55;
            int ok = (hit && sc >= need && cov >= cov_need);
            if (!is_question && cov < 25) ok = 0;
            /* 第四关：**长度比**。cov 单独看会被「一个少见词命中」骗到 ——
             * 实测「1加1等于几」(4 词) cov=55，其实是拿 `几` 去撞了一条长句。
             * 真正该对应的记忆，输入句长度不会差太远。
             *   「你今天怎么样」(6) vs「我 今天 过得 很 充实」(7) → 0.86，过
             *   「1加1等于几」(4)   vs 8 词长句                      → 0.5，不过 */
            if (ok) {
                size_t L1 = ai->last_input_len, L2 = ai->last_retrieval_len;
                size_t hi = L1 > L2 ? L1 : L2, lo = L1 < L2 ? L1 : L2;
                if (hi > 0 && (double)lo / (double)hi < 0.45) ok = 0;
            }
            char recall_note[96];
            if (hit && ok)
                snprintf(recall_note, sizeof recall_note, "命中语料句 sc=%d cov=%d", sc, cov);
            else if (hit)
                snprintf(recall_note, sizeof recall_note, "最像的一条没过阈值（sc=%d cov=%d）", sc, cov);
            else
                snprintf(recall_note, sizeof recall_note, "语料里没有像的");
            /* 第五关：**和假设冲突的整句不许用**。
             * 这是「假设进检索层」的落点：命中一条很像的记忆，但它说的和假设相反，
             * 那就宁可不说 —— 否则「如果 猫 会 飞」之后还会听见「猫 不会 飞」。 */
            char veto_note[160]; veto_note[0] = 0;
            if (hit && ok) {
                Token *outseq[AI_MAX_SENT];
                size_t on = hit->out_n;
                if (on > AI_MAX_SENT) on = AI_MAX_SENT;
                for (size_t k = 0; k < on; ++k) outseq[k] = hit->out[k];
                if (ai_hypo_conflicts(ai, ai->view_who, outseq, on,
                                      veto_note, sizeof veto_note) > 0) {
                    ok = 0;
                    snprintf(recall_note, sizeof recall_note,
                             "有一条更像的（sc=%d cov=%d），但它和假设相反：%s",
                             sc, cov, veto_note);
                }
            }
            /* ⓪ 如果显式打开了「联想直接输出」，就先采纳它（默认关闭）。
             * 这是混合模式的开关：平时联想只做内部思考，不参与输出。 */
            if (ai->walk_output) {
                char *idea = ai_plan_and_act(ai, seed, NULL, &conf, words, n);
                if (idea) { resp = idea; ai->n_walks++; }
            }
            /* ① 结构化帧优先：命中「问法骨架 + 槽位」就渲染一句新的。
             * 放在检索之前 —— 帧能识别「用户换了个槽位」这种输入，
             * 而检索遇到同样输入只会回一句最像的原句（含旧槽位）。
             * 实测这样才有真正的「造句」效果：
             *   你喜欢雪吗 → 我 喜欢 雪（语料里没写过这句） */
            if (!resp && ai->frames) {
                FrTrace tr;
                char *g = frames_generate(ai, toks, n, &tr);
                /* 帧层也要在假设下工作：渲染出来的话若和假设相反，直接作废。 */
                if (g && resp_conflicts_hypo(ai, g, veto_note, sizeof veto_note) > 0) {
                    free(g);
                    g = NULL;
                }
                if (g) { resp = g; conf = tr.used_fall ? 0.35f : 0.8f; }
                /* 把帧层的实情报进链里：骨架 → 校验 → 降级，三步咬合。
                 * 没有这一步，思维链就只是把四种策略并列念一遍。 */
                if (tr.ask_matched) {
                    char note[96];
                    snprintf(note, sizeof note, "%s%s",
                             tr.ask_frame ? tr.ask_frame : "?",
                             tr.matched ? "（骨架用上了）" : "（槽位没过校验）");
                    cot_push(ai, COT_PARSE, 1, 80, "认出问法骨架", note);
                } else {
                    cot_push(ai, COT_PARSE, 0, 0, "认出问法骨架", "没有骨架接得上");
                }
                ai->n_cot++;
                if (tr.ask_matched) {
                    int gok = tr.matched;
                    cot_push(ai, COT_GATE, gok, gok ? 90 : 0, "校验槽位类证据",
                             gok ? (tr.admit ? "靠关联链证据放行" : "取值在白名单里")
                                 : "类证据不够，不能拿它当这一类的东西说");
                    ai->n_cot++;
                }
                if (tr.used_fall) {
                    char note[96];
                    snprintf(note, sizeof note, "%s", resp ? resp : "");
                    cot_push(ai, COT_FALL, 1, 35, "降级：改说诚实答复",
                             note[0] ? note : "证据不够");
                    ai->n_cot++;
                    if (!ai->last_der.has_claim)
                        der_source(&ai->last_der, "降级答复（诚实说明证据不够）");
                } else if (tr.matched && !ai->last_der.has_claim) {
                    der_source(&ai->last_der, "结构化帧渲染（骨架合法，非逻辑结论）");
                }
                /* 检索排在骨架/降级之后 —— 前两步没话说才轮到它。
                 * 已经有人给话了就把这里记成「备选」，别谎报成命中了。 */
                if (resp && ok)
                    snprintf(recall_note, sizeof recall_note,
                             "已经有答案了（关系层/帧层给了话），sc=%d cov=%d 这条只当备选",
                             sc, cov);
                cot_push(ai, COT_RECALL, (!resp && ok) ? 1 : 0, sc, "整句检索", recall_note);
                ai->n_cot++;
                if (veto_note[0]) {
                    /* 假设否决了检索/帧层的说法 —— 这是「在假设下工作」的证据，
                     * 必须留在链上，否则看起来就像系统漏答了。 */
                    cot_push(ai, COT_HYP, 0, 20,
                             "假设否决了检索/帧层的说法（和假设相反）", veto_note);
                    ai->n_cot++;
                }
                /* 非逻辑路径也留个来源记录：::der 要能说清「这句话是从哪来的」 */
                if (!ai->last_der.has_claim && !resp && ok) {
                    char src[64];
                    snprintf(src, sizeof src, "整句记忆命中（sc=%d cov=%d）", sc, cov);
                    der_source(&ai->last_der, src);
                }
            }
            /* ② 扁平模板合成：**已停用**。
             * 它只要求「用户换了某个同位词」，没有槽位语义约束，
             * 于是会把「你 会 支持 我 吗」的 支持 当成技能填进「我 想 学 <X>」，
             * 产出「我 想 学 支持 我」。结构化帧（frame.h）已经用
             * 「问法骨架 + 允许填充集合」把这件事做对了，这里不再兜。
             * 代码保留（ai_synthesize / ai->n_synth）以便将来加约束再启用。 */
            (void)0;
            if (!resp && ok) {
                resp = ai_tokens_to_text(hit->out, hit->out_n);
                conf = 0.5f + (float)sc / 200.0f;      /* 越像越有把握 */
                if (conf > 0.95f) conf = 0.95f;
            }
            free(toks);
        }
    }
    if (!resp) {
        int is_question = ai_has_question_word(words, n);
        /* 逐词游走只在「陈述句且看起来有话说」时才兜底；
         * 它临场编句子很容易出「远 别 事 的」这种碎片，所以问题一律不交给它。 */
        /* 兜底的逐词游走只在「这条输入确实有话可说」时才允许：
         * 要求**词命中比例 ≥ 50%** —— 也就是命中的记忆里真的有一半以上
         * 是用户说过的词。只看 IDF 覆盖率会被一个少见词骗到
         * （「太阳 离 我们 多 远」cov=43，其实只有「远」出现过一次）。 */
        if (is_question || ai->last_retrieval_cov < 25 || ai->last_token_ratio < 50) {
            /* 没匹配上又是问题（或相似度低到没有意义）→ 诚实拒答，别瞎编 */
            size_t nref = 0;
            while (AI_REFUSALS[nref]) nref++;
            const char *rf = AI_REFUSALS[ai->n_refusals++ % nref];
            resp = (char *)malloc(strlen(rf) + 1);
            if (resp) memcpy(resp, rf, strlen(rf) + 1);
            conf = 0.2f;
            {
                char note[80];
                snprintf(note, sizeof note, "覆盖率 %d / 词命中比 %d，都不够",
                         ai->last_retrieval_cov, ai->last_token_ratio);
                cot_push(ai, COT_ADMIT, 0, 20, "认输：诚实拒答", note);
                ai->n_cot++;
                if (!ai->last_der.has_claim)
                    der_source(&ai->last_der, "拒答（覆盖率与词命中比都不够）");
            }
        } else {
            /* 结构化帧优先：能匹配到「问法骨架 + 槽位」就渲染一句新的。
             * 这是真正「说出语料里没写过的话」的路径（frame.h）。 */
            if (ai->frames) {
                Token **toks = (Token **)malloc(n * sizeof *toks);
                if (toks) {
                    for (size_t i = 0; i < n; ++i)
                        toks[i] = pool_token_intern(ai->pool, words[i]);
                    FrTrace tr;
                    resp = frames_generate(ai, toks, n, &tr);
                    if (tr.gate_failed) {
                        cot_push(ai, COT_GATE, tr.admit ? 1 : 0, 0,
                                 "校验槽位类证据",
                                 tr.admit ? "靠关联链证据放行" : "类证据不够");
                        ai->n_cot++;
                    }
                    if (tr.used_fall) {
                        cot_push(ai, COT_FALL, 1, 35, "改用诚实答复",
                                 resp ? "证据不够，但可以把话说明白" : "证据不够");
                        ai->n_cot++;
                    }
                    free(toks);
                }
            }
            if (!resp) {  /* 帧也匹配不上才退回逐词游走 */
                resp = ai_plan_and_act(ai, seed, NULL, &conf, words, n);
            }
        }
    }
    /* 收尾：把最终说出口的话记进链里，链到此闭合。 */
    {
        char note[80];
        snprintf(note, sizeof note, "%s", resp ? resp : "（无输出）");
        cot_push(ai, COT_DONE, resp ? 1 : 0, (int)(conf * 100.0f), "输出", note);
        ai->n_cot++;
    }
    ai->last_confidence = conf;
    ai->last_confidence_at_output = conf;

    size_t gen_steps = ai->last_path.n >= 1 ? ai->last_path.n - 1 : 0;
    MetaCog m = ai_metacog(ai, seed, conf, gen_steps, ai->max_gen, words, n);
    ai->meta = m;
    float novelty = sig_novelty(ai, words, n);
    ExprMode mode = ai_pick_expr_mode(ai, &m, novelty);
    const char *prefix = ai_render_expr(ai, mode);

    /* 内生信用分配 */
    if (surprise > 0.3f && ai->last_path.n >= 2)
        ai_credit_from_surprise(ai, surprise);

    /* 表达输出 */
    switch (mode) {
    case EXPR_CONFIDENT:
        snprintf(out, out_cap, "%s", resp ? resp : "");
        seed->recent_misses = 0;
        break;
    case EXPR_HEDGE:
    case EXPR_TENTATIVE:
    case EXPR_UNCERTAIN:
    case EXPR_LOST:
    case EXPR_CURIOUS:
        if (prefix && resp && resp[0])
            snprintf(out, out_cap, "%s，%s", prefix, resp);
        else if (prefix)
            snprintf(out, out_cap, "%s", prefix);
        else
            snprintf(out, out_cap, "%s", resp ? resp : "");
        seed->recent_misses++;
        if (mode == EXPR_LOST || mode == EXPR_CURIOUS) seed->misses++;
        break;
    case EXPR_SILENT:
    default:
        snprintf(out, out_cap, "……");
        seed->misses++;
        seed->recent_misses++;
        break;
    }
    ai->last_expr_mode = (int)mode;

    /* 思维接管输出（默认关，::thinkout on 打开）。
     * 只处理既有流水线给不出的两种情况 —— 矛盾、跨轮想通；
     * 别的一律不碰，所以那条被 21 套回归钉住的输出基线不受影响。 */
    if (think_take_output(ai, out, out_cap)) {
        if (!ai->last_der.has_claim)
            der_source(&ai->last_der, "思维：发现知识矛盾 / 跨轮想通（不是逻辑断言）");
        cot_push(ai, COT_INFER, 1, 70, "思维接管输出",
                 ai->last_der.source[0] ? ai->last_der.source : "内部思维的结果");
        ai->n_cot++;
    }

    if (mode == EXPR_CURIOUS) {
        for (size_t i = 0; i < n; ++i) {
            Token *t = pool_token_intern(ai->pool, words[i]);
            if (t && t->count == 0 && token_hits_total(t) == 0) {
                (void)ai_write_assoc(ai, ai->self, t, AI_WSRC_INNER);
                break;
            }
        }
    }

    /* 自举 */
    if (resp && resp[0] && strcmp(resp, "……") != 0) {
        size_t rn;
        char **rwords = ai_tokenize(ai, resp, &rn);
        if (rwords && rn > 0) {
            Token **toks = (Token **)malloc(rn * sizeof *toks);
            for (size_t i = 0; i < rn; ++i)
                toks[i] = pool_token_intern(ai->pool, rwords[i]);
            ai_set_last_output(ai, toks, rn);
            free(toks);
            if (rn > 1) {
                for (size_t i = 0; i + 1 < rn; ++i) {
                    Token *p2 = pool_token_intern(ai->pool, rwords[i]);
                    Token *q2 = pool_token_intern(ai->pool, rwords[i + 1]);
                    ai_learn_pair(ai, p2, q2);
                    ai->n_learned++;
                }
            }
        }
        ai_free_words(rwords, rn);
        a->n_spoke++;
    }

    /* 时间推进 */
    ai_timeline_advance(ai);

    /* ---- 微内核安全点 ----
     * 一轮的**唯一**结构修改入口：补丁动作先做（它看的是刚说出去的话），
     * 然后自修改走「提议 → 安全内核审查 → 应用 + 记账 → 元认知」。
     * 放在这里是因为所有遍历早就结束了 —— 加边/加 Token 不会
     * 让任何正在进行的 `for (i < t->count)` 失效。 */
    /* ---- 对话触发：自然语言 → Token 行为（表驱动，窄集）----
     * 放在这里（学完、话也组好了之后）：命中就用它的答复**替掉**流水线那句，
     * 但**不跳过**学习 —— 触发是"入口"，不是"特权"，动作照样走受审通道。 */
    {
        char tobuf[256];
        if (dialog_trigger_try(ai, words, n, tobuf, sizeof tobuf)) {
            snprintf(out, out_cap, "%s", tobuf);
            cot_push(ai, COT_INFER, 1, 80, "对话触发（Token 行为）", tobuf);
            ai->n_cot++;
        }
    }

    /* ---- 输出侧补丁（第一步）：暂存 + 动作补丁看 Token ----
     * 把"这一轮本来要说的话"先交给补丁暂存；能编辑自己发言的补丁（记事本）
     * 从这里拿到原文。**决策**留到后面 —— 它得先想完（think_settle）才有话说。 */
    patch_stage_all(ai, out);
    micro_patch_act(ai, out);

    /* ---- 思维循环（后段）：拿这一轮新学到的知识再想一遍旧问题 ----
     * 也是元认知看思维本身的地方（原地打转 / 未解问题太多 / 知识自相矛盾）。
     * 它提的结构修改（[自问:…] 节点、有证据的连结、**打草稿/发送**）走的是
     * 同一条意图队列，所以紧接着的安全点会一起审查、一起应用。 */
    think_settle(ai);

    micro_step(ai, words, n, out);

    /* ---- 写代码闭环（降级版；默认关）----
     * `AIP_CODE=<相对路径>` 时，**代理循环自己**在安全点之后走一遍
     * "写文件 → 跑编译器 → 看错误 → 按规则改一次 → 再跑一次"（一个进程只走一次）。
     * 它落的观测标记（`[代码:<rc>]`）走的是同一条意图队列 —— 所以上面那个安全点之后
     * 再单独 drain 一次（见 code.h）。默认不设这个环境变量就一个字都不做。 */
    if (code_auto_step(ai)) micro_step(ai, NULL, 0, out);

    /* ---- 输出侧补丁（第二步）：**问"这一轮到底发什么"** ----
     * 放在安全点**之后**：思维层这一轮要不要说、说什么（写草稿 / 发送 / 按住不说）
     * 都是刚才那批意图决定并应用的，这里才问得到正确答案。
     * 没挂补丁 / 补丁不接管 → out 仍是流水线文本（内核不依赖补丁）。 */
    {
        char pout[1024];
        int pr = patch_emit_all(ai, pout, sizeof pout);
        if (pr == 1)       snprintf(out, out_cap, "%s", pout);
        else if (pr == -1) out[0] = 0;          /* 补丁明确说：这一轮不说话 */
    }

    /* 维护 */
    ai->turns++;
    if (ai->consolidate_every && (ai->turns % ai->consolidate_every) == 0) {
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t->is_garbage) continue;
            token_consolidate(t);
            ai_write_count(ai, AI_WSRC_DECAY);
            token_decay_hits(t, ai->decay_factor);
        }
    }
    if (ai->gc.gc_interval && (ai->turns % ai->gc.gc_interval) == 0)
        ai_gc(ai);
    if (ai->sleep_every && (ai->turns % ai->sleep_every) == 0)
        ai_sleep_begin(ai);
    if (ai_sleep_pending(ai)) ai_sleep_step(ai);

    /* 漂移 / 诊断 / 叙事 */
    if (ai->drift_sample_every && (ai->turns % ai->drift_sample_every) == 0)
        ai_drift_sample(ai);
    if (ai->turns % 300 == 0)
        ai_diagnose(ai);
    if (ai->narrate_every && (ai->turns % ai->narrate_every) == 0) {
        char *narr = ai_narrate(ai, 5);
        if (narr) {
            size_t nn;
            char **nw = ai_tokenize(ai, narr, &nn);
            if (nw && nn > 0) {
                for (size_t i = 0; i < nn; ++i) {
                    Token *t = pool_token_intern(ai->pool, nw[i]);
                    (void)ai_write_assoc(ai, ai->self, t, AI_WSRC_INNER);
                }
            }
            ai_free_words(nw, nn);
            free(narr);
            ai->n_narrations++;
        }
    }

    free(resp);
    ai_free_words(words_alloc, n_alloc);
    ai->view_who = NULL;      /* 视角只对这一轮有效 */
    return strlen(out);
}

static inline void agent_run(Agent *a) {
    if (!a || !a->perceive) return;
    char out[4096];
    while (1) {
        const char *input = a->perceive(a->user);
        if (!input) {
            if (ai_sleep_pending(a->brain)) {
                ai_sleep_step(a->brain);
                continue;
            }
            break;
        }
        agent_step(a, input, out, sizeof out);
        if (a->act) a->act(out, a->user);
        if (a->verbose && (a->cycle % 100) == 0)
            ai_stats(a->brain);
    }
}

/* 内建 stdin/stdout */
static inline const char *stdin_perceive(void *user) {
    (void)user;
    static char buf[2048];
    printf("你 > ");
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) return NULL;
    size_t len = strlen(buf);
    if (len && buf[len-1] == '\n') buf[len-1] = 0;
    return buf;
}
static inline void stdout_act(const char *output, void *user) {
    (void)user;
    printf("AI > %s\n", output ? output : "");
    fflush(stdout);
}

#endif /* AGENT_H */