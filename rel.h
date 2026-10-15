#include <math.h>   /* log()：独占判定用似然比 */

/* ---- 「独占」判定的三个常量 ----
 * 判「一个主语最多一个宾语」用的是**似然比**，不是「多久算多」这种手写阈值：
 *   REL_EXCL_NOISE 独占关系里允许多少噪声（数据里混进一条错的多宾语事实）
 *   REL_EXCL_ODDS  判独占要求的赔率（10 = 10:1 才敢说独占）
 *   REL_EXCL_MIN_SUBJ 少于这个主语数就不表态（保留 AI_RELS 里的声明值）
 * 这样样本少的时候自然判不出来，样本够且一边倒的时候才改口 ——
 * 而且改口方向可以是**推翻声明**（数据说「属于」多值，就按数据来）。
 * 证据与似然比留在 ai->rel_excl_single/multi/llr，`::excl` 全部打出来。 */
#define REL_EXCL_NOISE     0.10
#define REL_EXCL_ODDS      10.0
#define REL_EXCL_MIN_SUBJ  2

/* rel.h — 语义关系层（L2）：完整逻辑链
 *
 * 上一版只有「传递」和「继承」两条规则，并且靠**闭合世界假设**硬答「不对」——
 * 那是**不健全**的推理：「知识库里没有」和「事实为假」是两件事。
 * 这一版补成一套能站住的推理：
 *
 *   ── 真值三态 ──────────────────────────────────────────────
 *     +1 真   —— 推得出肯定结论（直接 / 传递 / 继承 / 对称 / 逆关系）
 *     -1 假   —— 推得出**否定**结论（显式否定事实 `猫 不 会 飞`，或继承下来的否定）
 *      0 未知 —— 什么都没有。这时**不许说「不对」**，只能说不知道，
 *                或者给一个**明确标注为推测**的类比答案。
 *
 *   ── 规则（写全了，就这五条）──────────────────────────────
 *   R0 直接   A --R--> B
 *   R1 传递   A R C, C R B        （R 声明 trans）
 *   R2 继承   A 属于 C, C R B      （R 声明 inherit；否定也能继承）
 *   R3 对称   B R A                （R 声明 sym）
 *   R4 逆关系 B inv(R) A           （R 声明 inv）
 *   R5 类比   A 与 S 很像，S R B   （**只在前四条都失败时**试，结论标为推测）
 *
 *   R5 是这一版才有的：它把 L3 学出来的 `emb` 和符号规则接上了 ——
 *   知识库里查不到时，找「最像的、并且知道这件事」的那个词借答案。
 *   这是非单调的默认推理，所以 pf->guessed 会被置上，答复里必须写明是猜的。
 *
 *   ── 谓词表是开放的 ────────────────────────────────────────
 *   清单之外谓语（「想念」「养」…）会登记成**动态关系**（id 从 8 起，写进模型），
 *   所以系统不被 7 个动词框死。动态关系只支持肯定，不支持否定与传递 ——
 *   老实记在文档的局限里。
 *
 * 依赖：ai.h（AI / Token / emb_cosine / cot_push）与 frame.h 的 fr_is_func。
 */
#ifndef REL_H
#define REL_H

/* 注意：**不**在这里 include ai.h。本文件只由 ai.h 在末尾包含，
 * 而 ai.h → rel.h → derive.h 的顺序是刻意的（derive 要用 rel 的 Proof）。
 * 早先 rel.h 里写了 #include "ai.h"，形成一个环：编译 rel.h 时会绕回 ai.h，
 * 于是 derive.h 在 Proof 定义之前就被拉进来了（unknown type name 'Proof'）。 */
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 关系清单 */

/* 关系 id 常量与 REL_BIT/REL_BOOST 都定义在 **ai.h** 里 ——
 * AI 结构要用它们给运行时的独占表开数组，所以必须更早可见。 */

typedef struct {
    const char *name;    /* 关系词（用户可能这样写） */
    const char *neg;     /* 否定形式，答复时用 */
    uint8_t     id;      /* 关系 id；同义写法共用 id */
    uint8_t     trans;   /* 可传递：A R C, C R B ⟹ A R B */
    uint8_t     inherit; /* 可沿「属于」继承：A 属于 C, C R B ⟹ A R B */
    uint8_t     sym;     /* 对称：A R B ⟺ B R A */
    uint8_t     inv;     /* 逆关系 id（0 = 无）：B inv A ⟹ A R B */
    /* 独占（函数式）：一个主语只能有一个宾语。
     * 有了它才能**健全地**推出否定：「猫 属于 动物」+「动物/鸟 互不包含」
     * ⟹ 「猫 不属于 鸟」。这是「完备性」那条局限的正面解法 ——
     * 不是靠「知识库里没有就算假」（那是闭合世界，是一个**假设**），
     * 而是靠一条**声明出来的公理**。 */
    uint8_t     excl;
    /* 「做这件事需要前提」：R9 缺前提 → 否定能力 用的就是这一位。
     * 声明在**表里**而不是在求解器里写死 `rid == REL_CAN` ——
     * 哪些关系有前提是**知识**，不是控制流。实测只有「会」有这回事。
     * 注意字段顺序要和表的列序一致（…逆、独占、前提）：
     * 顺序错了不会编译报错，只会让值悄悄串位（这个坑刚踩过）。 */
    uint8_t     prereq;
} RelDef;

static const RelDef AI_RELS[] = {
    /* 名字    否定         id           传 继 对 逆          独占 前提 */
    { "属于", "不 属于",   REL_IS_A,     1, 0, 0, REL_CONTAIN, 1, 0 },
    { "是",   "不 是",     REL_IS_A,     1, 0, 0, REL_CONTAIN, 1, 0 },
    { "包含", "不 包含",   REL_CONTAIN,  0, 0, 0, REL_IS_A,    0, 0 },
    { "有",   "没有",      REL_HAS,      0, 1, 0, 0,          0, 0 },
    { "会",   "不 会",     REL_CAN,      0, 1, 0, 0,          0, 1 },
    { "怕",   "不 怕",     REL_FEAR,     0, 0, 1, 0,          0, 0 },
    { "喜欢", "不 喜欢",   REL_LIKE,     0, 0, 0, 0,          0, 0 },
    { "爱",   "不 爱",     REL_LIKE,     0, 0, 0, 0,          0, 0 },
    { "吃",   "不 吃",     REL_EAT,      0, 0, 0, 0,          0, 0 },
    { "在",   "不 在",     REL_AT,       0, 0, 0, 0,          1, 0 },
    /* 「需要」= 前提。它是 R9「缺前提 → 否定能力」用的关系：
     *   唱歌 需要 声带（前提），猫 没有 灵活的声带 ⟹ 猫 不 会 唱歌。
     * 声明成不可传递、不继承、不对称、无逆、不独占（一件事可以有很多前提）。 */
    { "需要", "不 需要",   REL_NEED,     0, 0, 0, 0,          0, 0 },
    { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static inline const RelDef *rel_by_name(const char *w) {
    if (!w) return NULL;
    for (size_t i = 0; AI_RELS[i].name; ++i)
        if (strcmp(AI_RELS[i].name, w) == 0) return &AI_RELS[i];
    return NULL;
}
static inline const RelDef *rel_by_id(uint8_t id) {
    for (size_t i = 0; AI_RELS[i].name; ++i)
        if (AI_RELS[i].id == id) return &AI_RELS[i];
    return NULL;
}

/* ------------------------------------------------- 动态关系（开放谓词表） */

/* 清单之外的谓语登记成动态关系，id 从 8 起。返回 id（0 = 登记不了）。 */
static inline uint8_t rel_dyn_intern(AI *ai, const char *w) {
    if (!ai || !w || !*w) return 0;
    for (size_t i = 0; i < ai->dynrel_n; ++i)
        if (ai->dynrel_name[i] && strcmp(ai->dynrel_name[i], w) == 0)
            return (uint8_t)(REL_DYN_BASE + i);
    if (ai->dynrel_n >= AI_MAX_DYNREL) return 0;
    size_t n = strlen(w) + 1;
    char *c = (char *)malloc(n);
    if (!c) return 0;
    memcpy(c, w, n);
    ai->dynrel_name[ai->dynrel_n] = c;
    ai->dynrel_n++;
    return (uint8_t)(REL_DYN_BASE + ai->dynrel_n - 1);
}
static inline const char *rel_dyn_name(const AI *ai, uint8_t id) {
    if (!ai || id < REL_DYN_BASE) return NULL;
    size_t i = (size_t)(id - REL_DYN_BASE);
    if (i >= ai->dynrel_n) return NULL;
    return ai->dynrel_name[i];
}
/* 这个谓语认识吗？
 *
 * allow_new=1 才**登记**新的动态关系；allow_new=0 只查。
 * 这个参数是必须的：解析器如果在「只是问一句」的时候也去登记，
 * 那么随便一句 `你 觉得 天气 怎么样` 里的「觉得」都会被登记成动态关系，
 * 表被垃圾填满、还写进模型（实测的泄漏）。只有**用户明确在陈述事实**
 * （教它一条新关系）时才该登记。 */
/* **谓词别名表**（一处）：常见的同义/近义说法归到**核心关系**上。
 *
 * 为什么要它：`我 害怕 猫` 与 `我 怕 猫` 说的是同一件事，但前者会去登记一个
 * **动态关系** → 和后者永远对不上（"他说的"和"我知道的"也一样对不上）。
 * 纪律：**只收语义确实一致的**，拿不准的一律不写（宁可对不上，也不要错并）。
 * `爱` 已经在核心表里（= 喜欢），所以不列。 */
typedef struct { const char *word; uint8_t rid; } RelAlias;

static const RelAlias REL_ALIASES[] = {
    /* ⚠ 有意**没有**收 `害怕`（尽管语料里 30 次、语义就是「怕」）：
     *   `frames.txt` 里有一条 `[帧:害怕X吗]`（问法就是「你 害怕 <X> 吗」），
     *   把「害怕」归到核心关系「怕」之后，**关系层会先答**，帧层那条兜底就再也用不上了
     *   —— 实测被 `tests/hypo_frames.sh` 当场抓住（假设下的帧/检索路径不再被走到）。
     *   这是"归属该给谁"的设计裁量，不是能顺手改的事：先按现状（帧层 owns 害怕），
     *   把冲突记在这里与 docs/对照-lingshu与五个局限.md 里。 */
    /* ⚠ 别名**只对分词器不切开的词有效**：多字词必须先是词表里的 token，
     *   否则会被切成单字（实测「喜爱」被切成「喜」+「爱」→ 别名永远看不到）。
     *   所以下面几行现在只有 `拥有` 是**真的可达**的（它是模型里的 token），
     *   其余先留着 —— 等它们进了词表就自动生效，不用再改代码。 */
    { "惧怕", REL_FEAR },
    { "喜爱", REL_LIKE },
    { "喜好", REL_LIKE },
    { "拥有", REL_HAS },    /* 模型里的 token（实测 3 条边）—— 当前唯一可达的别名 */
    { "具有", REL_HAS },
    { NULL, 0 }
};

static inline uint8_t rel_alias_id(const char *w) {
    if (!w) return 0;
    for (size_t i = 0; REL_ALIASES[i].word; ++i)
        if (!strcmp(REL_ALIASES[i].word, w)) return REL_ALIASES[i].rid;
    return 0;
}

static inline uint8_t rel_id_of(AI *ai, const char *w, int allow_new) {
    const RelDef *rd = rel_by_name(w);
    if (rd) return rd->id;
    uint8_t al = rel_alias_id(w);      /* 别名 → 核心关系（**唯一**一张表） */
    if (al) return al;
    if (!allow_new || !ai) return 0;
    return rel_dyn_intern(ai, w);
}
static inline const char *rel_name_of(const AI *ai, uint8_t id) {
    const RelDef *rd = rel_by_id(id);
    if (rd) return rd->name;
    return rel_dyn_name(ai, id);
}
static inline const char *rel_neg_of(const AI *ai, uint8_t id) {
    (void)ai;
    const RelDef *rd = rel_by_id(id);
    if (rd) return rd->neg;
    return "不";      /* 动态关系没有专门的否定形式，用「不」兜 */
}


/* 否定形式的**两种写法**（同一个判据，两处调用）。
 *
 * 关系表里声明的是一种写法（`不 会`），可**实际输入**可能是另一种：分词器把最常见的
 * 「不会」切成**一个** token（`不喜欢`/`不是` 同理），而 `不属于`/`不能`/`不敢` 却是两词。
 * 于是同一份声明得两种都认 —— 判据只写这一处：**把声明里的空格去掉再比**。
 *
 * ⚠ 这不是新语义：声明还是那一条（`AI_RELS[]` 里只有一份），这里只是"同一个否定词的两种切法"。
 * 返回 1 = `w` 就是某个关系的**合写否定形式**，并把关系 id 写进 `*out`。
 *
 * 为什么非修不可（2026-10-10 实测，audit §6.68 三 / §6.69）：修之前
 *   `猫 不会 飞 吗` / `甲 说 猫 不会 游泳` / `我 不喜欢 猫 吗` 全掉进**降级答复**
 *   （"这个我 不 太 懂"），而两词写法「不 会」一切正常 —— 同一个意思两种写法两种待遇。 */
/* **情态说法的归一**（第 6 条尾巴：合写否定修好之后，`不能/没法` 这类还掉降级答复）。
 *
 * `不能` 不是任何关系的"否定形式"，而是**情态写法**（能不能 ≈ 会不会做得到）。
 * 所以它不走 `rel_merged_neg_id`（那是"同一条声明的两种切法"），走这张**小的说法表**：
 * 只把最常用的几个情态说法归到 `(关系, 否定)` 上 —— 表是**数据**，加一行就多认一个说法。
 * ⚠ 归一是"认说法"，不是"新语义"：判据仍然是那条关系边。 */
typedef struct { const char *w; const char *rel; int neg; } RelModalRow;
static const RelModalRow REL_MODALS[] = {
    { "不能",  "会",   1 },
    { "没法",  "会",   1 },
    { "不会",  "会",   1 },     /* 合写（rel_merged_neg_id 已经认，这里只是让表自洽） */
    { "不敢",  "怕",   0 },     /* "不敢"更接近"怕"：先按**肯定**的怕收（宁可弱，不乱否） */
    { NULL, NULL, 0 }
};
static inline const RelModalRow *rel_modal_of(const char *w) {
    if (!w) return NULL;
    for (size_t i = 0; REL_MODALS[i].w; ++i)
        if (strcmp(REL_MODALS[i].w, w) == 0) return &REL_MODALS[i];
    return NULL;
}

static inline int rel_merged_neg_id(const char *w, uint8_t *out) {
    if (out) *out = 0;
    if (!w || !w[0]) return 0;
    char sq[24];
    for (size_t i = 0; AI_RELS[i].name; ++i) {
        const char *neg = AI_RELS[i].neg;
        if (!neg) continue;
        size_t k = 0;
        for (const char *q = neg; *q && k + 1 < sizeof sq; ++q)
            if (*q != ' ') sq[k++] = *q;
        sq[k] = 0;
        if (k && strcmp(w, sq) == 0) {
            if (out) *out = AI_RELS[i].id;
            return 1;
        }
    }
    return 0;
}

/* 把关系清单里的词**先 intern 进词池**。
 *
 * 不这么做的话，`包含` 这种词从没作为 Token 存在过（AI_RELS 只是编译期表），
 * 切词器就会把 `动物 包含 猫 吗` 切成 动物/包/含/猫/吗 —— 于是关系解析
 * 在第 1 个词位上看到「包」，整句都认不出来（实测）。
 * 让它们成为真词，切词/帧/检索全都会顺带受益。 */
static inline void rel_names_intern(AI *ai) {
    if (!ai || !ai->pool) return;
    for (size_t i = 0; AI_RELS[i].name; ++i)
        pool_token_intern(ai->pool, AI_RELS[i].name);
}

/* ---------------------------------------------------------------- 静态知识 */

/* 存盘里是不是已经有一条**观测**说了反话（缓存不算）？
 * 用在 relations_load 上：模型文件是**更晚**的状态（用户教过、存过盘），
 * relations.txt 是每次启动重读的基准知识 —— 拿基准去改用户教过的，等于
 * 「重启就把用户教的忘了」。所以装载时看到反面的**观测**就跳过这一行。
 * 改名/挪位置注意：rel_direct_* 这几个直查函数定义在下面，所以这里直接扫位。 */
static inline int rel_obs_opposite(const Token *a, uint8_t rid, const Token *b, int neg) {
    if (!a || !b || rid == 0 || rid >= REL_DYN_BASE) return 0;
    uint16_t bit = REL_BIT(rid);
    for (size_t i = 0; i < a->count; ++i) {
        const AssocEntry *e = &a->assocs[i];
        if (e->main != b || edge_is_causal_guess(e)) continue;
        if (e->role == ASSOC_ROLE_DERIVED) continue;      /* 缓存不是观测，该被推翻 */
        if (neg ? (e->rel & bit) : (e->nrel & bit)) return 1;
    }
    return 0;
}

/* 一行一条事实：
 *   <主语> <关系词> <宾语>          肯定
 *   <主语> 不 <关系词> <宾语>        否定（三态里的「假」）
 * '#' 开头是注释。返回读到的事实条数。 */
static inline size_t relations_load(AI *ai, const char *path) {
    if (!ai || !path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    size_t learned = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *w[4] = {NULL, NULL, NULL, NULL};
        size_t nw = 0;
        while (*p && nw < 4) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            w[nw++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
        int neg = 0;
        const char *relw = NULL, *sub = NULL, *obj = NULL;
        if (nw == 3) { sub = w[0]; relw = w[1]; obj = w[2]; }
        else if (nw == 4 && strcmp(w[1], "不") == 0) {
            neg = 1; sub = w[0]; relw = w[2]; obj = w[3];
        } else continue;
        /* 「没有」是「有」的否定形式，而且它是一个词（不是「不 有」两段）——
         * 不在这里认它，「猫 没有 声带」这种前提事实就整行被丢掉，
         * 缺前提推理(R9)永远缺一半前提。 */
        int norel = 0;
        if (!neg && (strcmp(relw, "没有") == 0 || strcmp(relw, "没") == 0)) {
            relw = "有"; neg = 1; norel = 1;
        }
        (void)norel;
        /* 合写否定（`不会`/`不喜欢`/`不是`）**必须先查**：`rel_id_of(..., allow_new=1)`
         * 会把「不会」当场登记成一个**动态谓语**（实测：那样边是建了，可它是动态关系、
         * 不是「会」的否定边 ⇒ 断言「不 会」查不到）。与 `rel_parse` 同一份判据。 */
        uint8_t rid = 0;
        {
            uint8_t mid = 0;
            if (!neg && rel_merged_neg_id(relw, &mid)) { rid = mid; neg = 1; }
            else rid = rel_id_of(ai, relw, 1);   /* 装载知识：允许登记新谓语 */
        }
        if (!rid) continue;
        Token *a = pool_token_intern(ai->pool, sub);
        Token *b = pool_token_intern(ai->pool, obj);
        if (!a || !b) continue;
        if (rid >= REL_DYN_BASE) {                   /* 动态关系：否定走 `nrel` 第 15 位（§6.80） */
            if (token_tag_dyn_neg(a, b, (uint8_t)(rid - REL_DYN_BASE + 1), neg,
                                      REL_BOOST) == 0) learned++;
            continue;
        }
        /* 存盘里已有观测说了反话 → 这条基准知识让路（别把用户教过的盖回默认值）。 */
        if (rel_obs_opposite(a, rid, b, neg)) continue;
        int rc = neg ? token_tag_nrel(a, b, rid, REL_BOOST)
                     : token_tag_rel(a, b, rid, REL_BOOST);
        if (rc == 0) learned++;
        /* **来源监控**：这一条是**显式教给它的基准知识**（不是它自己泡出来的经验）。
         * 每次装载都重打一遍，所以是确定性的；存档格式没动（role 一直是序列化字节）。 */
        token_mark_innate(a, b);
    }
    fclose(f);
    return learned;
}

/* ------------------------------------------------- 独占：从数据里量出来 */
/*
 * 「独占」（函数式：一个主语只能有一个宾语）原来是我手写进 AI_RELS 的 ——
 * 那是**声明**，不是知识。这里改成从图里量：
 *
 *   对每个核心关系 r，数一数有多少主语「有 ≥2 个不同的宾语」。
 *   如果绝大多数主语只有一个宾语（≥80%），就判定 r 独占。
 *
 * 只在样本够（≥5 个主语）时才改判定，样本不足就保留声明值 ——
 * 数据少的时候乱下结论比不结论更糟。
 *
 * 这一步让「独占公理」变成**经验结论**，同时也把「哪些关系能用来推否定」
 * 这件事和知识库本身对齐了。
 */
static inline void rel_excl_build(AI *ai) {
    if (!ai || !ai->pool) return;
    TokenPool *p = ai->pool;
    size_t single[REL_TABLE_MAX + 1] = {0};
    size_t multi[REL_TABLE_MAX + 1] = {0};
    size_t learned = 0;

    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t->name || t->name[0] == '[') continue;
        /* 每个关系分别数：这个主语有几个不同宾语 */
        for (uint8_t r = 1; r <= REL_CORE_MAX; ++r) {
            uint8_t bit = REL_BIT(r);
            const Token *obj[64];
            size_t no = 0;
            for (size_t j = 0; j < t->count && no < 64; ++j) {
                const AssocEntry *e = &t->assocs[j];
                if (!(e->rel & bit) || !e->main || e->main == t) continue;
                /* **统计只看观测**：独占/多值是从**数据**里量出来的，
                 * 而推理沉淀下来的缓存是自己推的 —— 拿它当数据会自我印证
                 * （推出来的结论又会强化推出它的那条规则）。 */
                if (e->role == ASSOC_ROLE_DERIVED) continue;
                int dup = 0;
                for (size_t q = 0; q < no; ++q) if (obj[q] == e->main) { dup = 1; break; }
                if (!dup) obj[no++] = e->main;
            }
            if (no == 0) continue;
            if (no == 1) single[r]++;
            else multi[r]++;
        }
    }
    /* 参照类：**看清一楚「不是独占」的关系长什么样**。
     * 把所有关系一股脑合并起来算「多宾语率」是不对的 —— 数据集里
     * 「属于/在」本身就是函数式的，它们会把基准拉低，于是「17 个主语都只有一个宾语」
     * 反而不够显著，独占的判定会被自己拖垮（实测「在」被误判成多值）。
     * 所以参照类只取**观察到了多宾语行为**的那些关系：它们才是「非独占」的样本。
     * 一条都没有（数据太小）就退回中性的 50%。 */
    size_t ref_s = 0, ref_m = 0;
    for (uint8_t r = 1; r <= REL_CORE_MAX; ++r) {
        if (multi[r] == 0) continue;
        ref_s += single[r]; ref_m += multi[r];
    }
    double p1 = (ref_s + ref_m) ? (double)ref_m / (double)(ref_s + ref_m) : 0.5;
    if (p1 < 0.05) p1 = 0.05;
    if (p1 > 0.95) p1 = 0.95;
    ai->rel_excl_base = (uint32_t)(p1 * 1000.0 + 0.5);

    size_t evidenced = 0;
    for (uint8_t r = 1; r <= REL_CORE_MAX; ++r) {
        size_t tot = single[r] + multi[r];
        ai->rel_excl_single[r] = single[r];
        ai->rel_excl_multi [r] = multi[r];
        ai->rel_excl_llr[r] = 0.0f;
        if (tot < REL_EXCL_MIN_SUBJ) continue;       /* 样本不够 → 不表态 */
        /* 两个假设的对数似然：
         *   H_multi「多值」：每个主语独立地以 p1 的概率有多个宾语（参照类给的）
         *   H_excl 「独占」：以 p0 的概率有多个宾语（p0 是**噪声容忍**，不是先验）
         * llr = ll_multi - ll_excl：>0 更像「多值」，<0 更像「独占」。 */
        double ll_m = (multi[r] ? multi[r] * log(p1) : 0.0)
                    + (double)(tot - multi[r]) * log(1.0 - p1);
        double ll_e = (multi[r] ? multi[r] * log(REL_EXCL_NOISE) : 0.0)
                    + (double)(tot - multi[r]) * log(1.0 - REL_EXCL_NOISE);
        double llr = ll_m - ll_e;
        ai->rel_excl_llr[r] = (float)llr;
        const double bar = log(REL_EXCL_ODDS);
        if (llr <= -bar) {
            /* 数据支持「独占」。声明值是 1 时这叫**印证**，是 0 时这才叫**改口**。 */
            evidenced++;
            if (!ai->rel_excl[r]) { ai->rel_excl[r] = 1; learned++; }
        } else if (llr >= bar) {
            /* 数据支持「多值」—— 方向是**推翻声明**也照推。 */
            evidenced++;
            if (ai->rel_excl[r]) { ai->rel_excl[r] = 0; learned++; }
        }
        /* 证据不够：保留 AI_RELS 里的声明值（那是先验，不是结论） */
    }
    ai->n_rel_excl_evidenced = evidenced;
    ai->n_rel_excl_learned = learned;
}

/* ---------------------------------------------------------------- 推理 */

/* **复核模式**：置 1 时，求解器把「推出来沉淀的」边**当成不存在**。
 * 只给一件事用 —— 复核一条缓存现在还有没有依据（见 rel_cache_supported）：
 * 把缓存本身排除在外再推一遍，推得出来才算有依据。
 * 用全局开关而不是给每个递归调用加参数：求解器有一串递归（传递/继承/对称/逆），
 * 加参数要改十几处调用点，而这个模式只在单线程、非嵌套复核时用得上
 * （和 ai.h 的 g_walk_trace 是同一类东西）。 */
static int g_rel_ignore_derived = 0;
/* §6.125 「**允许把记忆里的猜测当输入**」的趟（默认 0 = 不许）。
 * 只在两处打开：① 答复层"先按记忆解一遍"；② 验证器复核含记忆步的链时**独立重推**。
 * 复核模式（g_rel_ignore_derived=1）下它一律无效 —— 猜测永远不算"干净依据"。 */
static int g_rel_guess_visible = 0;

/* 「把缓存排除在外再推一遍」——定义在下面（rel_solve 之后），这里先声明：
 * ① R0 要拿它当实时闸；② 候选枚举（rel_next_cands）也要用它，
 * 不然「经由中间词」会拿一条**没依据的缓存**当第一跳。 */
static inline int rel_solve_ignoring_caches(AI *ai, const Token *a, uint8_t rid,
                                            const Token *b, int want_neg);

#define REL_MAX_DEPTH 5
#define REL_MAX_PROOF 16
#define REL_GUESS_SIM 40            /* 类比推测的相似度门槛（百分数） */

typedef struct {
    Token  *tok;      /* 走到哪个词 */
    uint8_t rel;      /* 从上一个词走到这里用的关系 id（起点为 0） */
    uint8_t neg;      /* 这一步是不是否定 */
    uint8_t via;      /* 0 直接 1 传递 2 继承 3 对称 4 逆 5 类比推测 */
    uint8_t hyp;      /* 这一跳的边是**从假设层**来的（不在图里） */
    /* 这一跳的边是**以前推出来沉淀下来的**（role == ASSOC_ROLE_DERIVED）。
     * 它和「观测到的」要分开：证据强度更低，而且被观测推翻时可以撤销。 */
    uint8_t known;
    int     sim;      /* via==5 时的相似度 */
} ProofStep;

typedef struct {
    ProofStep step[REL_MAX_PROOF];
    size_t    n;
    int       status;     /* +1 真  -1 假  0 未知 */
    int       guessed;    /* 结论里有没有用到类比推测 */
} Proof;

static inline const char *rel_via_name(uint8_t via) {
    switch (via) {
        case 0: return "直接事实";
        case 1: return "传递";
        case 2: return "继承";
        case 3: return "对称";
        case 4: return "逆关系";
        case 5: return "类比推测";
        case 6: return "假设";
        case 7: return "独占";
        case 8: return "闭合世界";
        case 9: return "缺前提";   /* R9：做这件事的前提它没有 ⟹ 它做不到 */
        case 10: return "前提";    /* R10：经前提（前提成立才走这条连结） */
        case 11: return "前提不";  /* R10b：反前提（前提不在才走） */
        default: return "?";
    }
}

static inline void proof_push(Proof *pf, Token *t, uint8_t rel, uint8_t neg,
                              uint8_t via, int sim) {
    if (!pf || pf->n >= REL_MAX_PROOF) return;
    ProofStep *s = &pf->step[pf->n++];
    /* **每个字段都要写**：hyp 漏了不写就会留着上一次求解的残值 ——
     * 实测症状是「撤回假设之后再问，那一步被误标成『靠假设』，
     * 验证器正确地拒绝（假设里没有它），于是整条问答退化成帧答复」。
     * 和当年 role 字节没初始化是同一类 bug。 */
    s->tok = t; s->rel = rel; s->neg = neg; s->via = via; s->sim = sim;
    s->hyp = 0;
    s->known = 0;      /* 每个字段都要写（漏了就会留着上一次求解的残值） */
}

/* 带假设层的事实查询。*from_hypo 填 1 表示这条是从**假设**里来的。
 * 求解器一律走这个版本 —— 这就是「在假设世界里推理」的全部秘密：
 * 查询时多看一眼覆盖层，图本身一动不动。 */
static inline int rel_direct_h(const AI *ai, const Token *a, uint8_t rid, const Token *b,
                               int *from_hypo) {
    const HypoFact *hf = ai ? hypo_find(&ai->hypo, ai->view_who, a, rid, b, NULL) : NULL;
    if (hf && !hf->neg) { if (from_hypo) *from_hypo = 1; return 1; }
    return 0;
}
static inline int rel_direct_neg_h(const AI *ai, const Token *a, uint8_t rid, const Token *b,
                                   int *from_hypo) {
    const HypoFact *hf = ai ? hypo_find(&ai->hypo, ai->view_who, a, rid, b, NULL) : NULL;
    if (hf && hf->neg) { if (from_hypo) *from_hypo = 1; return 1; }
    return 0;
}

/* 直查，并报告这条边**是不是推出来沉淀的**（*derived 可以是 NULL）。
 * 语义和 rel_direct 完全一样，只是多带一个「来源」出来 ——
 * 求解器要靠它把「观测」和「缓存」分开（证据强度与可撤销性都不同）。 */
static inline int rel_direct_as(const Token *a, uint8_t rid, const Token *b, int *derived) {
    if (derived) *derived = 0;
    if (!a || !b || rid == 0) return 0;
    if (rid >= REL_DYN_BASE) {            /* 动态关系走自己的那套（§6.80：否定旗子在 nrel 15 位） */
        uint8_t d = (uint8_t)(rid - REL_DYN_BASE + 1);
        for (size_t i = 0; i < a->count; ++i) {
            const AssocEntry *e = &a->assocs[i];
            if (e->main != b || e->dyn != d || edge_is_causal_guess(e)) continue;
            if (edge_dyn_neg(e)) continue;         /* 要的是**肯定**那条 */
            if (derived) *derived = edge_is_guess(e) ? 2
                                                     : (e->role == ASSOC_ROLE_DERIVED);
            return 1;
        }
        return 0;
    }
    uint16_t bit = (uint16_t)(1u << (rid - 1));
    for (size_t i = 0; i < a->count; ++i) {
        const AssocEntry *e = &a->assocs[i];
        if (e->main != b || edge_is_causal_guess(e)) continue;
        if (e->rel & bit) {
            if (derived) *derived = edge_is_guess(e) ? 2
                                                     : (e->role == ASSOC_ROLE_DERIVED);
            return 1;
        }
    }
    return 0;
}
static inline int rel_direct_neg_as(const Token *a, uint8_t rid, const Token *b, int *derived) {
    if (derived) *derived = 0;
    if (!a || !b || rid == 0) return 0;
    if (rid >= REL_DYN_BASE) {            /* 同上：动态关系的否定看 15 位 */
        uint8_t d = (uint8_t)(rid - REL_DYN_BASE + 1);
        for (size_t i = 0; i < a->count; ++i) {
            const AssocEntry *e = &a->assocs[i];
            if (e->main != b || e->dyn != d || edge_is_causal_guess(e)) continue;
            if (!edge_dyn_neg(e)) continue;        /* 要的是**否定**那条 */
            if (derived) *derived = edge_is_guess(e) ? 2
                                                     : (e->role == ASSOC_ROLE_DERIVED);
            return 1;
        }
        return 0;
    }
    uint16_t bit = (uint16_t)(1u << (rid - 1));
    for (size_t i = 0; i < a->count; ++i) {
        const AssocEntry *e = &a->assocs[i];
        if (e->main != b || edge_is_causal_guess(e)) continue;
        if (e->nrel & bit) {
            if (derived) *derived = edge_is_guess(e) ? 2
                                                     : (e->role == ASSOC_ROLE_DERIVED);
            return 1;
        }
    }
    return 0;
}

static inline int rel_direct(const Token *a, uint8_t rid, const Token *b) {
    if (!a || !b || rid == 0) return 0;
    if (rid >= REL_DYN_BASE) {                      /* 动态关系 */
        uint8_t d = (uint8_t)(rid - REL_DYN_BASE + 1);
        for (size_t i = 0; i < a->count; ++i)
            if (a->assocs[i].main == b && a->assocs[i].dyn == d &&
                !edge_is_guess(&a->assocs[i])) return 1;
        return 0;
    }
    uint16_t bit = REL_BIT(rid);
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].main == b && !edge_is_guess(&a->assocs[i]) &&
            (a->assocs[i].rel & bit)) return 1;      /* §6.125 猜测记忆**不算知识** */
    return 0;
}

static inline int rel_direct_neg(const Token *a, uint8_t rid, const Token *b) {
    if (!a || !b || rid == 0 || rid >= REL_DYN_BASE) return 0;  /* 否定只支持核心关系 */
    uint16_t bit = REL_BIT(rid);
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].main == b && !edge_is_guess(&a->assocs[i]) &&
            (a->assocs[i].nrel & bit)) return 1;     /* §6.125 同上 */
    return 0;
}

/* §6.125 「这条边是**记忆里的猜测**吗」（供可见性那一趟用；`derived == 2` 是记号的来源） */
static inline int rel_direct_guess(const Token *a, uint8_t rid, const Token *b, int want_neg) {
    int dv = 0;
    int r = want_neg ? rel_direct_neg_as(a, rid, b, &dv) : rel_direct_as(a, rid, b, &dv);
    return r && dv == 2;
}

/* 在最前面插一个起点 t，并把「走到第二个点用的是哪条规则」标在那一步上。
 * 规则标签属于**到达的那一步**（step[i].via = 从 step[i-1] 到 step[i] 用了什么规则），
 * 挂在起点上是没意义的 —— 第一次写就挂错了，链上显示成了「直接事实」。 */
/* 枚举「以 a 为主语、关系为 rid 的下一步候选」：**图里的边 + 假设层里的边**。
 *
 * 这一步是反事实推理能不能往外推的关键。第一版只在图里搜，
 * 于是「如果 猫 属于 鸟」推不出「猫 会 飞」—— 因为假设那条边**根本不在图里**
 * （这正是我们要的：假设不进图），图里的循环自然看不到它。
 * 所以搜索也必须过一遍覆盖层。 */
static inline size_t rel_next_cands(const AI *ai, const Token *a, uint8_t rid,
                                    int neg_want, Token **out, uint8_t *from_hyp,
                                    size_t cap) {
    size_t n = 0;
    if (!a || !out) return 0;
    if (from_hyp) for (size_t i = 0; i < cap; ++i) from_hyp[i] = 0;
    /* 边界用 REL_DYN_BASE，不用 REL_CONTAIN —— 「包含」的 id 正好是 8，
     * 写成 `rid < REL_CONTAIN` 会把它错当成动态关系，永远查不到边（踩过）。 */
    if (rid < REL_DYN_BASE) {
        uint16_t bit = REL_BIT(rid);
        for (size_t i = 0; i < a->count && n < cap; ++i) {
            const AssocEntry *e = &a->assocs[i];
            if (!e->main || e->main == a) continue;
            /* 缓存当中间跳要**先有依据**：没依据的缓存不算路（和 R0 同一道闸） */
            if (e->role == ASSOC_ROLE_DERIVED) {
                if (g_rel_ignore_derived) continue;
                /* 求解只读图（不改结构），这里去掉 const 只是为了复用同一个入口 */
                if (!rel_solve_ignoring_caches((AI *)ai, a, rid, e->main, neg_want)) continue;
            }
            if (neg_want ? (e->nrel & bit) : (e->rel & bit)) out[n++] = e->main;
        }
    } else {
        uint8_t d = (uint8_t)(rid - REL_DYN_BASE + 1);
        for (size_t i = 0; i < a->count && n < cap; ++i) {
            const AssocEntry *e = &a->assocs[i];
            if (!e->main || e->main == a || e->dyn != d) continue;
            if (neg_want ? !edge_dyn_neg(e) : edge_dyn_neg(e)) continue;   /* §6.80：否定也算一档 */
            out[n++] = e->main;
        }
    }
    if (ai) {
        const HypoFact *hf[AI_MAX_HYPO * AI_MAX_HYPO_DEPTH];
        size_t nh = hypo_edges(&ai->hypo, ai->view_who, a, rid, neg_want, hf,
                               sizeof hf / sizeof hf[0]);
        for (size_t i = 0; i < nh && n < cap; ++i) {
            if (hf[i]->b == a) continue;
            int dup = 0;
            for (size_t q = 0; q < n; ++q) if (out[q] == hf[i]->b) { dup = 1; break; }
            if (!dup) { out[n] = hf[i]->b; if (from_hyp) from_hyp[n] = 1; n++; }
        }
    }
    return n;
}

/* ---- 「假设也要进检索/帧层」：冲突检测 ----
 *
 * 以前假设只做到「不被代偿」：关系层被叫停，可检索/帧层照样按**没有假设**的世界
 * 答话 —— 「如果 猫 会 飞」之后还能听见「猫 不会 飞」。
 * 判定很朴素：这句话里若有一对词 (x,y)，图里有 x -[r]-> y（或否定形式），
 * 而**看得见的假设**里写的是相反的极性，就算冲突。
 * 不做语义理解，只用「句子里出现的边 + 假设层」这一套同一事实视图。
 * 没开假设时零开销（先查 hypo_visible_n）。 */
static inline size_t ai_hypo_conflicts(const AI *ai, const Token *view_who,
                                       Token *const *seq, size_t n,
                                       char *out, size_t cap) {
    size_t nconf = 0;
    if (out && cap) out[0] = 0;
    if (!ai || !seq || n < 2) return 0;
    if (n > 32) n = 32;
    if (hypo_visible_n(&ai->hypo, view_who) == 0) return 0;
    for (size_t i = 0; i < n; ++i) {
        const Token *x = seq[i];
        if (!x) continue;
        for (size_t j = 0; j < n; ++j) {
            const Token *y = seq[j];
            if (!y || y == x) continue;
            for (size_t k = 0; k < x->count; ++k) {
                const AssocEntry *e = &x->assocs[k];
                if (e->main != y) continue;
                for (uint8_t r = 1; r <= REL_CORE_MAX; ++r) {
                    uint16_t bit = REL_BIT(r);
                    int gpos = (e->rel  & bit) ? 1 : 0;
                    int gneg = (e->nrel & bit) ? 1 : 0;
                    if (!gpos && !gneg) continue;
                    const HypoFact *hf = hypo_find(&ai->hypo, view_who, x, r, y, NULL);
                    if (!hf) continue;
                    int hneg = hf->neg ? 1 : 0;
                    if ((gpos && hneg) || (gneg && !hneg)) {
                        nconf++;
                        if (out && cap && !out[0]) {
                            const RelDef *rd = rel_by_id(r);
                            snprintf(out, cap, "%s %s%s %s",
                                     x->name ? x->name : "?",
                                     hneg ? "" : "不 ",
                                     rd && rd->name ? rd->name : "?",
                                     y->name ? y->name : "?");
                        }
                    }
                }
            }
        }
    }
    /* 第二路：**看句面**，不依赖图里有没有那条边。
     * 帧层的答复是模板渲染出来的（`我 不 怕 黑`），句中的边不一定在图里 ——
     * 只查图的话，假设就管不住帧层。判据：
     *   句子里同时出现 a 和 b，且中间夹着这个关系的词（怕/会/属于…），
     *   就算这句在讲那条关系；极性看 a 与关系词之间有没有「不」「没」。 */
    for (size_t k = 0; k < ai->hypo.depth; ++k) {
        const HypoFrame *f = &ai->hypo.fr[k];
        if (!hypo_frame_visible(f, view_who)) continue;
        for (size_t q = 0; q < f->n; ++q) {
            const HypoFact *hp = &f->f[q];
            const RelDef *rd = rel_by_id(hp->rid);
            if (!rd || !rd->name || !hp->a || !hp->b) continue;
            size_t ia = n, ib = n, ir = n;
            for (size_t i = 0; i < n; ++i) {
                const Token *t = seq[i];
                if (!t) continue;
                if (ia == n) { if (t == hp->a) ia = i; continue; }
                if (ir == n && t->name && strcmp(t->name, rd->name) == 0) { ir = i; continue; }
                if (t == hp->b) { ib = i; break; }
            }
            if (ia == n || ir == n || ib == n) continue;
            int sneg = 0;
            for (size_t i = ia; i < ir; ++i)
                if (seq[i] && seq[i]->name &&
                    (strcmp(seq[i]->name, "不") == 0 || strcmp(seq[i]->name, "没") == 0)) {
                    sneg = 1; break;
                }
            if (sneg == (hp->neg ? 1 : 0)) continue;   /* 极性一致 → 不算冲突 */
            nconf++;
            if (out && cap && !out[0])
                snprintf(out, cap, "%s %s%s %s",
                         hp->a->name ? hp->a->name : "?",
                         hp->neg ? "" : "不 ",
                         rd->name,
                         hp->b->name ? hp->b->name : "?");
        }
    }
    return nconf;
}

static inline void proof_prepend_step(Proof *pf, Token *t, uint8_t rel,
                                      uint8_t via, int sim, uint8_t hyp) {
    if (!pf || pf->n + 1 >= REL_MAX_PROOF) return;
    for (size_t q = pf->n; q > 0; --q) pf->step[q] = pf->step[q-1];
    pf->step[0].tok = t; pf->step[0].rel = 0; pf->step[0].neg = 0;
    pf->step[0].via = 0; pf->step[0].sim = 0;
    pf->step[0].hyp = 0; pf->step[0].known = 0;
    pf->step[1].rel = rel;
    pf->step[1].via = via;
    pf->step[1].sim = sim;
    pf->step[1].hyp = hyp;
    pf->step[1].known = 0;
    pf->n++;
}

/* 求解：A R B 是真是假？want_neg=1 时求「A 不 R B」。
 * 返回 1 表示求出了确定结论（pf->status = +1 / -1）。 */
/* 前向声明：下面两个「规则前提」判定要递归用求解器 */
/* 复核模式下用的查边：**只看干净的图观测** ——
 * 跳过推出来沉淀的边、跳过自相矛盾的边（rel/nrel 同位都置 = 这条边自己打架，
 * 它不该给任何缓存当依据）、也不认假设覆盖层（假设不是知识）。
 * 定义在 rel_solve_go 之前（它要用），而 rel_solve_go 之后定义的复核函数还会用到它。 */
static inline int rel_direct_review(const Token *a, uint8_t rid, const Token *b,
                                    int want_neg) {
    if (!a || !b || rid == 0 || rid >= REL_DYN_BASE) return 0;
    uint16_t bit = REL_BIT(rid);
    for (size_t i = 0; i < a->count; ++i) {
        const AssocEntry *e = &a->assocs[i];
        if (e->main != b || edge_is_causal_guess(e)) continue;
        if (e->role == ASSOC_ROLE_DERIVED) continue;       /* 缓存不算依据 */
        if ((e->rel & bit) && (e->nrel & bit)) continue;   /* 自己打架的边不算依据 */
        if (want_neg ? (e->nrel & bit) : (e->rel & bit)) return 1;
    }
    return 0;
}

/* 把缓存排除在外再推一遍（返回是否能推出确定的真值）。定义在 rel_solve 之后。 */
static inline int rel_solve_ignoring_caches(AI *ai, const Token *a, uint8_t rid,
                                            const Token *b, int want_neg);

static inline int rel_solve_go(AI *ai, const Token *a, uint8_t rid, const Token *b,
                               int want_neg, Proof *pf, int depth,
                               const Token **seen, size_t n_seen);

/* ---- 独占规则（R6）的前提独立判定 ----
 * 验证器要用它**重新检查**这一步凭什么成立，而不是相信构造时标的 rule=7。
 * 返回证据 b1（为什么能否定 b），没有证据返回 NULL。 */
/* 把一层假设渲染成短文本：「猫 属于 鸟」/「他 以为：猫 会 飞」。
 * 撤回的时候要能说清撤掉的是哪一层（只说「撤了一层」等于没说）。 */
static inline void hypo_frame_text(const AI *ai, size_t k, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!ai || k >= ai->hypo.depth) return;
    const HypoFrame *f = &ai->hypo.fr[k];
    size_t off = 0;
    if (f->who && f->who->name)
        off += (size_t)snprintf(out + off, cap - off, "%s 以为：", f->who->name);
    for (size_t i = 0; i < f->n && off < cap; ++i) {
        const HypoFact *x = &f->f[i];
        off += (size_t)snprintf(out + off, cap - off, "%s%s %s%s %s",
                                i ? "；" : "",
                                x->a && x->a->name ? x->a->name : "?",
                                x->neg ? "不 " : "",
                                rel_name_of(ai, x->rid),
                                x->b && x->b->name ? x->b->name : "?");
    }
}

static inline int rel_is_excl(const AI *ai, uint8_t rid) {
    if (!ai || rid == 0 || rid > REL_TABLE_MAX) return 0;
    return ai->rel_excl[rid] != 0;
}
static inline Token *rel_excl_witness(AI *ai, const Token *a, uint8_t rid,
                                     const Token *b, int depth) {
    if (!ai || !a || !b || !rel_is_excl(ai, rid)) return NULL;
    Token *cands[128];
    uint8_t chyp[128];
    size_t nc = rel_next_cands(ai, a, rid, 0, cands, chyp, 128);
    for (size_t i = 0; i < nc; ++i) {
        Token *b1 = cands[i];
        if (!b1 || b1 == b || b1 == a) continue;
        /* b1 与 b 互不包含 —— 用 属于 的闭包判，两向都要查 */
        if (rel_direct(b1, REL_IS_A, b)) continue;
        if (rel_direct(b, REL_IS_A, b1)) continue;
        if (rel_solve_go(ai, b1, REL_IS_A, b, 0, NULL, depth + 1, NULL, 0)) continue;
        if (rel_solve_go(ai, b, REL_IS_A, b1, 0, NULL, depth + 1, NULL, 0)) continue;
        return b1;
    }
    return NULL;
}

/* ---- 「前提」的见证（R9 用）----
 * 做一件事 Y 得先有 P（`Y 需要 P`），而 X 恰好**没有** P ⟹ X 不 会 Y。
 * 找得到这样的 P 就返回它 —— 和独占那条规则一个套路：链上只写结论，
 * 「理由」由规则号表达，验证器再独立复核一遍。
 * 两处各放一跳继承（多了容易推歪）：
 *   · Y 需要 P 也可以来自上位能力（Y 属于 Y'，Y' 需要 P）；
 *   · X 没有 P 也可以来自上位（X 属于 X'，X' 没有 P）。 */
static inline Token *rel_need_witness(AI *ai, const Token *x, const Token *y, int depth) {
    if (!ai || !x || !y || depth > REL_MAX_DEPTH) return NULL;
    Token   *need[64];
    uint8_t  nh[64];
    size_t nn = rel_next_cands(ai, y, REL_NEED, 0, need, nh, 64);
    {   /* 上位能力的要求也继承：Y 属于 Y'，Y' 需要 P */
        Token   *sup[32];
        uint8_t  sh[32];
        size_t ns = rel_next_cands(ai, y, REL_IS_A, 0, sup, sh, 32);
        for (size_t i = 0; i < ns && nn < 64; ++i) {
            uint8_t tmp[64];
            size_t add = rel_next_cands(ai, sup[i], REL_NEED, 0,
                                        need + nn, tmp, 64 - nn);
            nn += add;
        }
    }
    if (nn == 0) return NULL;
    Token   *whole[32];
    uint8_t  wh[32];
    size_t nw = rel_next_cands(ai, x, REL_IS_A, 0, whole, wh, 32);
    for (size_t i = 0; i < nn; ++i) {
        Token *p = need[i];
        if (!p || p == x || p == y) continue;
        if (rel_direct_neg(x, REL_HAS, p)) return p;
        for (size_t k = 0; k < nw; ++k) {
            if (whole[k] == p) continue;
            if (rel_direct_neg(whole[k], REL_HAS, p)) return p;
        }
    }
    return NULL;
}

/* ---- 闭合世界（R7）的前提独立判定 ---- */
static inline int rel_cw_ok(const AI *ai, const Token *a) {
    if (!ai || !ai->closed_world || !a) return 0;
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].rel || a->assocs[i].nrel || a->assocs[i].dyn) return 1;
    return 0;
}

static inline int rel_solve_go(AI *ai, const Token *a, uint8_t rid, const Token *b,
                               int want_neg, Proof *pf, int depth,
                               const Token **seen, size_t n_seen) {
    if (!ai || !a || !b || depth > REL_MAX_DEPTH) return 0;
    const RelDef *rd = rel_by_id(rid);

    /* R0 直接（图 + 假设覆盖层）。
     * via=6 表示这一步靠的是假设 —— 链上会标成 DER_HYP，
     * 答复也必须写明「在假设下」，不能和真事实混为一谈。 */
    int hy_pos = 0, hy_neg = 0;
    int kn_pos = 0, kn_neg = 0;
    int d_pos, d_neg;
    if (g_rel_ignore_derived) {
        /* 复核模式：只用干净的图观测（缓存不算依据，不然「用缓存证明缓存」是循环论证；
         * 假设也不算 —— 假设不是知识） */
        d_pos = rel_direct_review(a, rid, b, 0);
        d_neg = rel_direct_review(a, rid, b, 1);
    } else {
        d_pos = rel_direct_as(a, rid, b, &kn_pos) || rel_direct_h(ai, a, rid, b, &hy_pos);
        d_neg = rel_direct_neg_as(a, rid, b, &kn_neg) ||
                rel_direct_neg_h(ai, a, rid, b, &hy_neg);
    }
    /* **缓存要跟着前提走**：一条推出来沉淀的边，只有在「把它排除在外还推得出来」
     * 的时候才算数。前提被推翻 / 被撤 / 自己也是缓存而它的依据没了 ——
     * 三种情况都会在这里失手，于是这条缓存当场不被采信（收尾时再撤销它）。
     * 复核本身用 g_rel_ignore_derived 挡住递归（复核里不再复核）。 */
    if (d_pos && kn_pos && !g_rel_ignore_derived && !rel_solve_ignoring_caches(ai, a, rid, b, 0))
        d_pos = 0;
    if (d_neg && kn_neg && !g_rel_ignore_derived && !rel_solve_ignoring_caches(ai, a, rid, b, 1))
        d_neg = 0;
    /* §6.125 **记忆里的猜测**（RECURSIVE TRANSFER 的入口）。
     * 上一段那道"缓存要跟着前提走"的闸，把**纯猜测**（没有独立依据、重推不出来）关在门外 ——
     * 这正是防中毒的关键。这里给出一趟**明确打开**的例外：只在 `g_rel_guess_visible` 时可见，
     * 而且用上它就**把整条证明标成 guessed**（答复必须写"先前推测的"）。
     * 三个"看不见"的场合：复核模式（g_rel_ignore_derived）、验证器自查"知识库里有吗"、
     * 以及任何没开这个全局的普通求解 —— 所以记忆永远不会变成"知道的"。 */
    int mem_used = 0;
    if (g_rel_guess_visible && !g_rel_ignore_derived) {
        if (!d_pos && rel_direct_guess(a, rid, b, 0)) { d_pos = 1; kn_pos = 2; mem_used = 1; }
        if (!d_neg && rel_direct_guess(a, rid, b, 1)) { d_neg = 1; kn_neg = 2; mem_used = 1; }
        if (mem_used && pf) pf->guessed = 1;
    }
    if (want_neg ? d_neg : d_pos) {
        uint8_t via = ((want_neg ? hy_neg : hy_pos) && !(want_neg ? rel_direct_neg(a, rid, b)
                                                                 : rel_direct(a, rid, b)))
                          ? 6 : 0;
        int known = want_neg ? kn_neg : kn_pos;
        proof_push(pf, (Token *)a, 0, 0, 0, 0);        /* proof_push 内部会判 pf */
        proof_push(pf, (Token *)b, rid, (uint8_t)want_neg, via, 0);
        if (pf && via == 6) pf->step[pf->n - 1].hyp = 1;
        if (pf && known && via != 6) pf->step[pf->n - 1].known = 1;
        /* pf 可能是 NULL：对称/逆关系那两条规则递归时只需要「成不成立」，
         * 不要路径。这里必须判空 —— 漏了它直接段错误（ASan 抓到的）。 */
        if (pf) pf->status = want_neg ? -1 : +1;
        return 1;
    }

    if (rd) {
        /* **具体优先于一般**（非单调推理的核心，default with exceptions）：
         * 同一对词上若已经有一条**观测到的**边说的是正相反的事，
         * 那么靠规则外推出来的结论要让路 —— 不是「两边都成立、报个矛盾」，
         * 而是「这条一般规则在这个具体例子上被推翻了」。
         * 实测触发它的场景：
         *   猫 属于 动物、动物 有 毛 ⟹ 猫 有 毛（继承）；随后用户说「猫 没有 毛」
         *   —— 观测是具体事实，规则该让路。不让路的话每轮都会报「自相矛盾」，
         *   而且用户教的修正永远留不下来。
         * 只判 depth==0：例外不许传播到推理内部（传播出去会把整条链都掐死）。
         * 只认**非缓存**的直接边：缓存是自己以前记的笔记，不是观测，
         * 它该被现场推理推翻并撤销（见 think.h 的「缓存被推翻」）。 */
        /* **具体优先于一般**（default with exceptions）的边界，每一条都是被回归逼出来的：
         *   ① 只挡**传递/继承**这两条外推规则。独占(R7)/闭合世界(R8)/缺前提(R9)
         *      不挡 —— 它们的结论本来就是「否定」，拿它们和直接否定对撞没意义；
         *      挡了还会改掉依据链（`·独占` 变成直接事实），而那是回归钉住的呈现。
         *   ② 只判 depth==0：例外不许传播到推理内部（传播出去会把整条链掐死）。
         *   ③ **假设世界里不挡**：覆盖层本来就压过图里的边（R0 已经这么做），
         *      挡了会出现「如果 猫 属于 鸟」推不出「猫 会 飞」（实测）。
         *   ④ 只认**非缓存**的直接边：缓存是自己以前记的笔记，不是观测；
         *      它该被现场推理推翻并撤销（见 think.h 的「缓存被推翻」）。 */
        int spec_blocked = 0;
        if (depth == 0 && hypo_visible_n(&ai->hypo, ai->view_who) == 0) {
            int derived = 0;
            int opposite_direct = want_neg ? rel_direct_as(a, rid, b, &derived)
                                           : rel_direct_neg_as(a, rid, b, &derived);
            if (opposite_direct && !derived) spec_blocked = 1;
        }
        for (size_t i = 0; i < n_seen; ++i) if (seen[i] == a) return 0;
        if (n_seen >= REL_MAX_DEPTH) return 0;
        const Token *nseen[REL_MAX_DEPTH + 1];
        for (size_t i = 0; i < n_seen; ++i) nseen[i] = seen[i];
        nseen[n_seen] = a;

        /* R1 传递（候选来自图 + 假设层）—— 被具体事实挡住时不外推 */
        if (rd->trans && !spec_blocked) {
            Token *cands[128];
            uint8_t chyp[128];
            size_t nc = rel_next_cands(ai, a, rid, want_neg, cands, chyp, 128);
            for (size_t i = 0; i < nc; ++i) {
                if (cands[i] == b) continue;
                size_t save = pf ? pf->n : 0;
                if (rel_solve_go(ai, cands[i], rid, b, want_neg, pf, depth + 1,
                                 nseen, n_seen + 1)) {
                    proof_prepend_step(pf, (Token *)a, rid, 1, 0, chyp[i]);
                    return 1;
                }
                if (pf) pf->n = save;
            }
        }
        /* R2 继承（肯定与否定都能继承）—— 同上，被具体事实挡住时不外推 */
        if (rd->inherit && !spec_blocked) {
            for (int wneg = 0; wneg <= 1; ++wneg) {
                if (want_neg && !wneg) continue;      /* 只在求否定时试否定继承 */
                if (!want_neg && wneg) continue;
                Token *cands[128];
                uint8_t chyp[128];
                size_t nc = rel_next_cands(ai, a, REL_IS_A, 0, cands, chyp, 128);
                for (size_t i = 0; i < nc; ++i) {
                    if (cands[i] == b) continue;
                    size_t save = pf ? pf->n : 0;
                    if (rel_solve_go(ai, cands[i], rid, b, wneg, pf, depth + 1,
                                     nseen, n_seen + 1)) {
                        proof_prepend_step(pf, (Token *)a, REL_IS_A, 2, 0, chyp[i]);
                        return 1;
                    }
                    if (pf) pf->n = save;
                }
            }
        }
        /* R10 **经前提**（用户要的「推理层按前提决定用不用」）：
         *   A —[前提]→ P，**P 在当前注意窗口里**（前提成立），且 P R B ⟹ A R B。
         * 与 R2 继承的区别：走的是**前提链接**（边上的 [前提] 标记），不是「属于」；
         * 而且它**带闸门**：前提不成立就**不用**这条连结（不是"图里没有"，
         * 而是"现在用不了"）。前提本身是普通 token（联想照走）—— 闸门只在逻辑这一侧。
         * 由 `tools/optest.c` / `tests/tokenop.sh` 钉住：前提在提问里 → 用；不在 → 不用。 */
        if (!spec_blocked) {
            for (size_t i = 0; i < a->count; ++i) {
                const AssocEntry *pe = &a->assocs[i];
                int ppol = edge_premise_polarity(pe);
                if (!ppol || !pe->main) continue;
                Token *P = pe->main;
                if (P == b) continue;
                /* 条件不成立 → 不用：正前提要它在窗口里，反前提要它**不在** */
                if (!ai_premise_cond_ok(ai, P, ppol)) continue;
                size_t save = pf ? pf->n : 0;
                if (rel_solve_go(ai, P, rid, b, want_neg, pf, depth + 1,
                                 nseen, n_seen + 1)) {
                    proof_prepend_step(pf, (Token *)a, 0, (uint8_t)(ppol < 0 ? 11 : 10), 0, 0);
                    return 1;
                }
                if (pf) pf->n = save;
            }
        }
        /* R3 对称：B R A ⟹ A R B。
         * 链上直接写成 A -[R·对称]-> B —— 把反向路径原样搬过来会读成
         * 「B → A」，与问题方向相反，反而看不懂。 */
        if (rd->sym) {
            size_t save = pf ? pf->n : 0;
            if (rel_solve_go(ai, b, rid, a, want_neg, NULL, depth + 1,
                             nseen, n_seen + 1)) {
                if (pf) {
                    pf->n = 0;
                    proof_push(pf, (Token *)a, 0, 0, 0, 0);
                    proof_push(pf, (Token *)b, rid, (uint8_t)want_neg, 3, 0);
                    pf->status = want_neg ? -1 : +1;
                }
                return 1;
            }
            if (pf) pf->n = save;
        }
        /* R6 独占（函数式）：a R b1 且 R 独占，b1 与 b2 **互不包含**
         *   ⟹ a 不 R b2。
         *
         * 「互不包含」这一条是安全阀，不能省：
         *   猫 属于 动物，问 猫 属于 鸟 —— 鸟 属于 动物（鸟 ⊂ 动物），
         *   所以「猫 是 鸟」完全可能，**推不出否定**。
         *   猫 属于 动物，问 猫 属于 植物 —— 动物 与 植物 互不包含 → 能推出否定。
         * 这正是「完备性」那条局限的正面解法：不靠「没写就算假」，
         * 靠一条声明出来的公理 + 一次包含判定。 */
        if (rel_is_excl(ai, rid) && !want_neg) {
            Token *b1 = rel_excl_witness(ai, a, rid, b, depth);
            if (b1) {
                if (pf) {
                    /* 链上只写**两步**：a（起点）→ b（否定结论）。
                     * 早先写成 a→b1→b 三跳，看起来像一条链，
                     * 可验证器按「相邻两环首尾相接」去验就崩了 ——
                     * b1 是**理由**不是链的一环。理由已经在 rule=7 里表达了。 */
                    pf->n = 0;
                    proof_push(pf, (Token *)a, 0, 0, 0, 0);
                    proof_push(pf, (Token *)b, rid, 1, 7, 0);
                    pf->status = -1;
                }
                return 1;
            }
        }

        /* R9 缺前提 → 否定能力：Y 需要 P（做 Y 得先有 P），而 X 没有 P
         *   ⟹ X 不 会 Y。
         * 这就是「训练数据里没有『猫 会不会唱歌』，却能推出来」的那条规则：
         *   唱歌 需要 灵活的声带；猫 没有 灵活的声带 ⟹ 猫 不 会 唱歌。
         * 只加在「求否定 + 这个关系**在表里声明了 prereq**」上 ——
         * 别的动词（吃/喜欢/怕…）没有「前提」这回事，硬套会推出
         * 「猫 没有 声带 ⟹ 猫 不 喜欢唱歌」这种胡话。 */
        if (want_neg && rd && rd->prereq && rel_need_witness(ai, a, b, depth)) {
            if (pf) {
                pf->n = 0;
                proof_push(pf, (Token *)a, 0, 0, 0, 0);
                proof_push(pf, (Token *)b, rid, 1, 9, 0);
                pf->status = -1;
            }
            return 1;
        }

        /* R4 逆关系：B inv(R) A ⟹ A R B */
        if (rd->inv) {
            size_t save = pf ? pf->n : 0;
            if (rel_solve_go(ai, b, rd->inv, a, want_neg, NULL, depth + 1,
                             nseen, n_seen + 1)) {
                if (pf) {
                    pf->n = 0;
                    proof_push(pf, (Token *)a, 0, 0, 0, 0);
                    proof_push(pf, (Token *)b, rid, (uint8_t)want_neg, 4, 0);
                    pf->status = want_neg ? -1 : +1;
                }
                return 1;
            }
            if (pf) pf->n = save;
        }
    }
    /* R7 闭合世界（**可选，默认关**）：我们认识这个主语（它有别的已知关系），
     * 却推不出 a R b ⟹ 认为 a 不 R b。
     *
     * 这是一个**假设**，不是真理 —— 所以必须显式打开（::world closed）。
     * 只在最外层用（depth==0）：子目标上乱用会把否定传播到推理内部。
     * 默认开放世界时，这个分支不生效，推不出就老实说「不知道」。 */
    if (ai && ai->closed_world && depth == 0 && !want_neg) {
        if (rel_cw_ok(ai, a)) {
            if (pf) {
                pf->n = 0;
                proof_push(pf, (Token *)a, 0, 0, 0, 0);
                proof_push(pf, (Token *)b, rid, 1, 8, 0);
                pf->status = -1;
            }
            return 1;
        }
    }
    return 0;
}

/* **同类互推**（"兄弟"里有没有谁也 R B）：A 与 A2 都 属于 同一个 P，而 A2 —R→ B。
 * 放在 rel.h 是因为它**在 think.h 与 agent.h 之前** —— 思维层与对话层都要用它
 * （以前只在 agent.h 里有一份，思维层调不到，只能另写；这正是 §6.100 记的分层问题）。
 * 返回找到的那个"兄弟"（`*sup_out` 给出共同上位），没有就 NULL。扫描有上限（64 个候选）。 */
static inline const Token *rel_sibling_with(const AI *ai, const Token *a, uint8_t rid,
                                            const Token *b, const Token **sup_out) {
    if (sup_out) *sup_out = NULL;
    if (!ai || !a || !b || rid == 0) return NULL;
    Token *sup[8];
    uint8_t sh[8];
    size_t nsup = rel_next_cands(ai, a, REL_IS_A, 0, sup, sh, 8);
    for (size_t k = 0; k < nsup; ++k) {
        const Token *P = sup[k];
        if (!P || !P->name || P->name[0] == '[' || P == b) continue;
        size_t scanned = 0;
        for (size_t i = 0; i < ai->pool->count && scanned < 64; ++i) {
            const Token *t2 = ai->pool->tokens[i];
            if (!t2 || t2->is_garbage || t2 == a || !t2->name || t2->name[0] == '[') continue;
            if (!rel_direct(t2, REL_IS_A, P)) continue;
            scanned++;
            if (rel_direct(t2, rid, b)) { if (sup_out) *sup_out = P; return t2; }
        }
    }
    return NULL;
}

static inline void rel_solve(AI *ai, const Token *a, uint8_t rid, const Token *b,
                             int want_neg, Proof *pf) {
    if (!pf) return;
    pf->n = 0; pf->status = 0; pf->guessed = 0;
    if (!ai || !a || !b) return;
    rel_solve_go(ai, a, rid, b, want_neg, pf, 0, NULL, 0);
}

/* 把缓存（推出来沉淀的边）当成不存在，再推一遍 —— 这是「缓存跟着前提走」的复核手段。
 * 不另存依赖表、不动存档格式：**依据就是「还能不能从图里推出来」**，
 * 每次现算。返回 1 = 推得出确定真值。 */
static inline int rel_solve_ignoring_caches(AI *ai, const Token *a, uint8_t rid,
                                            const Token *b, int want_neg) {
    if (!ai || !a || !b || rid == 0) return 0;
    Proof pf;
    g_rel_ignore_derived = 1;
    rel_solve(ai, a, rid, b, want_neg, &pf);
    g_rel_ignore_derived = 0;
    return pf.status != 0;
}

/* R5 类比推测：推不出来时，找**最像 A、并且对这件事有态度**的那个词借答案。
 *
 * 注意是「有态度」而不是「知道肯定」：最像的那个词也可能明确**否定**这件事
 * （`猫 不 会 飞`），那它的意见同样有价值。所以按相似度取最近邻，
 * 由它投赞成票还是反对票决定推测的方向 —— 这就是 k=1 的最近邻默认推理。
 * 矛盾时由「更近的那个」说话，不做多数表决（这个规模的数据不支持）。
 *
 * 这是非单调推理，所以 pf->guessed 会被置上，答复里必须写明是猜的。 */
/* **结构相似**：两个词的**关系邻居集合**有多重合（同一个宾语 + 同一位关系算一个元素）。
 * 只看带关系位的边 —— 所以它衡量的是"**结构**像不像"，不是"用词像不像"。
 * 这正是「结构映射式类比」与 R5（emb 最近邻）的区别：用词完全不同、但关系骨架一致
 * （`猫 属于 动物`、`狗 属于 动物`、`猫 有 毛`、`狗 有 毛`）时，emb 可能远，结构很近。
 * 返回 0..100；`out_common` 带回共同邻居数（调用方要用它挡掉"碰巧重合一个"）。 */
static inline int rel_struct_sim(const Token *a, const Token *s, size_t *out_common) {
    if (out_common) *out_common = 0;
    if (!a || !s || a == s) return 0;
    size_t sa = 0, ss = 0, inter = 0;
    for (size_t i = 0; i < a->count; ++i)
        if (a->assocs[i].main && (a->assocs[i].rel || a->assocs[i].nrel)) sa++;
    for (size_t j = 0; j < s->count; ++j)
        if (s->assocs[j].main && (s->assocs[j].rel || s->assocs[j].nrel)) ss++;
    if (sa == 0 || ss == 0) return 0;
    for (size_t i = 0; i < a->count; ++i) {
        const AssocEntry *e = &a->assocs[i];
        if (!e->main || (!e->rel && !e->nrel)) continue;
        for (size_t j = 0; j < s->count; ++j) {
            const AssocEntry *f = &s->assocs[j];
            if (f->main != e->main) continue;
            if ((f->rel & e->rel) || (f->nrel & e->nrel)) { inter++; break; }
        }
    }
    if (out_common) *out_common = inter;
    size_t uni = sa + ss - inter;
    if (uni == 0) return 0;
    return (int)((inter * 100u) / uni);
}

/* 结构类比的判据（写在**一处**）：共同关系邻居至少这么多，且结构相似度至少这么高。 */
#define REL_STRUCT_MIN_COMMON 3
#define REL_STRUCT_MIN_PCT    60

static inline int rel_guess(AI *ai, const Token *a, uint8_t rid, const Token *b,
                            Proof *pf) {
    /* ⚠ 原来这里硬性要求 `a->emb && b->emb` ⇒ **对话里现学的新词没有 emb**（emb 只在装载/训练时
     * 构建）⇒ 联想引擎对新词直接罢工（症状：学了 `乙熊 属于 熊类`、`乙熊 会 冬眠` 之后，
     * 问 `甲熊 会 冬眠 吗` 永远"不确定"）。结构/兄弟这类证据**本来就不需要 emb**：
     * 下面用到 emb 的地方自己会判 `a->emb && s->emb` ⇒ 这里不必拦。 */
    if (!ai || !a || !b || !pf) return 0;
    Token *best = NULL;
    int best_pol = 0;
    int best_pct = REL_GUESS_SIM;              /* 门槛：低于它不算"像" */
    /* **同上位的兄弟**（§6.117）：`a` 与某个 `s` 有共同上位 P，且 `s` 对 (rid,b) 有态度。
     * 这比"共同关系邻居数"更硬（同为 P 的实例），但小图里共同邻居常只有 1~2 个
     * ⇒ 以前被 `REL_STRUCT_MIN_COMMON = 3` 挡在门外（实测：甲熊/乙熊 只共 2 个邻居，
     * 于是"甲熊 会 冬眠 吗"永远答"不确定"）。这里先算一次，循环里给它**够过门槛**的分。 */
    const Token *sib0 = rel_sibling_with(ai, a, rid, b, NULL);
    int best_struct = 0;                       /* 这次的依据是**结构**还是 emb */
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *s = ai->pool->tokens[i];
        if (s->is_garbage || s == a || s == b) continue;
        if (!s->name || s->name[0] == '[') continue;
        int pol = 0;
        int hy = 0;
        if (rel_direct(s, rid, b) || rel_direct_h(ai, s, rid, b, &hy)) pol = +1;
        else if (rel_direct_neg(s, rid, b) || rel_direct_neg_h(ai, s, rid, b, &hy)) pol = -1;
        if (!pol) continue;                          /* 它得对这件事有态度 */
        /* 两条候选来源，取更像的那个：
         *   ① emb 余弦（用词/语境像）—— 需要两边都有 emb；
         *   ② **结构相似**（关系骨架像）—— 用词可以完全不同，这是结构映射式类比。 */
        int pct = -1, is_struct = 0;
        if (a->emb && s->emb) {
            double c = (double)emb_cosine(a->emb, s->emb);
            if (c * 100.0 > pct) { pct = (int)(c * 100.0); is_struct = 0; }
        }
        {
            size_t common = 0;
            int st = rel_struct_sim(a, s, &common);
            if (st >= REL_STRUCT_MIN_PCT && common >= REL_STRUCT_MIN_COMMON && st > pct) {
                pct = st;
                is_struct = 1;
            }
        }
        if (sib0 && s == sib0 && pct < REL_STRUCT_MIN_PCT) {
            /* 兄弟这条证据：直接给到结构门槛分（不再要求共同邻居 ≥3） */
            pct = REL_STRUCT_MIN_PCT; is_struct = 1;
        }
        if (pct > best_pct || (pct == best_pct && is_struct && !best_struct)) {
            best_pct = pct; best = s; best_pol = pol; best_struct = is_struct;
        }
    }
    if (!best) return 0;
    pf->n = 0;
    pf->status = best_pol;
    pf->guessed = 1;
    proof_push(pf, (Token *)a, 0, 0, 0, 0);
    proof_push(pf, best, rid, 0, 5, best_pct);
    proof_push(pf, (Token *)b, rid, (uint8_t)(best_pol < 0), 0, 0);
    return 1;
}

/* 把推导链渲染成一行，带**关系标签**（不只是词）：
 *   猫 -[属于·传递]-> 动物 -[有]-> 毛
 * 这一步很重要：只有词的话看不出每一跳是靠哪条规则、哪个关系走的。 */
static inline void proof_text(const AI *ai, const Proof *pf, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!pf || pf->n == 0) return;
    size_t off = 0;
    for (size_t i = 0; i < pf->n; ++i) {
        const ProofStep *s = &pf->step[i];
        const char *nm = (s->tok && s->tok->name) ? s->tok->name : "?";
        int w;
        if (i == 0) {
            w = snprintf(out + off, cap - off, "%s", nm);
        } else {
            const char *rn = rel_name_of(ai, s->rel);
            if (!rn) rn = "?";
            if (s->via == 5) {
                w = snprintf(out + off, cap - off, " ≈（%d%% 像）≈> %s", s->sim, nm);
            } else if (s->via == 10 || s->via == 11) {
                /* 前提这一跳没有关系词：读作 `A -[前提]-> P` / `A -[前提不]-> P` */
                w = snprintf(out + off, cap - off, " -[%s]-> %s", rel_via_name(s->via), nm);
            } else if (s->via == 0) {
                w = snprintf(out + off, cap - off, " -[%s%s]-> %s",
                             s->neg ? "不 " : "", rn, nm);
            } else {
                w = snprintf(out + off, cap - off, " -[%s·%s]-> %s",
                             rn, rel_via_name(s->via), nm);
            }
        }
        if (w < 0 || (size_t)w >= cap - off) { off = cap - 1; break; }
        off += (size_t)w;
    }
    out[off < cap ? off : cap - 1] = 0;
}

/* ---------------------------------------------------------------- 句形识别 */

static inline int rel_is_qmark(const char *w) {
    if (!w) return 0;
    return strcmp(w, "吗") == 0 || strcmp(w, "呢") == 0 ||
           strcmp(w, "么") == 0 || strcmp(w, "吧") == 0 || strcmp(w, "?") == 0 ||
           strcmp(w, "？") == 0;
}

/* 正好一个汉字、而且是实词。虚词检查极要紧：`你 吃 火锅 了 吗` 里 `了`
 * 也是单字，不管它就会把宾语拼成 `火锅了`，解析器凭空造出垃圾 Token（踩过）。 */
static inline int rel_one_cjk(const char *s) {
    if (!s) return 0;
    if (strlen(s) != 3 || ((unsigned char)s[0] & 0xF0) != 0xE0) return 0;
    return !fr_is_func(s);
}

/* 先只验**形状**、完全不碰关系表：
 *   `<主语> [不] <谓语词> <宾语…> [吗]`，主语 1 个词、谓语 1 个词、
 *   宾语 1 个词或 2~4 个单字实词。
 * 存在的意义：登记新谓语是**有副作用**的，不能先登记再发现形状不对 ——
 * 实测「你 觉得 天气 怎么样」会因此把「觉得」登记成动态关系（还写进模型）。 */
static inline int rel_shape_ok(char *const *words, size_t n) {
    if (!words || n < 3 || n > 6) return 0;
    size_t last = n;
    if (rel_is_qmark(words[n - 1])) last = n - 1;
    if (last < 3) return 0;
    if (!words[0][0] || words[0][0] == '[') return 0;
    size_t ri = 1;
    if (last >= 4 && strcmp(words[1], "不") == 0) ri = 2;
    if (ri >= last) return 0;
    size_t nobj = last - ri - 1;
    if (nobj == 0 || nobj > 4) return 0;
    if (nobj == 1) return words[ri + 1][0] != '[';
    for (size_t i = ri + 1; i < last; ++i)
        if (!rel_one_cjk(words[i])) return 0;
    return 1;
}

/* 解析 `<主语> [不] <关系词> <宾语…> [吗]`。
 * 主语固定第 0 个词；`不` 可选；宾语可由多个单字拼回一个词（宠物 → 宠+物）。
 * 拼好的宾语写进 buf，*bw 指向 buf。 */
static inline int rel_parse(AI *ai, char *const *words, size_t n,
                            char *buf, size_t bufsz,
                            const char **aw, uint8_t *rid, int *neg,
                            const char **bw, int *is_q, const char **rel_word,
                            int allow_new) {
    if (!ai || !words || n < 3) return 0;
    size_t last = n;
    int q = 0;
    if (rel_is_qmark(words[n - 1])) { q = 1; last = n - 1; }
    if (last < 3) return 0;
    if (!words[0][0] || words[0][0] == '[') return 0;

    size_t ri = 1;                        /* 关系词的位置 */
    int is_neg = 0;
    if (last >= 4 && (strcmp(words[1], "不") == 0 || strcmp(words[1], "没") == 0)) { is_neg = 1; ri = 2; }
    /* 「没有」是「有」的否定形式，而且它是**一个词**：
     *   `猫 没有 声带` = 猫 -[不 有]-> 声带，而不是「猫 没有 X」三个词。
     * 不认它的话，「缺前提」那类事实就没法从对话里说进来（实测）。 */
    if (!is_neg && last == 3 && ri == 1 &&
        (strcmp(words[1], "没有") == 0 || strcmp(words[1], "没") == 0)) {
        /* 只接受**正好一个宾语词**：多的词宁可整句不认，也不能悄悄丢掉
         * （静默丢词会学出「鱼 不 有 声」这种半截事实）。 */
        is_neg = 1; ri = 2;
        if (aw) *aw = words[0];
        uint8_t hid = rel_id_of(ai, "有", 0);
        if (!hid) return 0;
        if (bw) *bw = words[ri];
        if (rid) *rid = hid;
        if (neg) *neg = 1;
        if (rel_word) *rel_word = words[1];
        if (is_q) *is_q = q;
        return 1;
    }
    if (ri >= last) return 0;
    uint8_t id = 0;
    const char *merged_name = NULL;   /* 合写时：那个 token 里**已经含**否定了，别再拼一遍 */
    /* 「不 X」/「没 X」：先看**拼起来**是不是情态说法（`不能`/`没法`/`不敢` —— 分词器把
     * 它们切成两个词，所以得拼回去查那张表）；再看 X 是不是关系词。
     * 顺序要紧：先查表再 `rel_id_of(..., allow_new)`，否则「能」会被当场登记成动态谓语。 */
    if (is_neg && ri == 2 && words[ri]) {
        char jw[40];
        snprintf(jw, sizeof jw, "%s%s", words[1], words[ri]);
        const RelModalRow *mr = rel_modal_of(jw);
        if (mr) {
            uint8_t r2 = rel_id_of(ai, mr->rel, 0);
            if (r2) {
                id = r2;
                is_neg = mr->neg;
                merged_name = rel_name_of(ai, r2);
            }
        }
    }
    if (!is_neg) {
        /* 合写否定（`不会`/`不喜欢`/`不是`）：**一个 token 就是「否定 + 关系」** ——
         * 与两词写法共用同一份声明（`rel_merged_neg_id` 去空格比）。 */
        uint8_t mid = 0;
        if (rel_merged_neg_id(words[1], &mid)) {
            is_neg = 1;
            id = mid;
            { const RelDef *mrd = rel_by_id(mid); merged_name = mrd ? mrd->name : NULL; }
        } else if (rel_modal_of(words[1])) {
            /* 情态写法（`不能`/`没法`/…）：归到那张表上的 (关系, 否定) */
            const RelModalRow *mr = rel_modal_of(words[1]);
            uint8_t rid2 = rel_id_of(ai, mr->rel, 0);
            if (rid2 == 0) return 0;
            id = rid2;
            is_neg = mr->neg;
            merged_name = rel_name_of(ai, rid2);
        } else {
            id = rel_id_of(ai, words[ri], allow_new);
        }
    } else if (!id) {
        id = rel_id_of(ai, words[ri], allow_new);
    }
    if (!id) return 0;
    if (!id) return 0;

    size_t nobj = last - ri - 1;
    if (nobj == 0) return 0;
    if (nobj == 1) {
        if (words[ri + 1][0] == '[') return 0;
        if (bw) *bw = words[ri + 1];
    } else {
        if (nobj > 4) return 0;
        size_t bl = 0;
        for (size_t i = ri + 1; i < last; ++i) {
            if (!rel_one_cjk(words[i])) return 0;
            size_t l = strlen(words[i]);
            if (bl + l + 1 > bufsz) return 0;
            memcpy(buf + bl, words[i], l);
            bl += l;
        }
        buf[bl] = 0;
        if (bw) *bw = buf;
    }
    if (aw) *aw = words[0];
    if (rid) *rid = id;
    if (neg) *neg = is_neg;
    if (rel_word) *rel_word = merged_name ? merged_name : words[ri];
    if (is_q) *is_q = q;
    return 1;
}

#endif /* REL_H */
