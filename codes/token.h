/* token.h — 单头文件关联机 v12
 *
 * 符号层：Token / 边 / hits / mem_state
 * 亚符号层：VSA 超向量 + 绑定/解绑/捆绑 + 视角树绑定
 */
#ifndef TOKEN_H
#define TOKEN_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* 语境字段（subs/subs_not）在 v10 里已删；这个上限**只剩"读 v9 旧文件"时的合法性检查**用
 * （见 model_io.h 的 has_ctx）。别再拿它开新数组 —— 边上没有语境了，见 AssocEntry 的注释。 */
#ifndef TOKEN_CTX_MAX
#define TOKEN_CTX_MAX 4
#endif
#ifndef TOKEN_UNKNOWING_NAME
#define TOKEN_UNKNOWING_NAME "（未知）"
#endif
#ifndef TOKEN_GAP_DEFAULT
#define TOKEN_GAP_DEFAULT 0.5f
#endif
#ifndef TOKEN_HASH_INIT
#define TOKEN_HASH_INIT 16u
#endif
#ifndef VSA_DIM
#define VSA_DIM 1024
#endif

#define TOKEN_SEQ_NONE ((uint32_t)-1)
#define TOKEN_ID_NONE  ((uint32_t)-1)

typedef struct Token           Token;
typedef struct TokenPool       TokenPool;
typedef struct AssocEntry      AssocEntry;
typedef struct ActivateCtx     ActivateCtx;
typedef struct NameEntry       NameEntry;
typedef struct PerspectiveNode PerspectiveNode;

typedef enum {
    MEM_ACTIVE = 0, MEM_FADING = 1, MEM_DORMANT = 2, MEM_FORGOTTEN = 3,
} MemState;

typedef void (*MeaningFree)(void *meaning);

typedef struct {
    size_t      (*dump_meaning)(void *meaning, uint8_t *out, size_t cap);
    void       *(*load_meaning)(const uint8_t *in, size_t len);
    MeaningFree  free_meaning;
} MeaningCodec;

typedef enum {
    /* 边的「角色」：这条边是在语料的哪个位置学到的。
     * 生成时只沿同一角色的边走 —— 这是让回复不跑偏的关键：
     * 从回复段里学到的转移，不会被语料别处的高频共现抢走。 */
    ASSOC_ROLE_NONE  = 0,   /* 通用共现（没标过） */
    ASSOC_ROLE_INPUT = 1,   /* 学自「输入段」内部 */
    ASSOC_ROLE_REPLY = 2,   /* 学自「回复段」内部（含 尾词→结束标记） */
    /* 现场造词时接的「类成员」边：新词 ↔ 该槽位声明的示范词。
     * 单独一种角色是为了让「这个词属于这一类」有一个**精确**的判据 ——
     * 只看邻居重叠（Dice/覆盖率）会误放：`天气` 的出边里有大半是
     * 「知道X吗」的示范词，纯覆盖率算下来能到 60% 以上，但其实它跟
     * 这一类没关系。只认这种角色，覆盖率就只对**真正接过线**的词成立。 */
    ASSOC_ROLE_COINED = 3,
    /* **推出来的**：不是听到的、也不是学来的，而是推理沉淀下来的
     * （见 docs/thinking.md 的「推理沉淀」一节）。它比观测弱：
     *   ① 求解器认得出它（ProofStep.known），证据强度按「缓存」算，低于现场推理；
     *   ② 一旦**观测**与它相反，观测赢，缓存那一半被清掉（token_tag_rel/nrel 里做）；
     *   ③ 它可以被撤销（安全内核只对「观测到的关系边」禁止删除）。
     * 这一档是 v9 之后**新用的值**，但字节本来就在存档里（role 早就是序列化字段），
     * 所以**格式没动、不用重训** —— 旧模型里不会有这个值，新模型里它照样读得回来。 */
    ASSOC_ROLE_DERIVED = 4,
    /* **基准知识**（来源监控里的"一开始就教过我的"）：`relations.txt` / 内置词表
     * 这类**显式教给它的**知识，装载时打这一档 —— 和"训练语料里泡出来的经验"、
     * "对话里听来的观测"分开。用途只有一个：**说清楚一条知识是哪来的**
     * （::edges / ::src / ::delib），从而让"你说的""我知道的""我推出来的"分得开。
     * 同样是 v9 之后新用的值，字节本来就在存档里（role 一直是序列化字段）→
     * **格式没动**；每次装载 `relations.txt` 都会重新打标，所以是确定性的。 */
    ASSOC_ROLE_INNATE = 5,
} AssocRole;

/* 来源档的中文名（一份，谁要显示都调它 —— 别在几个地方各写一遍）。 */
static inline const char *assoc_role_name(uint8_t role) {
    switch (role) {
    case ASSOC_ROLE_INPUT:   return "输入";
    case ASSOC_ROLE_REPLY:   return "回复";
    case ASSOC_ROLE_COINED:  return "造词";
    case ASSOC_ROLE_DERIVED: return "推论";
    case ASSOC_ROLE_INNATE:  return "基准";
    default:                 return "共现";
    }
}
/* 话题 id：语料里显式标注（`[话题:N]` 那一行属于第 N 个话题）。
 * 0 = 无话题（seed / generated 那些通用共现语料）。 */
#define ASSOC_TOPIC_NONE 0u

/* 句尾模式：这个 Token 后面**还有几个词**这个句子就结束了。
 * 用来在生成时知道「该收尾了」—— 只看有没有 [对话:结束] 是不够的，
 * 那条边经常选不中，句子就会顺着共现边一直漂。
 *   0 = 没标记
 *   1 = 后面还有 0 个词（它本身就是句尾）
 *   2 = 后面还有 1 个词
 *   3 = 后面还有 2 个及以上
 */
typedef enum {
    ASSOC_PAT_NONE     = 0,
    ASSOC_PAT_TAIL0    = 1,
    ASSOC_PAT_TAIL1    = 2,
    ASSOC_PAT_TAIL2P   = 3,
} AssocPattern;

/* ============================ 关系位掩码常量 ============================
 * 定义放在 token.h：AssocEntry 的 rel/nrel 就是按它排的位，
 * 位宽和 id 必须挨着看（放在 ai.h 里会因为包含顺序看不到）。 */
#define AI_MAX_DYNREL  16      /* 动态关系（清单外的谓语）最多几个 */
#define REL_TABLE_MAX  24      /* 运行时关系表最大 id（含动态关系） */

/* ---- 关系 id（rel.h 的 AI_RELS 用它们）---- */
#define REL_BIT(id) (uint16_t)(1u << ((id) - 1u))
#define REL_BOOST  32u
#define REL_IS_A    1u   /* 属于（是 也算） */
#define REL_HAS     2u   /* 有 */
#define REL_CAN     3u   /* 会 */
#define REL_FEAR    4u   /* 怕 */
#define REL_LIKE    5u   /* 喜欢 / 爱 */
#define REL_EAT    6u   /* 吃 */
#define REL_AT      7u   /* 在 */
#define REL_CONTAIN 8u   /* 包含（属于 的逆关系） */
/* 「需要」：做一件事的**前提**（唱歌 需要 声带）。
 * 有了它才能推出训练数据里没有的结论 ——
 *   唱歌 需要 声带；猫 没有 灵活的声带 ⟹ 猫 不 会 唱歌。
 * 这是「缺前提 → 否定能力」那条规则（R9）用的关系，见 rel.h。
 * 加第 9 个核心关系要求 rel/nrel 位掩码从 1 字节扩到 2 字节（模型 v9）。 */
#define REL_NEED    9u
#define REL_CORE_MAX REL_NEED       /* 核心关系的最大 id（循环用这个，别写 REL_AT） */
#define REL_DYN_BASE 10u /* 动态关系 id 从这里开始 */

struct AssocEntry {
    /* ⚠ **v10：边上的语境字段（subs/subs_not/n_subs/n_not）已删**。
     * 它原来占每条边 64 字节（8 个指针 + 2 个计数 = sizeof 的一半），
     * 而实测出厂模型 41138 条边里**一条带语境的都没有**（用户 2026-10-07 拍板「历史遗留可以删」）。
     * 条件本身没丢，只是从"边上的字段"搬成了"图里的节点 + 动作槽上的标记"：
     *   · 前提：`A —[前提]→ P → B`（P 是**普通 token**，`[前提]` 挂在动作槽上）→ edge_premise_polarity()
     *   · 因果猜想：动作槽上的 `[因果]` → edge_is_causal_guess()
     * 于是"联想"这条路上**没有任何条件**（用户原话：联想本来不依赖前提），
     * 只有逻辑/思维层按上面两个判据决定用不用。 */
    Token    *main;
    Token    *action;
    uint32_t  hits, seq;
    uint64_t  birth_tick, last_touch_tick;
    uint8_t   mem_state;
    uint8_t   role;         /* AssocRole：见上 */
    uint8_t   topic;        /* 话题 id，见 ASSOC_TOPIC_NONE */
    uint8_t   pattern;      /* AssocPattern：离句尾还有几个词 */
    /* 语义关系的**位掩码**（见 rel.h 的 AI_RELS）：0 = 不是关系边。
     * 用位掩码而不是单个 id，是因为同一对词可以同时有多种关系 ——
     * `老师 有 学生` 和 `老师 喜欢 学生` 都是真的。只存一个 id 的话，
     * 后写的会盖掉先写的，而且存盘再读时两条事实会**各自再加一次权**，
     * hits 翻倍、emb 跟着变（这个坑实测踩过，tools/rtdiff.c 就是为它写的）。
     * 7 种关系正好塞进一个字节。
     * 注意 role 记的是**出处**（学自输入段还是回复段），rel 记的才是**语义**。
     * 这个字节**写进模型**（v6 起），因为对话里现场学到的关系必须能存下来。 */
    /* 位掩码：bit(id-1)。第 9 个核心关系「需要」用到了 bit8，所以是 2 字节
     * （模型 v9 起；v8 及以前在文件里是 1 字节，读的时候按单字节读）。 */
    uint16_t  rel;          /* 核心关系的**肯定**位掩码（bit0..8 = REL_IS_A..REL_NEED） */
    /* 核心关系的**否定**位掩码：`猫 不 会 飞` 这种否定事实要有地方存。
     * 有了它，「不知道」和「知道是假的」才能分开 ——
     * 之前只能靠闭合世界假设硬说「不对」，那是不成立的推理（见 rel.h 文件头）。 */
    uint16_t  nrel;
    /* 动态关系 id（1..AI_MAX_DYNREL，0 = 无）：
     * 清单之外的谓语（比如「想念」）也能当关系记住，这样词表就是开放的。
     * 动态关系只支持肯定，不支持否定 —— 老实记在文档里。 */
    uint8_t   dyn;
};

struct Token {
    uint32_t    id;
    void       *meaning;
    AssocEntry *assocs;
    size_t      count, cap;
    uint32_t    next_seq;
    uint32_t    misses, recent_misses;
    Token      *parent;
    uint8_t     is_category, is_garbage;
    uint64_t    birth_tick, last_touch_tick;
    uint8_t     mem_state;
    TokenPool  *owner;
    const char *name;
    int8_t     *hv;
    int8_t     *hv_prev;
    /* emb = **学出来的**分布式表示（1024 维 int8）。
     * 和 hv 的区别：hv 是「名字的随机投影」（vsa_init(hash(name))），只适合做
     * bind/unbind 代数；emb 是从图的邻居按 PPMI 加权做**随机索引**投影出来的，
     * 所以语义相关的词余弦真的会高。
     * emb 是 (边 + hits + 稳定 id) 的确定性函数 → **不进模型文件**，
     * 训练收尾和读盘之后各现算一遍（见 ai_embed_build）。 */
    int8_t     *emb;
    Token      *perspective_of;
    Token      *next_in_time;
};

/* ============================ 前提 token（淘汰「语境」后的表示） ============================
 *
 * 用户的澄清（原话）：「联想本来不依赖前提，可以正常联想；前提 token 可以被正常联想到，
 * 但是**逻辑会知道这两个之间的前提 token 是什么**，并且可以被正常处理。」
 *
 * 所以一条「有前提的连结」在图上长这样：
 *
 *     A —[前提]→ P → B          （P 是**普通 token**；`[前提]` 是行为 token，挂在 A→P 的**动作槽**上）
 *
 * 三条性质：
 *   ① **联想照走**：A→P→B 都是普通边，别处不因为「有前提」而被挡住；
 *   ② **前提能被正常联想到**：P 就是普通 token（可以是被说出来的词），
 *      它自己有关系、有共现，谁也不特殊对待它；
 *   ③ **逻辑知道前提是什么**：`token_premise_between(A,B)` 直接答出 P（可多个）——
 *      条件不再藏在边的字段里，而是**图里的节点 + 边上的标记**。
 *
 * 旧的「语境」（AssocEntry.subs/subs_not）由此**退役**：先是没有创建者（2026-10-07 上午），
 * 然后连字段一起删掉（模型 v10；旧文件仍读得回来，见 docs/语境退役-前提token.md）。
 * 注意 `[前提]` 是**标记**（行为 token），不是「前提 token」本身 ——
 * 前提 token 是它指向的那个普通词。 */
#define AI_PREMISE_MARK     "[前提]"      /* 条件：P **在**注意窗口里 */
#define AI_PREMISE_NOT_MARK "[前提不]"    /* 条件：P **不在**注意窗口里（旧 subs_not 的替代） */
/* 因果猜想的行为 token（也挂在**动作槽**上）—— 名字只有这一处：
 * ai.h 的 `ai->causal` 也用它 intern，判据也只有这一个（edge_is_causal_guess）。 */
#define AI_CAUSAL_MARK      "[因果]"

/* 这条边是「前提链接」吗？极性：+1 = 正前提（P 要在），-1 = 反前提（P 要不出现），0 = 不是。
 * 判据只有一份：所有消费者（求解器/验证器/思维层/查询）都走它。 */
static inline int edge_premise_polarity(const AssocEntry *e) {
    if (!e || !e->action || !e->action->name) return 0;
    if (strcmp(e->action->name, AI_PREMISE_MARK) == 0)     return +1;
    if (strcmp(e->action->name, AI_PREMISE_NOT_MARK) == 0) return -1;
    return 0;
}
static inline int edge_is_premise_link(const AssocEntry *e) {
    return edge_premise_polarity(e) != 0;
}
/* 这条边是「因果猜想」吗？—— 三条护栏共用这一个判据（见 ai.h 的 ai_learn_causal）：
 *   ① 知识查询（rel.h 的 rel_direct_*）认不出它；
 *   ② 走边（ai_edge_walkable）/预测（ai_predict_readonly）挡掉它；
 *   ③ 削弱只走 ai_weaken_causal。 */
static inline int edge_is_causal_guess(const AssocEntry *e) {
    return e && e->action && e->action->name &&
           strcmp(e->action->name, AI_CAUSAL_MARK) == 0;
}
/* 「这条边是**普通边**（观测/共现/知识/缓存）」吗？—— 因果猜想**不是**。
 * v9 里这个区分靠"边上有没有语境字段"（`n_subs == 0` 就是普通边）；语境字段删掉之后
 * 判据搬到动作槽上，于是**所有"按 main 找那条边"的循环都必须用这个函数跳过猜想边**：
 * token_edge_is_derived / token_add_assoc_unique / token_tag_* / token_observe_pair /
 * token_remove_assoc / token_set_action_by_target / micro_edge_index / rel.h 的各查询。
 * ⚠ 少写一处，猜想就会冒充知识 —— 实测过一次：`token_edge_is_derived` 漏了这一条，
 *   「撤销缓存」静默失效（tests/consolidate.sh 第 ④ 节红，见 §6.50 的返工记录）。 */
static inline int edge_is_plain(const AssocEntry *e) { return !edge_is_causal_guess(e); }

/* A 到 P 的那条**前提链接**边的指针（没有就 NULL） */
static inline const AssocEntry *token_premise_link(const Token *from, const Token *to) {
    if (!from || !to) return NULL;
    for (size_t i = 0; i < from->count; ++i) {
        const AssocEntry *e = &from->assocs[i];
        if (e->main == to && edge_is_premise_link(e)) return e;
    }
    return NULL;
}

/* A 出边里有没有**前提链接**（有的话，逻辑层就该按前提决定用不用） */
static inline int token_has_premise_link(const Token *from) {
    if (!from) return 0;
    for (size_t i = 0; i < from->count; ++i)
        if (edge_is_premise_link(&from->assocs[i])) return 1;
    return 0;
}

/* **逻辑问的那句话**：A 与 B 之间的前提 token 是什么（带**极性**）？
 * 顺着 A —[前提]/[前提不]→ P → B 找；pol 里写 +1（要 P 在）/ -1（要 P 不在）。
 * 返回条数（一件事可以有多个前提）。 */
static inline size_t token_premises(const Token *a, const Token *b,
                                    const Token **out, int *pol, size_t cap) {
    size_t n = 0;
    if (!a || !b) return 0;
    for (size_t i = 0; i < a->count && n < cap; ++i) {
        const AssocEntry *e = &a->assocs[i];
        int p = edge_premise_polarity(e);
        if (!p || !e->main) continue;
        const Token *pt = e->main;
        for (size_t j = 0; j < pt->count; ++j)
            if (pt->assocs[j].main == b) {
                if (out) out[n] = pt;
                if (pol) pol[n] = p;
                n++;
                break;
            }
    }
    return n;
}
/* 不带极性的旧接口（只关心"有没有前提、是谁"的调用方继续用它） */
static inline size_t token_premise_between(const Token *a, const Token *b,
                                           const Token **out, size_t cap) {
    return token_premises(a, b, out, NULL, cap);
}

/* ============================ 顺序：图里写着的 vs tick ============================
 *
 * 用户 2026-10-08 的实测（audit §6.57）留了一条缺口：**顺序结构写进图了，查询层没接**
 * —— `::order` 只看"首次见到的 tick"，于是 `[原语:时间链]` 建的 A→B 它不认，
 * 同一个 tick 里先后也看不出来。这一条把查询层接上（**不是新语义**：读的就是已经写进图的边）。
 *
 * 图里的顺序有两种写法（都是现有原语建的，没新造记号）：
 *   ① `next_in_time` 链：`[原语:之后]/[原语:之前]/[原语:时间链]` 建的 A → … → B（可多跳）；
 *   ② `[关系:之后]` / `[关系:之前]` **标记边**：A 的边里同时挂着标记和 B（`[原语:之后]` 的写法，
 *      标记的含义是"a 在 b 之后/之前"）。
 * 返回 +1 = **a 先** / −1 = **b 先** / 0 = 图里没说（调用方退回 tick 比较）。
 * `why` 写明依据（哪条链、几跳、哪条标记边）—— 查询层必须说得出"凭什么"。 */
#define TOKEN_ORDER_MARK_AFTER  "[关系:之后]"
#define TOKEN_ORDER_MARK_BEFORE "[关系:之前]"
#define TOKEN_ORDER_HOP_MAX     16

/* 有界拼接（token.h 是最底层，**不引 stdio** —— 这一层不打印、也不格式化） */
static inline void token_order_text(char *dst, size_t cap,
                                    const char *a, const char *mid, const char *b) {
    const char *parts[3] = { a, mid, b };
    size_t k = 0;
    if (!dst || cap == 0) return;
    for (int i = 0; i < 3; ++i) {
        if (!parts[i]) continue;
        for (const char *q = parts[i]; *q && k + 1 < cap; ++q) dst[k++] = *q;
    }
    dst[k] = 0;
}

static inline int token_order_between(const Token *a, const Token *b,
                                      char *why, size_t whycap, int *hops_out) {
    if (why && whycap) why[0] = 0;
    if (hops_out) *hops_out = 0;
    if (!a || !b || a == b) return 0;
    const char *an = a->name ? a->name : "?";
    const char *bn = b->name ? b->name : "?";
    /* ① next_in_time 链（最多 TOKEN_ORDER_HOP_MAX 跳；有环也不会转不出来） */
    {
        const Token *cur = a;
        for (int hop = 1; hop <= TOKEN_ORDER_HOP_MAX; ++hop) {
            cur = cur->next_in_time;
            if (!cur) break;
            if (cur == b) {
                if (why && whycap)
                    token_order_text(why, whycap, "`next_in_time` 链：", an, " → … → ");
                if (why && whycap) {
                    size_t l = strlen(why);
                    if (l + 1 < whycap) token_order_text(why + l, whycap - l, bn, NULL, NULL);
                }
                if (hops_out) *hops_out = hop;
                return +1;
            }
        }
        cur = b;
        for (int hop = 1; hop <= TOKEN_ORDER_HOP_MAX; ++hop) {
            cur = cur->next_in_time;
            if (!cur) break;
            if (cur == a) {
                if (why && whycap)
                    token_order_text(why, whycap, "`next_in_time` 链：", bn, " → … → ");
                if (why && whycap) {
                    size_t l = strlen(why);
                    if (l + 1 < whycap) token_order_text(why + l, whycap - l, an, NULL, NULL);
                }
                if (hops_out) *hops_out = hop;
                return -1;
            }
        }
    }
    /* ② `[关系:之后]/[关系:之前]` 标记边：X 上同时挂着标记和另一个词，就说明这两个的先后 */
    {
        const char *marks[2] = { TOKEN_ORDER_MARK_AFTER, TOKEN_ORDER_MARK_BEFORE };
        for (int side = 0; side < 2; ++side) {
            const Token *x = side ? b : a;
            const Token *y = side ? a : b;
            int aft = 0, bef = 0, linked = 0;
            for (size_t i = 0; i < x->count; ++i) {
                const Token *m = x->assocs[i].main;
                if (!m || !m->name) continue;
                if (strcmp(m->name, marks[0]) == 0)      aft = 1;
                else if (strcmp(m->name, marks[1]) == 0) bef = 1;
                else if (m == y)                         linked = 1;
            }
            if (!linked || (!aft && !bef)) continue;
            /* `[关系:之后]` 挂在 x 上 ⇒ **y 先**；`[关系:之前]` 挂在 x 上 ⇒ **x 先** */
            int x_first = bef ? +1 : -1;
            if (why && whycap)
                token_order_text(why, whycap, "边上的 ", bef ? marks[1] : marks[0], " 标记");
            return side ? -x_first : x_first;
        }
    }
    return 0;                    /* 图里没说：调用方退回 tick 比较 */
}


struct NameEntry { uint32_t hash, id; NameEntry *next; };

struct PerspectiveNode {
    Token           *who;
    PerspectiveNode *parent;
    uint32_t         depth;
    uint32_t         refcount;
};

struct TokenPool {
    Token     **tokens;
    size_t      count, cap;
    Token      *unknowing;
    MeaningFree meaning_free;
    NameEntry **buckets;
    size_t      n_buckets, n_entries;
    uint64_t    current_tick;

    PerspectiveNode  *perspective_root;
    PerspectiveNode  *perspective_cur;
    PerspectiveNode **perspective_pool;
    size_t            perspective_n, perspective_cap;
};

/* ============================ VSA 核心 ============================ */

static inline void vsa_init(int8_t *hv, uint32_t seed) {
    uint32_t x = seed ? seed : 0x9E3779B9u;
    for (size_t i = 0; i < VSA_DIM; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        hv[i] = (x & 1) ? 1 : -1;
    }
}
static inline void vsa_bind(const int8_t *a, const int8_t *b, int8_t *out) {
    for (size_t i = 0; i < VSA_DIM; ++i) out[i] = a[i] * b[i];
}
static inline void vsa_unbind(const int8_t *bound, const int8_t *role, int8_t *out) {
    for (size_t i = 0; i < VSA_DIM; ++i) out[i] = bound[i] * role[i];
}
static inline void vsa_bundle(const int8_t *const *vecs, size_t n, int8_t *out) {
    if (n == 0) { for (size_t i = 0; i < VSA_DIM; ++i) out[i] = 1; return; }
    int32_t *acc = (int32_t *)calloc(VSA_DIM, sizeof(int32_t));
    if (!acc) { for (size_t i = 0; i < VSA_DIM; ++i) out[i] = 1; return; }
    for (size_t k = 0; k < n; ++k)
        for (size_t i = 0; i < VSA_DIM; ++i) acc[i] += vecs[k][i];
    for (size_t i = 0; i < VSA_DIM; ++i) out[i] = (acc[i] >= 0) ? 1 : -1;
    free(acc);
}
static inline float vsa_cosine(const int8_t *a, const int8_t *b) {
    int32_t dot = 0;
    for (size_t i = 0; i < VSA_DIM; ++i) dot += a[i] * b[i];
    return (float)dot / (float)VSA_DIM;
}
static inline void vsa_merge(int8_t *dst, const int8_t *src, float alpha) {
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;
    int32_t *acc = (int32_t *)malloc(VSA_DIM * sizeof(int32_t));
    if (!acc) return;
    for (size_t i = 0; i < VSA_DIM; ++i)
        acc[i] = (int32_t)((float)dst[i] * (1.0f - alpha) + (float)src[i] * alpha);
    for (size_t i = 0; i < VSA_DIM; ++i) dst[i] = (acc[i] >= 0) ? 1 : -1;
    free(acc);
}

/* ============================ 哈希 ============================ */

static inline uint32_t tok_hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}
static inline int pool_rehash(TokenPool *p, size_t new_size) {
    NameEntry **nb = (NameEntry **)calloc(new_size, sizeof *nb);
    if (!nb) return -1;
    for (size_t i = 0; i < p->n_buckets; ++i) {
        NameEntry *e = p->buckets[i];
        while (e) {
            NameEntry *next = e->next;
            size_t idx = e->hash & (new_size - 1);
            e->next = nb[idx]; nb[idx] = e;
            e = next;
        }
    }
    free(p->buckets);
    p->buckets = nb; p->n_buckets = new_size;
    return 0;
}
static inline void pool_hash_insert(TokenPool *p, Token *t) {
    if (!t->name) return;
    uint32_t h = tok_hash_str(t->name);
    size_t idx = h & (p->n_buckets - 1);
    NameEntry *e = (NameEntry *)malloc(sizeof *e);
    if (!e) return;
    e->hash = h; e->id = t->id;
    e->next = p->buckets[idx];
    p->buckets[idx] = e;
    p->n_entries++;
    if (p->n_entries * 4 > p->n_buckets * 3)
        (void)pool_rehash(p, p->n_buckets * 2);
}

/* ============================ 池 ============================ */

static inline TokenPool *pool_new(void) {
    TokenPool *p = (TokenPool *)calloc(1, sizeof(TokenPool));
    if (!p) return NULL;
    p->perspective_root = (PerspectiveNode *)calloc(1, sizeof(PerspectiveNode));
    if (!p->perspective_root) { free(p); return NULL; }
    p->perspective_root->who = NULL;
    p->perspective_root->parent = NULL;
    p->perspective_root->depth = 0;
    p->perspective_root->refcount = 1;
    p->perspective_cur = p->perspective_root;
    return p;
}
static inline void pool_set_meaning_free(TokenPool *p, MeaningFree fn) {
    if (p) p->meaning_free = fn;
}
static inline void pool_free(TokenPool *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (p->meaning_free && t->meaning) p->meaning_free(t->meaning);
        free(t->assocs); free(t->hv); free(t->hv_prev); free(t->emb);
        free((void *)t->name); free(t);
    }
    free(p->tokens);
    if (p->buckets) {
        for (size_t i = 0; i < p->n_buckets; ++i) {
            NameEntry *e = p->buckets[i];
            while (e) { NameEntry *n = e->next; free(e); e = n; }
        }
        free(p->buckets);
    }
    if (p->perspective_pool) {
        for (size_t i = 0; i < p->perspective_n; ++i)
            free(p->perspective_pool[i]);
        free(p->perspective_pool);
    }
    free(p->perspective_root);
    free(p);
}
static inline Token *pool_token(TokenPool *p, const char *name, void *meaning) {
    if (!p) return NULL;
    if (p->n_buckets == 0 && pool_rehash(p, TOKEN_HASH_INIT) != 0) return NULL;
    if (p->count == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 8;
        Token **nt = (Token **)realloc(p->tokens, ncap * sizeof *nt);
        if (!nt) return NULL;
        p->tokens = nt; p->cap = ncap;
    }
    Token *t = (Token *)calloc(1, sizeof *t);
    if (!t) return NULL;
    if (name) {
        size_t len = strlen(name);
        char *copy = (char *)malloc(len + 1);
        if (!copy) { free(t); return NULL; }
        memcpy(copy, name, len + 1);
        t->name = copy;
    }
    t->hv      = (int8_t *)malloc(VSA_DIM);
    t->hv_prev = (int8_t *)malloc(VSA_DIM);
    t->emb     = (int8_t *)calloc(VSA_DIM, 1);   /* 全 0 = 还没算过 */
    if (!t->hv || !t->hv_prev || !t->emb) {
        free(t->hv); free(t->hv_prev); free(t->emb);
        free((void *)t->name); free(t);
        return NULL;
    }
    uint32_t seed = t->name ? tok_hash_str(t->name)
                            : (uint32_t)p->count ^ (uint32_t)p->current_tick;
    vsa_init(t->hv, seed);
    memcpy(t->hv_prev, t->hv, VSA_DIM);

    t->id = (uint32_t)p->count;
    t->meaning = meaning;
    t->birth_tick = p->current_tick;
    t->last_touch_tick = p->current_tick;
    t->mem_state = MEM_ACTIVE;
    t->owner = p;
    p->tokens[p->count++] = t;
    if (t->name) pool_hash_insert(p, t);
    return t;
}
static inline Token *pool_token_by_id(const TokenPool *p, uint32_t id) {
    if (!p || id >= p->count) return NULL;
    return p->tokens[id];
}
static inline Token *pool_token_intern(TokenPool *p, const char *name) {
    if (!p || !name) return NULL;
    if (p->n_buckets == 0 && pool_rehash(p, TOKEN_HASH_INIT) != 0) return NULL;
    uint32_t h = tok_hash_str(name);
    size_t idx = h & (p->n_buckets - 1);
    for (NameEntry *e = p->buckets[idx]; e; e = e->next) {
        if (e->hash != h) continue;
        Token *t = p->tokens[e->id];
        if (t->name && strcmp(t->name, name) == 0) {
            if (t->is_garbage) {
                t->is_garbage = 0;
                t->birth_tick = p->current_tick;
                t->last_touch_tick = p->current_tick;
                t->mem_state = MEM_ACTIVE;
                t->misses = 0; t->recent_misses = 0;
                if (t->hv && t->hv_prev) memcpy(t->hv_prev, t->hv, VSA_DIM);
            }
            return t;
        }
    }
    return pool_token(p, name, NULL);
}
static inline Token *pool_token_child(TokenPool *p, Token *parent,
                                      const char *name, void *meaning) {
    Token *t = pool_token(p, name, meaning);
    if (t) t->parent = parent;
    return t;
}
/* 只读查找：池里有没有这个精确名字（不做 interning，不改图） */
static inline Token *pool_find(const TokenPool *p, const char *name) {
    if (!p || !name || !p->n_buckets) return NULL;
    uint32_t h = tok_hash_str(name);
    size_t idx = h & (p->n_buckets - 1);
    for (NameEntry *e = p->buckets[idx]; e; e = e->next) {
        if (e->hash != h) continue;
        Token *t = p->tokens[e->id];
        if (t->name && strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

static inline Token *pool_unknowing(TokenPool *p) {
    if (!p) return NULL;
    if (!p->unknowing)
        p->unknowing = pool_token(p, TOKEN_UNKNOWING_NAME, NULL);
    return p->unknowing;
}
/* 这条边是**推出来的**（缓存）还是观测/学来的？求解器与安全内核都要问这个。 */
static inline int token_edge_is_derived(const Token *t, const Token *main) {
    if (!t || !main) return 0;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].main == main && edge_is_plain(&t->assocs[i]))
            return t->assocs[i].role == ASSOC_ROLE_DERIVED;
    return 0;
}

static inline void token_set_category(Token *t, int on) { if (t) t->is_category = on ? 1 : 0; }
static inline void token_set_parent(Token *t, Token *parent) { if (t) t->parent = parent; }
static inline int token_is_descendant(const Token *t, const Token *ancestor) {
    if (!t || !ancestor) return 0;
    while (t) { if (t == ancestor) return 1; t = t->parent; }
    return 0;
}

/* ============================ touch ============================ */

static inline void token_touch(Token *t) {
    if (!t || t->is_garbage) return;
    uint64_t now = t->owner ? t->owner->current_tick : 0;
    t->last_touch_tick = now;
    t->mem_state = MEM_ACTIVE;
}
static inline void token_touch_entry(Token *t, AssocEntry *e) {
    if (!t || !e) return;
    e->hits++;
    uint64_t now = t->owner ? t->owner->current_tick : 0;
    e->last_touch_tick = now;
    e->mem_state = MEM_ACTIVE;
}

/* ============================ 关联编辑 ============================ */

static inline int token_add_assoc_ex(Token *t, Token *main, Token *action) {
    if (!t) return -1;
    if (t->count == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 4;
        AssocEntry *na = (AssocEntry *)realloc(t->assocs, ncap * sizeof *na);
        if (!na) return -1;
        t->assocs = na; t->cap = ncap;
    }
    AssocEntry *e = &t->assocs[t->count];
    e->main = main; e->action = action;
    e->hits = 0;
    e->seq = t->next_seq++;
    e->birth_tick = t->owner ? t->owner->current_tick : 0;
    e->last_touch_tick = e->birth_tick;
    e->mem_state = MEM_ACTIVE;
    e->role = ASSOC_ROLE_NONE;      /* 新边默认通用角色/无话题，不留脏值 */
    e->topic = ASSOC_TOPIC_NONE;
    e->pattern = ASSOC_PAT_NONE;
    e->rel = 0;                     /* 不是关系边 */
    e->nrel = 0;
    e->dyn = 0;
    t->count++;
    return 0;
}
/* 「t —main→ 这条边在不在」—— v10 起边上没有语境，所以只看 main。
 * （同 main 的两条边只可能是**动作槽不同**，那种用 token_find_assoc_action。） */
static inline int token_add_assoc_unique(Token *t, Token *main) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].main == main && edge_is_plain(&t->assocs[i])) return 0;
    return token_add_assoc_ex(t, main, NULL);
}
/* 找 (main, action) 都对的那条边（没有就 NULL）。
 * 为什么需要：因果猜想边（动作槽 = `[因果]`）要和**普通观测边并存** ——
 * 「猫 吃 鱼」是观测到的知识，「我做 X 之后 Y 出现」是猜想，两者不是同一条边。 */
static inline AssocEntry *token_find_assoc_action(Token *t, Token *main, Token *action) {
    if (!t) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].main == main && t->assocs[i].action == action) return &t->assocs[i];
    return NULL;
}
/* 按 (main, action) 建边：已经有了就把那条返回（不新建、不动 hits）。 */
static inline AssocEntry *token_add_assoc_action_unique(Token *t, Token *main, Token *action) {
    AssocEntry *e = token_find_assoc_action(t, main, action);
    if (e) return e;
    if (token_add_assoc_ex(t, main, action) != 0) return NULL;
    return &t->assocs[t->count - 1];
}

/* 「这条知识是显式教我的（基准），还是我自己经历/推出来的」——
 * 来源监控要问的就是这一句。 */
static inline int assoc_role_is_innate(const AssocEntry *e) {
    return e && e->role == ASSOC_ROLE_INNATE;
}
/* 给一条（已存在的）边打上「基准」档；边不在就先建再打。 */
static inline void token_mark_innate(Token *a, Token *b) {
    if (!a || !b) return;
    (void)token_add_assoc_unique(a, b);
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].main == b) { a->assocs[i].role = ASSOC_ROLE_INNATE; return; }
}

/* ============================ 逐条可信度（#5） ============================
 *
 * 用户点过的「逐条可信度权重」。它**不是新存的字段**，而是从已有字段算出来的
 * 启发式 —— 所以**格式没动、不用重训**：
 *   ① 来源档：基准 60 起（显式教过）、观测 50 起、推论 35 起（推出来的最弱）；
 *   ② 命中次数：每次 +2，最多 +25（见过很多次更可信）；
 *   ③ 新近：离上次被碰到越久扣越多（每 5000 tick −5，最多 −20）。
 * 只用于**说清"我更信哪一条"**（`::src`、矛盾报告），不参与真值判定 ——
 * 除非显式打开 `::trust on`（那一步会改行为，先 A/B）。 */
/* 逐条可信度的**算法本体**：来源档 + 命中加成 得正分，再乘一个**新近权重**
 * （0..1）。权重由 `ai.h` 的"唯一时间核"算好传进来 —— 衰减口径全库只有一处；
 * 这里不 include ai.h（头文件顺序），所以核值由调用方给。 */
/* 来源档那一档给多少分**由调用方定**（线上是 Beta 后验，见 ai.h 的 ai_source_class_pct；
 * 这里留旧口径当默认，方便单元测试与离线统计）。 */
static inline int assoc_credibility_cls(const AssocEntry *e, double recency_w, int class_pct) {
    if (!e) return 0;
    int base = (class_pct >= 0 && class_pct <= 100) ? class_pct : 50;
    int hits_bonus = (e->hits > 12) ? 25 : (int)e->hits * 2;
    int pos = base + hits_bonus;
    if (pos > 85) pos = 85;
    if (recency_w < 0.0) recency_w = 0.0;
    if (recency_w > 1.0) recency_w = 1.0;
    int v = (int)((double)pos * recency_w + 0.5);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
/* 旧口径（固定档 60/50/35，不衰减）：单元测试与离线统计用；线上请用 ai_credibility(ai, e) */
static inline int assoc_credibility_w(const AssocEntry *e, double recency_w) {
    int cls = (e && e->role == ASSOC_ROLE_INNATE)  ? 60
            : (e && e->role == ASSOC_ROLE_DERIVED) ? 35 : 50;
    return assoc_credibility_cls(e, recency_w, cls);
}
static inline int assoc_credibility(const AssocEntry *e, uint64_t now_tick) {
    (void)now_tick;
    return assoc_credibility_w(e, 1.0);
}
/* 把 a→b 这条边标成某个角色，并给它加权。
 * 边不存在就先建（这正是「从语料里学到的位置信息」落地的地方）。
 * 只标正向边：生成是沿 main 走的，所以只需要 a→b 这一侧有角色。 */
/* 给 a→b 打上 角色 + 话题 并加权；边不存在就先建。 */
static inline int token_tag_full(Token *a, Token *b, uint8_t role,
                                 uint8_t topic, uint8_t pattern, uint32_t bump) {
    if (!a || !b || a == b) return -1;
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &a->assocs[i];
        if (e->main != b || !edge_is_plain(e)) continue;
        e->role = role;
        e->topic = topic;
        if (pattern) e->pattern = pattern;
        e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &a->assocs[a->count - 1];
    e->role = role;
    e->topic = topic;
    e->pattern = pattern;
    e->hits = bump;
    return 0;
}

/* ---- 观测改口语义（默认开，`::revise off` 可关，用于 A/B 对照）----
 *
 * 同一对词、同一条关系上，**后落的那条观测替掉先落的对立极性**。
 * 为什么：用户先说「猫 有 毛」、后说「猫 没有 毛」，那是**改口**，不是
 * 「系统里有两份互相矛盾的依据」。旧语义下两个极性并存 →
 * 每次提问都报「关于猫有毛这件事，我这儿有互相矛盾的依据」，
 * 用户教的修正永远留不下来（docs/thinking.md 第六点六节那条局限）。
 *
 * 边界（三条，都写在代码里）：
 *   ① 只改**极性**，不删边 —— 「关系边不许删」那条铁律没松；
 *   ② 只改**同一对词、同一条关系**；不同宾语的多值事实各自并存；
 *   ③ **观测压过缓存**不算改口（那是「具体压过一般」，另一套语义，
 *      确认话也不一样）。
 *
 * `g_obs_revised` 是「刚刚那次 tag 是不是改了口」的一次性标志：
 * 调用方（agent_learn_relation）落完观测立刻读它，据此换一句确认话。
 * 和 g_rel_ignore_derived 一样是模块级约定 —— 单线程、单次调用即读即清。 */
static int g_obs_revise  = 1;   /* 默认开 */
static int g_obs_revised = 0;   /* 这一次 tag 改口了吗（tag 入口先清 0） */

/* 给 a→b 打上语义关系（边不存在就先建）。
 * 和 token_tag_full 分开，是因为「出处角色」和「语义关系」是两件事：
 * 一条关系边也可能同时带着回复角色。 */
static inline int token_tag_rel(Token *a, Token *b, uint8_t rel_id, uint32_t bump) {
    if (!a || !b || a == b || rel_id == 0 || rel_id >= REL_DYN_BASE) return -1;
    g_obs_revised = 0;
    uint16_t bit = (uint16_t)(1u << (rel_id - 1));
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &a->assocs[i];
        if (e->main != b || !edge_is_plain(e)) continue;
        /* **观测压过缓存**：这条边若是以前推出来沉淀的，而现在来了观测
         * （不管说的是同一件事还是它的反面），先把缓存**整个撤掉**再落观测。
         * 理由：同一条边上不混两种来源 —— 混了之后
         *   ① 「这条边是推出来的吗」就答不清楚（撤销时会误删观测）；
         *   ② 缓存与观测并存会出现「又肯定又否定」，之后每次都报「自相矛盾」。 */
        if (e->role == ASSOC_ROLE_DERIVED) {
            e->rel = 0; e->nrel = 0;
            e->role = ASSOC_ROLE_NONE;
        }
        /* 改口：把**同一条边上对立极性**的旧观测改掉（见上面那段注释）。
         * 缓存那一半上面已经清过了，所以走到这里还留着的对立位一定是观测。 */
        if (g_obs_revise && (e->nrel & bit)) { e->nrel &= (uint16_t)~bit; g_obs_revised = 1; }
        /* **幂等**：这一位已经置过就不再加权（关系可以叠加，同一位不能重复加）。
         * 否则存盘再读会重复装一遍 relations.txt，hits 翻倍 → emb 跟着变。 */
        if (e->rel & bit) return 0;
        e->rel |= bit;
        if (bump) e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &a->assocs[a->count - 1];
    e->rel = bit;
    e->hits = bump;
    return 0;
}

/* 给 a→b 打上**否定**的核心关系（`猫 不 会 飞`） */
static inline int token_tag_nrel(Token *a, Token *b, uint8_t rel_id, uint32_t bump) {
    if (!a || !b || a == b || rel_id == 0 || rel_id >= REL_DYN_BASE) return -1;
    g_obs_revised = 0;
    uint16_t bit = (uint16_t)(1u << (rel_id - 1));
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &a->assocs[i];
        if (e->main != b || !edge_is_plain(e)) continue;
        /* 同 token_tag_rel：观测进来时先把这条边上的缓存整个撤掉 */
        if (e->role == ASSOC_ROLE_DERIVED) {
            e->rel = 0; e->nrel = 0;
            e->role = ASSOC_ROLE_NONE;
        }
        /* 改口：把同一条边上对立极性的旧观测改掉（理由见 token_tag_rel） */
        if (g_obs_revise && (e->rel & bit)) { e->rel &= (uint16_t)~bit; g_obs_revised = 1; }
        if (e->nrel & bit) return 0;
        e->nrel |= bit;
        if (bump) e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &a->assocs[a->count - 1];
    e->nrel = bit;
    e->hits = bump;
    return 0;
}

/* 给 a→b 打上**动态关系**（清单外的谓语，id 从 8 起） */
static inline int token_tag_dyn(Token *a, Token *b, uint8_t dyn_id, uint32_t bump) {
    if (!a || !b || a == b || dyn_id == 0) return -1;
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &a->assocs[i];
        if (e->main != b || !edge_is_plain(e)) continue;
        if (e->dyn == dyn_id) return 0;
        e->dyn = dyn_id;
        if (bump) e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &a->assocs[a->count - 1];
    e->dyn = dyn_id;
    e->hits = bump;
    return 0;
}

static inline int token_tag_role_topic(Token *a, Token *b, uint8_t role,
                                       uint8_t topic, uint32_t bump) {
    return token_tag_full(a, b, role, topic, ASSOC_PAT_NONE, bump);
}

static inline int token_tag_role(Token *a, Token *b, uint8_t role, uint32_t bump) {
    return token_tag_role_topic(a, b, role, ASSOC_TOPIC_NONE, bump);
}

static inline int token_link_bidirectional(Token *a, Token *b) {
    if (!a || !b) return -1;
    if (token_add_assoc_unique(a, b) != 0) return -1;
    if (token_add_assoc_unique(b, a) != 0) return -1;
    return 0;
}
static inline int token_remove_assoc(Token *t, Token *main) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (e->main != main || !edge_is_plain(e)) continue;
        memmove(e, e + 1, (t->count - i - 1) * sizeof *e);
        t->count--;
        return 0;
    }
    return -1;
}
static inline int token_set_action(Token *t, uint32_t seq, Token *action) {
    if (!t || seq == TOKEN_SEQ_NONE) return -1;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].seq == seq) { t->assocs[i].action = action; return 0; }
    return -1;
}
static inline int token_set_action_by_target(Token *t, Token *main, Token *action) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (e->main != main || !edge_is_plain(e)) continue;
        e->action = action;
        return 0;
    }
    return -1;
}
static inline void token_clear_actions(Token *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; ++i) t->assocs[i].action = NULL;
}

/* ============================ 条件：**不在边上**（v10 起） ============================
 *
 * 语境（subs/subs_not）从边上删掉之后，这里不再有"语境筛选"。
 * 留着这一个函数（而不是顺手把 ctx 参数链一起拆掉）有两个理由：
 *   ① 筛选点散在 8 处（联想 / 激活 / 两个信号 / 选择器 / focus_step），
 *      `ctx/n_ctx` 参数横跨 token.h → token_lang.h → ai.h → think.h 四层；
 *   ② 把"边上的条件"收在**一个地方**：以后真要给边接条件，从这里接（别处不许偷偷加）。
 * 它现在**只做一件事**：挡掉**因果猜想**（那不是知识 —— 见 ai.h 的三条护栏）。
 * 这一条在 v9 里是靠"带语境"顺带挡住的（n_subs>0），语境字段删了就必须在这里显式挡，
 * 否则猜想边会漏进联想/扩散激活/置信度（**实测会改回答**：见 §6.50 的返工记录）。
 * **条件本身没丢，是搬走了**（用户原话：联想本来不依赖前提，逻辑才知道前提是什么）：
 *   · 前提 → 动作槽上的 `[前提]`/`[前提不]`（edge_premise_polarity），逻辑/思维层用；
 *   · 因果猜想 → 动作槽上的 `[因果]`（edge_is_causal_guess），这里挡住不算知识。 */
static inline int entry_matches(const AssocEntry *e, Token *const *ctx, size_t n_ctx) {
    (void)ctx; (void)n_ctx;
    return !edge_is_causal_guess(e);
}

/* ============================ 稳定 handle ============================ */

static inline AssocEntry *token_seq_to_entry(Token *t, uint32_t seq) {
    if (!t || seq == TOKEN_SEQ_NONE) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].seq == seq) return &t->assocs[i];
    return NULL;
}
static inline const AssocEntry *token_seq_to_entry_c(const Token *t, uint32_t seq) {
    if (!t || seq == TOKEN_SEQ_NONE) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (t->assocs[i].seq == seq) return &t->assocs[i];
    return NULL;
}
static inline Token *token_main_by_seq(const Token *t, uint32_t seq) {
    const AssocEntry *e = token_seq_to_entry_c(t, seq);
    return e ? e->main : NULL;
}
static inline int token_hit_seq(Token *t, uint32_t seq) {
    AssocEntry *e = token_seq_to_entry(t, seq);
    if (!e) return 0;
    token_touch_entry(t, e);
    return 1;
}

/* ============================ 联想 ============================ */

static inline size_t token_reach(const Token *t, float energy) {
    if (!t || t->count == 0) return 0;
    if (energy < 0) energy = 0;
    if (energy > 1) energy = 1;
    size_t reach = (size_t)(energy * (float)t->count) + 1;
    if (reach > t->count) reach = t->count;
    return reach;
}
static inline size_t token_associate_idx(const Token *t, Token *const *ctx,
                                         size_t n_ctx, float energy) {
    size_t reach = token_reach(t, energy);
    for (size_t i = 0; i < reach; ++i)
        if (entry_matches(&t->assocs[i], ctx, n_ctx)) return i;
    return (size_t)-1;
}
static inline Token *token_associate(const Token *t, Token *const *ctx,
                                     size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    return i == (size_t)-1 ? NULL : t->assocs[i].main;
}
static inline Token *token_associate1(const Token *t, Token *ctx, float energy) {
    if (ctx) { Token *c[1] = { ctx }; return token_associate(t, c, 1, energy); }
    return token_associate(t, NULL, 0, energy);
}
static inline uint32_t token_associate_seq(const Token *t, Token *const *ctx,
                                           size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    return i == (size_t)-1 ? TOKEN_SEQ_NONE : t->assocs[i].seq;
}
static inline void token_hit(Token *t, size_t i) {
    if (t && i < t->count) token_touch_entry(t, &t->assocs[i]);
}
static inline Token *token_associate_tap(Token *t, Token *const *ctx,
                                         size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    if (i == (size_t)-1) return NULL;
    token_touch_entry(t, &t->assocs[i]);
    return t->assocs[i].main;
}
static inline uint32_t token_associate_tap_seq(Token *t, Token *const *ctx,
                                               size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    if (i == (size_t)-1) return TOKEN_SEQ_NONE;
    token_touch_entry(t, &t->assocs[i]);
    return t->assocs[i].seq;
}
static inline Token *token_associate_weighted(const Token *t, Token *const *ctx,
                                              size_t n_ctx, float energy, uint32_t *rng) {
    if (!t || !rng) return NULL;
    if (*rng == 0) *rng = 0x9E3779B9u;
    size_t reach = token_reach(t, energy);
    uint64_t total = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &t->assocs[i];
        if (entry_matches(e, ctx, n_ctx)) total += (uint64_t)e->hits + 1;
    }
    if (total == 0) return NULL;
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x;
    uint64_t pick = (uint64_t)x % total, acc = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &t->assocs[i];
        if (entry_matches(e, ctx, n_ctx)) {
            acc += (uint64_t)e->hits + 1;
            if (pick < acc) return e->main;
        }
    }
    return NULL;
}

/* ============================ 空缺感 ============================ */

static inline uint32_t token_hits_total(const Token *t) {
    if (!t) return 0;
    uint32_t s = 0;
    for (size_t i = 0; i < t->count; ++i) s += t->assocs[i].hits;
    return s;
}
static inline float token_gap(const Token *t) {
    if (!t) return 0;
    uint32_t rm = t->recent_misses;
    return (float)rm / (float)(rm + token_hits_total(t) + 1);
}
static inline Token *token_associate_or_unknown(Token *t, Token *const *ctx,
                                                size_t n_ctx, float energy,
                                                TokenPool *pool, float threshold) {
    if (!t) return NULL;
    Token *r = token_associate(t, ctx, n_ctx, energy);
    if (r) { t->recent_misses /= 2; return r; }
    t->misses++; t->recent_misses++;
    if (pool && token_gap(t) >= threshold) return pool_unknowing(pool);
    return NULL;
}

/* ============================ 巩固 / 衰减 ============================ */

static inline int token_entry_cmp_desc(const void *a, const void *b) {
    const AssocEntry *ea = (const AssocEntry *)a, *eb = (const AssocEntry *)b;
    if (ea->hits != eb->hits) return ea->hits < eb->hits ? 1 : -1;
    if (ea->seq  != eb->seq)  return ea->seq  < eb->seq  ? -1 : 1;
    return 0;
}
static inline void token_consolidate(Token *t) {
    if (t && t->count > 1)
        qsort(t->assocs, t->count, sizeof *t->assocs, token_entry_cmp_desc);
}
static inline void token_decay_hits(Token *t, float factor) {
    if (!t) return;
    if (factor < 0) factor = 0;
    if (factor > 1) factor = 1;
    for (size_t i = 0; i < t->count; ++i)
        t->assocs[i].hits = (uint32_t)((float)t->assocs[i].hits * factor);
    t->recent_misses = (uint32_t)((float)t->recent_misses * factor);
}

/* ============================ 观察学习 ============================ */

static inline void token_observe_pair(Token *a, Token *b) {
    if (!a || !b || a == b) return;
    for (size_t k = 0; k < a->count; ++k) {
        AssocEntry *e = &a->assocs[k];
        if (e->main == b && edge_is_plain(e)) {
            token_touch_entry(a, e);
            return;
        }
    }
    if (token_add_assoc_ex(a, b, NULL) == 0)
        token_touch_entry(a, &a->assocs[a->count - 1]);
}
static inline void token_observe(Token *const *seq, size_t n, size_t window) {
    if (!seq || n < 2) return;
    if (window < 2) window = 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        size_t end = i + window;
        if (end > n) end = n;
        for (size_t j = i + 1; j < end; ++j)
            token_observe_pair(seq[i], seq[j]);
    }
}
static inline void token_observe_names(TokenPool *p, const char *const *names,
                                       size_t n, size_t window) {
    if (!p || !names || n < 2) return;
    Token **seq = (Token **)malloc(n * sizeof *seq);
    if (!seq) return;
    for (size_t i = 0; i < n; ++i) seq[i] = pool_token_intern(p, names[i]);
    token_observe(seq, n, window);
    free(seq);
}

/* ============================ 视角树 ============================ */

static inline PerspectiveNode *perspective_new(TokenPool *p,
                                               PerspectiveNode *parent,
                                               Token *who) {
    if (!p) return NULL;
    if (p->perspective_n == p->perspective_cap) {
        size_t ncap = p->perspective_cap ? p->perspective_cap * 2 : 16;
        PerspectiveNode **np = (PerspectiveNode **)realloc(
            p->perspective_pool, ncap * sizeof *np);
        if (!np) return NULL;
        p->perspective_pool = np;
        p->perspective_cap = ncap;
    }
    PerspectiveNode *n = (PerspectiveNode *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->who = who; n->parent = parent;
    n->depth = parent ? parent->depth + 1 : 0;
    n->refcount = 1;
    p->perspective_pool[p->perspective_n++] = n;
    return n;
}
static inline PerspectiveNode *perspective_enter(TokenPool *p, Token *who) {
    if (!p) return NULL;
    PerspectiveNode *n = perspective_new(p, p->perspective_cur, who);
    if (n) p->perspective_cur = n;
    return n;
}
static inline void perspective_exit(TokenPool *p) {
    if (!p || !p->perspective_cur || !p->perspective_cur->parent) return;
    p->perspective_cur = p->perspective_cur->parent;
}
static inline void perspective_goto(TokenPool *p, PerspectiveNode *n) {
    if (p && n) p->perspective_cur = n;
}
static inline size_t perspective_depth(const TokenPool *p) {
    return p && p->perspective_cur ? p->perspective_cur->depth : 0;
}
static inline size_t perspective_path(const TokenPool *p, Token **out, size_t max) {
    if (!p || !p->perspective_cur || !out) return 0;
    size_t d = p->perspective_cur->depth;
    if (d > max) d = max;
    PerspectiveNode *n = p->perspective_cur;
    for (size_t i = 0; i < d && n; ++i) {
        out[d - 1 - i] = n->who;
        n = n->parent;
    }
    return d;
}
static inline Token *pool_current_perspective(const TokenPool *p) {
    return (p && p->perspective_cur) ? p->perspective_cur->who : NULL;
}
static inline void pool_push_perspective(TokenPool *p, Token *who) { perspective_enter(p, who); }
static inline void pool_pop_perspective(TokenPool *p) { perspective_exit(p); }

static inline void vsa_bind_perspective(const TokenPool *p,
                                        const int8_t *content, int8_t *out) {
    memcpy(out, content, VSA_DIM);
    if (!p || !p->perspective_cur) return;
    PerspectiveNode *n = p->perspective_cur;
    while (n && n->who && n->who->hv) {
        vsa_bind(out, n->who->hv, out);
        n = n->parent;
    }
}
static inline void vsa_unbind_perspective(const TokenPool *p,
                                          const int8_t *bound, int8_t *out) {
    memcpy(out, bound, VSA_DIM);
    if (!p || !p->perspective_cur) return;
    Token *path[32];
    size_t d = perspective_path(p, path, 32);
    for (size_t i = 0; i < d; ++i)
        if (path[i] && path[i]->hv) vsa_unbind(out, path[i]->hv, out);
}
static inline void vsa_unbind_to_depth(const TokenPool *p, uint32_t depth,
                                       const int8_t *bound, int8_t *out) {
    memcpy(out, bound, VSA_DIM);
    if (!p || !p->perspective_cur) return;
    PerspectiveNode *n = p->perspective_cur;
    while (n && n->depth > depth && n->who && n->who->hv) {
        vsa_unbind(out, n->who->hv, out);
        n = n->parent;
    }
}

/* ============================ VSA 检索 ============================ */

/* ---- 随机索引（random indexing）嵌入 ----
 *
 * emb(t) = Σ_c PPMI(t,c) · r_c     r_c 是上下文 c 的固定随机 ±1 向量
 *
 * 为什么这样就是「符号神经」：PPMI 是**符号统计**（谁跟谁共现得多），
 * r_c 是**分布式编码**（每个符号一个近乎正交的高维向量），两者相乘再求和，
 * 得到的 emb 里每个维度都没有名字，但余弦保留了 PPMI 空间的相似度
 * （随机投影的 Johnson-Lindenstrauss 性质）。于是「猫」和「狗」在 emb 里
 * 真的会靠近，而它们的名字哈希完全无关。
 *
 * r_c 由 c 的**稳定 id** 生成：同一个 id 永远得到同一个向量，
 * 所以 emb 是 (图 + id) 的确定性函数，可以随时现算，不必存进模型。 */
static inline void emb_randvec(uint32_t id, int8_t *out) {
    uint64_t x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)id * 0xBF58476D1CE4E5B9ULL);
    for (size_t i = 0; i < VSA_DIM; i += 64) {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        for (size_t k = 0; k < 64 && i + k < VSA_DIM; ++k)
            out[i + k] = ((z >> k) & 1) ? 1 : -1;
    }
}

/* emb 的余弦：emb 不是 ±1 而是一般的 int8，必须按模长归一化。
 * （vsa_cosine 只除以维度，那是给 ±1 向量用的，用在这里会算错。） */
static inline float emb_cosine(const int8_t *a, const int8_t *b) {
    if (!a || !b) return 0.0f;
    int64_t dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < VSA_DIM; ++i) {
        dot += (int)a[i] * (int)b[i];
        na  += (int)a[i] * (int)a[i];
        nb  += (int)b[i] * (int)b[i];
    }
    if (na == 0 || nb == 0) return 0.0f;      /* 还没算过 emb */
    double d = sqrt((double)na) * sqrt((double)nb);
    return d > 0 ? (float)((double)dot / d) : 0.0f;
}

/* 按 emb 找最近的词（`::analogy` 用得到）。全 0 的 emb 不参与。 */
static inline Token *pool_query_emb(const TokenPool *p, const int8_t *query,
                                    float threshold, float *out_sim) {
    if (!p || !query) return NULL;
    Token *best = NULL;
    float best_sim = threshold;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t->is_garbage || !t->emb) continue;
        if (t->name && t->name[0] == '[') continue;
        float s = emb_cosine(query, t->emb);
        if (s > best_sim) { best_sim = s; best = t; }
    }
    if (out_sim) *out_sim = best ? best_sim : 0.0f;
    return best;
}

static inline Token *pool_query_vsa(TokenPool *p, const int8_t *query,
                                    float threshold, float *out_sim) {
    if (!p || !query) return NULL;
    Token *best = NULL;
    float best_sim = threshold;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t->is_garbage || !t->hv) continue;
        if (t->name && t->name[0] == '[') continue;
        float s = vsa_cosine(query, t->hv);
        if (s > best_sim) { best_sim = s; best = t; }
    }
    if (out_sim) *out_sim = best ? best_sim : 0.0f;
    return best;
}

/* ============================ 递归激活 ============================ */

typedef void (*TokenVisitFn)(Token *t, size_t depth, float energy, void *user);

struct ActivateCtx {
    Token       **stack;
    size_t        n, cap;
    float         decay, cutoff;
    size_t        max_depth;
    TokenVisitFn  on_visit;
    void         *user;
    Token        *trigger_self;
    AssocEntry   *trigger_entry;
};

static inline void activate_ctx_init(ActivateCtx *a, float decay, float cutoff, size_t max_depth) {
    memset(a, 0, sizeof *a);
    a->decay = decay > 0 ? decay : 0.7f;
    a->cutoff = cutoff > 0 ? cutoff : 0.05f;
    a->max_depth = max_depth ? max_depth : 16;
}
static inline void activate_ctx_free(ActivateCtx *a) {
    if (a) { free(a->stack); a->stack = NULL; a->n = a->cap = 0; }
}
static inline int act_push(ActivateCtx *a, Token *t) {
    if (a->n == a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 8;
        Token **ns = (Token **)realloc(a->stack, ncap * sizeof *ns);
        if (!ns) return -1;
        a->stack = ns; a->cap = ncap;
    }
    a->stack[a->n++] = t;
    return 0;
}
static inline int act_seen(const ActivateCtx *a, const Token *t) {
    for (size_t i = 0; i < a->n; ++i) if (a->stack[i] == t) return 1;
    return 0;
}

static inline void token_activate_action(Token *t, Token *const *ctx,
                                         size_t n_ctx, float energy, size_t depth,
                                         ActivateCtx *a);

static inline void token_activate(Token *t, Token *const *ctx, size_t n_ctx,
                                  float energy, size_t depth, ActivateCtx *a) {
    if (!t || !a) return;
    if (depth > a->max_depth) return;
    if (energy < a->cutoff) return;
    if (act_seen(a, t)) return;
    if (act_push(a, t) != 0) return;
    token_touch(t);
    if (a->on_visit) a->on_visit(t, depth, energy, a->user);
    size_t reach = token_reach(t, energy);
    float next_e = energy * a->decay;
    for (size_t i = 0; i < reach; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        token_touch_entry(t, e);
        if (e->action) {
            Token *ss = a->trigger_self; AssocEntry *se = a->trigger_entry;
            a->trigger_self = t; a->trigger_entry = e;
            token_activate_action(e->action, ctx, n_ctx, next_e, depth + 1, a);
            a->trigger_self = ss; a->trigger_entry = se;
        }
        token_activate(e->main, ctx, n_ctx, next_e, depth + 1, a);
    }
    a->n--;
}
static inline void token_activate_action(Token *t, Token *const *ctx,
                                         size_t n_ctx, float energy, size_t depth,
                                         ActivateCtx *a) {
    if (!t || !a) return;
    if (depth > a->max_depth) return;
    if (energy < a->cutoff) return;
    if (a->on_visit) a->on_visit(t, depth, energy, a->user);
    size_t reach = token_reach(t, energy);
    float next_e = energy * a->decay;
    for (size_t i = 0; i < reach; ++i) {
        AssocEntry *e = &t->assocs[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        token_touch_entry(t, e);
        if (e->action) {
            Token *ss = a->trigger_self; AssocEntry *se = a->trigger_entry;
            a->trigger_self = t; a->trigger_entry = e;
            token_activate_action(e->action, ctx, n_ctx, next_e, depth + 1, a);
            a->trigger_self = ss; a->trigger_entry = se;
        }
        token_activate(e->main, ctx, n_ctx, next_e, depth + 1, a);
    }
}
static inline void token_activate_from(Token *t, Token *const *ctx, size_t n_ctx,
                                       float energy, ActivateCtx *a) {
    token_activate(t, ctx, n_ctx, energy, 0, a);
}

/* ============================ 持久化 ============================ */

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
typedef struct { uint8_t *buf; size_t len, cap; int overflow; } Writer;
typedef struct { const uint8_t *buf; size_t len, pos; int err; } Reader;

static inline void w_bytes(Writer *w, const void *src, size_t n) {
    if (w->buf) {
        if (w->len + n > w->cap) { w->overflow = 1; return; }
        memcpy(w->buf + w->len, src, n);
    }
    w->len += n;
}
static inline void w_u8 (Writer *w, uint8_t v)  { w_bytes(w, &v, 1); }
static inline void w_u32(Writer *w, uint32_t v) { uint8_t b[4]; put_u32(b, v); w_bytes(w, b, 4); }
static inline int r_bytes(Reader *r, void *dst, size_t n) {
    if (r->pos + n > r->len) { r->err = 1; return -1; }
    if (dst) memcpy(dst, r->buf + r->pos, n);
    r->pos += n;
    return 0;
}
static inline uint8_t  r_u8 (Reader *r) { uint8_t b = 0; r_bytes(r, &b, 1); return b; }
static inline uint32_t r_u32(Reader *r) { uint8_t b[4]; if (r_bytes(r, b, 4)) return 0; return get_u32(b); }

/* 旧词池序列化（token_pool_dump / token_pool_load / TOKEN_MAGIC / TOKEN_VERSION）**已删**：
 * 它自 model_io.h 那份存档出现起就被取代（见 model_io.h 文件头：那条路装不下意义指针/派生层），
 * 仓里没有任何调用者，而且它也在写 subs/subs_not 那套已经退役的语境字段 ——
 * 2026-10-07「删历史遗留」时一并删掉（v10）。 */
#endif /* TOKEN_H */
