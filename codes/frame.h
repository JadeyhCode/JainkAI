/* frame.h — 结构化回复层
 *
 * 表示：回复不再是「一串固定的词」，而是**帧**：
 *   - 一组问法模板 Q（可以匹配多种问法）
 *   - 一组回复模板 A（里面的占位符是槽位）
 *   - 每个槽位一个**填充类**（哪些词可以填进去）
 *
 * 生成 = 「匹配一个问法模板 → 认出用户改了哪个槽位 → 把回复模板里对应的
 *        槽位填上用户的取值 → 渲染」。
 * 于是能说出「语料里没写过、但骨架合法」的句子 —— 这是扁平检索做不到的。
 *
 * 依赖：token.h + ai.h 的类型（被 ai.h 之后包含）。
 */
#ifndef FRAME_H
#define FRAME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FR_MAX_TMPL  16      /* 每帧最多几个模板 */
#define FR_MAX_SLOT  8       /* 每帧最多几个槽位 */
#define FR_MAX_FILL  48      /* 每个槽位最多几个填充词 */
#define FR_MAX_FRAMES 64
#define FR_MAX_TOK   24      /* 一个模板最多几个词 */
#define FR_COIN_BUMP 4       /* 现场造词时，新词与示范词之间那条边的权重 */

/* frames.txt 里「降级答复」的行前缀。用 sizeof-1 取长度，不手数字节。 */
static const char FR_FALL_PFX[] = "[降级答] ";

typedef struct {
    const char *name;
    Token *toks[FR_MAX_TMPL][FR_MAX_TOK];
    int8_t  slot_at[FR_MAX_TMPL][FR_MAX_TOK];   /* -1 = 普通词，>=0 = 槽位下标 */
    size_t  n_toks[FR_MAX_TMPL];
    size_t  n_tmpl;
} FrTemplates;

/* 槽位取值：可以是多个词（用户说「画画」就是 画+画 两个词）。 */
typedef struct {
    Token *tok[FR_MAX_TOK];
    size_t n;
} FrValue;

typedef struct {
    const char *name;                    /* 槽位名，如 X / Y */
    Token *fill[FR_MAX_FILL];
    size_t n_fill;
    size_t max_len;                      /* 这个槽位最多吸收几个词（0 = 用默认） */
} FrSlot;

/* ------------------------------------------------- 槽位取值的「关联链证据」 */

/* 结构化帧原本只认 frames.txt 手写的白名单。白名单是**从语料里抽出来的**，
 * 抽到了就认、没抽到就一律拒绝，于是「你会支持我吗」这种输入永远落不进帧，
 * 只能退回整句检索。可是关联链里其实还留着证据：图里记着「支持」和
 * 白名单里那些词（说话/写字/做饭…）的邻居长得很像 —— 都是「人做的事」。
 *
 * Gate B（帧读关联链产物）= 白名单落空时，改看这份分布证据：
 *   ① 候选必须是**实词**（虚词的邻居天然和别人像，因为/很/不 都能拿到 40~60 分，
 *      但填进槽位就是病句）；
 *   ② 候选必须是**成形词**，不能是切词碎片（「顾」「信」「识」这种单字，
 *      或者「累了吗」这种带语气词尾巴的），它们的分布分数同样虚高；
 *   ③ 候选与这个槽位**声明的示范集**的邻居重叠（Dice）要达到阈值。
 *
 * 实测（model.bin，[帧:想X吗] 的 X 示范集 21 个词，阈值 40）：
 *   支持 47 ✓   理解 66 ✓   保护 66 ✓   告诉 42 ✓
 *   照顾 16 ✗   昨天 29 ✗   因为 55（被①挡）  很 41（被①挡）  不 59（被①挡）
 *   猫/狗/雪 等白名单内的词走 Gate A，不经过这里。
 *
 * 注意这**不是**把白名单换成「谁分数高用谁」——输入里的词是用户给的，
 * Gate B 只回答「用户说的这个词，像不像这个槽位该有的词」。 */
#define FR_ASSOC_SIM_MIN 40

/* 语气/结构助词：**一张表，两处用**。
 *   · fr_is_func()  —— 它们是虚词，填进槽位必成病句（Gate B ①号门）；
 *   · fr_wellformed() —— 以它们收尾的是切词碎片（「累了吗」），不是词。
 * 以前这两处各写一份，而「同一份清单出现在两处」就是行为分叉的种子：
 * 改了一处忘了另一处，症状是「某个语气词当虚词挡住了、当词尾却没挡住」。
 * 表里只放**两处都成立**的 8 个（既虚、又是典型词尾）。反例：「经过」以「过」收尾，
 * 但「过」在词尾是常态（它只是虚词，不是碎片指纹）—— 所以它留在虚词表、
 * 不进这张表，否则会误伤真词。子集关系由 tools/frametest.c 钉着。 */
static const char *FR_PARTICLES[] = {
    "的", "了", "吗", "呢", "吧", "啊", "嘛", "呀",
    NULL
};

/* 虚词表：Gate B 的①号门。只列真正没有实义、填进槽位必成病句的词。
 * 与 FR_PARTICLES 是**并集关系**（fr_is_func 两张都查）—— 清单仍然只有两份，
 * 且一份是另一份的父集，不是抄出来的第二份。 */
static const char *FR_FUNC[] = {
    "是", "在", "和", "也", "都", "就", "很", "不", "没", "还",
    "哦", "嗯",
    "我", "你", "他", "她", "它", "我们", "你们", "他们",
    "这", "那", "这个", "那个", "这些", "那些",
    "有", "会", "要", "能", "可以", "应该", "可能",
    "把", "被", "给", "对", "跟", "向", "从", "到", "让", "使",
    "而", "但", "因为", "所以", "如果", "虽然", "然后", "而且",
    "什么", "怎么", "为什么", "哪", "哪个", "多少", "几",
    "那么", "这么", "这样", "那样", "怎样", "如何", "这里", "那里",
    "时候", "现在", "刚才", "一直", "已经", "正在", "起来", "出来",
    "下去", "上去", "一些", "有点", "稍微", "一起",
    "一", "一个", "个", "些", "点", "上", "下", "里", "中", "外",
    "地", "得", "着", "过",
    "来", "去", "又", "再", "只", "才",
    "更", "最", "太", "非常", "一下",
    NULL
};

/* 遍历两张表（虚词表 + 助词表）—— fr_is_func 与自检都用它，免得又抄一份。
 * 重复项会被跳过（自检据此报警，见 tools/frametest.c）。 */
static inline size_t fr_func_list(const char **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; FR_FUNC[i] && n < cap; ++i) out[n++] = FR_FUNC[i];
    for (size_t i = 0; FR_PARTICLES[i] && n < cap; ++i) out[n++] = FR_PARTICLES[i];
    return n;
}

static inline int fr_is_func(const char *s) {
    if (!s || !*s) return 1;
    for (size_t i = 0; FR_FUNC[i]; ++i)
        if (strcmp(s, FR_FUNC[i]) == 0) return 1;
    for (size_t i = 0; FR_PARTICLES[i]; ++i)
        if (strcmp(s, FR_PARTICLES[i]) == 0) return 1;
    return 0;
}

/* ②号门：成形词。返回 1 = 可以当槽位取值。
 *   - 至少要两个汉字（单字词在语料里多半是切词碎片；白名单里的单字词
 *     如 猫/狗/雪/火 由 Gate A 负责，不走这里）；
 *   - 不能以语气/结构助词收尾（「累了吗」这种是切词碎片，不是词）。 */
static inline int fr_wellformed(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '[') return 0;
    size_t cjk = 0, ascii = 0, len = strlen(s);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xE0) == 0xE0) { cjk++; i += 2; }        /* 3 字节 → 一个汉字 */
        else if (c < 128) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9')) ascii++;
            else return 0;                                /* 标点/符号 */
        }
    }
    if (cjk < 2 && ascii < 2) return 0;
    /* 汉字收尾的语气/结构助词（UTF-8 逐字节比对，别手数字节数）。
     * **用 fr_is_func 那张助词表**（FR_PARTICLES）：以前这里另抄了一份，
     * 和 Gate B 的①号门分家就会漂移。 */
    for (size_t i = 0; FR_PARTICLES[i]; ++i) {
        size_t tl = strlen(FR_PARTICLES[i]);
        if (len >= tl && memcmp(s + len - tl, FR_PARTICLES[i], tl) == 0) return 0;
    }
    return 1;
}

/* 两个符号节点的出边邻居重叠度（Dice×100，0~100）。
 * 必须先按 main 去重：符号节点的边表里同一个邻居会有重复行，
 * 不去重会算出 >100 的分数（写探针时踩过）。 */
static inline int fr_dice(const Token *a, const Token *b) {
    if (!a || !b || a->count == 0 || b->count == 0) return 0;
    size_t na = 0, nb = 0, inter = 0;
    /* **跳过关系边**：这一层要量的是「上下文像不像」（共现统计），
     * 而关系边是 L2 那份人工知识。混进来会稀释：实测 relations.txt 加载后
     * `动物` 的出边多了几条（属于生物/有毛/有腿…），Dice 立刻掉到门槛以下，
     * `你知道动物吗` 就从「我知道动物」退化成降级答复了。
     * 换句说：别让人工知识去污染分布统计。 */
    for (size_t i = 0; i < a->count; ++i) {
        Token *m = a->assocs[i].main;
        if (!m || (m->name && m->name[0] == '[')) continue;
        if (a->assocs[i].rel) continue;
        size_t k;
        for (k = 0; k < i; ++k) if (a->assocs[k].main == m) break;
        if (k < i) continue;
        na++;
        for (size_t j = 0; j < b->count; ++j)
            if (b->assocs[j].main == m) { inter++; break; }
    }
    for (size_t j = 0; j < b->count; ++j) {
        Token *m = b->assocs[j].main;
        if (!m || (m->name && m->name[0] == '[')) continue;
        if (b->assocs[j].rel) continue;
        size_t k;
        for (k = 0; k < j; ++k) if (b->assocs[k].main == m) break;
        if (k < j) continue;
        nb++;
    }
    size_t denom = na + nb;
    return denom ? (int)((2 * inter * 100) / denom) : 0;
}

/* Gate B 本体：候选 cand 能不能靠关联链证据填进「示范集 ex」这个槽位。 */
/* 这个符号节点名是不是**正好一个汉字**。
 * 切词器只在「找不到更长的词」时才按字切，所以「整段都是单字」正是
 * 「用户写了个不存在的复合词」的指纹。这条判据是拿血换来的：
 *   `你 吃 火锅 了 吗`  → 火/锅 都是单字 → 是切词产物 → 可以拼回「火锅」✓
 *   `你 怕 危险 吗`     → 怕(单字) + 危险(两字词) → 不是切词产物 ✗
 *   `你 知道 天气 吗`   → 知道/天气 都是词        → 不是切词产物 ✗
 * 少了这条，模板「你 <X> 吗」会把 `怕危险`、`知道天气` 整个吞进 X 位，
 * 然后拼成新词答出去（`我 不 怕危险`）—— 实测踩过。 */
static inline int fr_one_cjk(const char *s) {
    if (!s) return 0;
    return (strlen(s) == 3) && (((unsigned char)s[0] & 0xF0) == 0xE0);
}

/* 「类成员」覆盖率：候选的出边里，有多少条是**现场造词时接的**类成员边
 * （ASSOC_ROLE_COINED）直接盖住示例集。
 * 为什么不是普通覆盖率：只看邻居会在宽类上误放 —— 实测 `天气` 的出边里有
 * 一大半是「知道X吗」的示范词，普通覆盖率能到 60%+，于是
 * `你知道天气吗` 被放进「知道X吗」的 X 位，答出「我 不 知道天气」。
 * 只认造词接的那种角色，覆盖率就只对**真的接过线**的词成立：
 * 造的时候 100%，存档重载后仍然是 100%（role 字节是写进模型的）。 */
static inline int fr_cover100(const Token *cand, Token *const *ex, size_t ne) {
    if (!cand || ne == 0) return 0;
    size_t hit = 0;
    for (size_t i = 0; i < ne; ++i) {
        for (size_t j = 0; j < cand->count; ++j)
            if (cand->assocs[j].main == ex[i] &&
                cand->assocs[j].role == ASSOC_ROLE_COINED) { hit++; break; }
    }
    return (int)((hit * 100) / ne);
}

#define FR_ASSOC_COVER_MIN 60

static inline int fr_assoc_score_ok(const Token *cand, Token *const *ex, size_t ne) {
    if (!cand || cand->count == 0 || ne == 0) return 0;
    if (fr_cover100(cand, ex, ne) >= FR_ASSOC_COVER_MIN) return 1;
    int best = 0;
    for (size_t i = 0; i < ne; ++i) {
        int d = fr_dice(cand, ex[i]);
        if (d > best) best = d;
    }
    return best >= FR_ASSOC_SIM_MIN;
}

/* 单个词的完整三重门 */
static inline int fr_assoc_admit(const Token *cand, Token *const *ex, size_t ne) {
    if (!cand || !cand->name) return 0;
    if (!fr_wellformed(cand->name)) return 0;      /* ②号门 */
    if (fr_is_func(cand->name)) return 0;          /* ①号门 */
    return fr_assoc_score_ok(cand, ex, ne);
}

/* 多词跨度：整体当成「一个词」来验。
 * 为什么必须单独一条路：`你 吃 火锅 了 吗` 里「火锅」被切词器拆成 火+锅，
 * 而 ②号门（至少两个汉字）会把单字部件 火 直接挡掉 —— 于是帧根本进不来，
 * 现场造词也就永远轮不到。这里改成：**成形性看拼出来的整体**（火锅 ✓），
 * **类的证据看各部件里最强的那一个**（火 对食物类 Dice=89 → 过）。
 * 这条路只在「跨度 ≥2 且全是内容词」时走，而那种跨度几乎只可能来自切词。 */
static inline int fr_span_admit(const FrValue *v, Token *const *ex, size_t ne) {
    if (!v || v->n < 2) return 0;
    char buf[128]; size_t bl = 0;
    for (size_t i = 0; i < v->n; ++i) {
        Token *t = v->tok[i];
        if (!t || !t->name || t->name[0] == '[') return 0;
        if (!fr_one_cjk(t->name)) return 0;            /* 必须整段都是单字 */
        size_t nl = strlen(t->name);
        if (bl + nl + 1 >= sizeof buf) return 0;
        memcpy(buf + bl, t->name, nl); bl += nl;
    }
    buf[bl] = 0;
    if (!fr_wellformed(buf)) return 0;                 /* 拼出来的整体要成形 */
    for (size_t i = 0; i < v->n; ++i)
        if (fr_assoc_score_ok(v->tok[i], ex, ne)) return 1;
    return 0;
}

/* 某个符号节点是不是在一个「允许填充集合」里 */
static inline int fr_in_set(Token *const *set, size_t n, const Token *t) {
    if (!t) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (set[i] == t) return 1;
        /* 多词取值：允许的首词是它也算（比如取值「画画」以「画」开头） */
        if (set[i] && t->name && set[i]->name &&
            strcmp(set[i]->name, t->name) == 0) return 1;
    }
    return 0;
}

typedef struct {
    const char *name;
    FrTemplates ask;                     /* 问法模板 */
    FrTemplates ans;                     /* 回复模板 */
    /* 降级答复：[降级答] 行。特征是**不需要槽位类证据**也能说出口 ——
     * 「我 不 太 懂 <X>」对任何 X 都成立。所以兜底的那一步用它。 */
    FrTemplates fall;
    FrSlot      slot[FR_MAX_SLOT];
    size_t      n_slot;
} Frame;

typedef struct {
    Frame   fr[FR_MAX_FRAMES];
    size_t  n_fr;
    /* 可选：给所有帧共用的「对象槽」候选（由 decl 行累积） */
    Token  *decl[FR_MAX_FILL];
    size_t  n_decl;
} FrameSet;

/* 一帧是怎么被选中的 —— 交给思维链去记。
 * 单独一个结构、而不是让 frame.h 直接写日志：帧层只报告**发生了什么**，
 * 「这件事意味着什么、下一步该干什么」由 agent.h 的思维链决定。 */
typedef struct {
    const char *frame;    /* 命中的帧名（NULL = 没命中） */
    const char *ask_frame;/* **问法**匹配上的帧名（槽位可能没过校验） */
    const char *slot;     /* 靠关联链证据放行的槽位名（NULL = 没用到） */
    int ask_matched;      /* 问法骨架认出来了（不等于最终用了这一帧） */
    int matched;          /* 真匹配上了一帧 */
    int gate_failed;      /* 有槽位卡在类证据校验上 */
    int used_fall;        /* 最终用的是降级答复 */
    int admit;            /* 靠关联链证据放行过 */
    int coined, split, merge, relay;
} FrTrace;

/* ---------------------------------------------------------------- 造词 */

/* 现场造词：槽位取值是「本不存在的复合词」时，把它造成一个符号节点并接进图里。
 *
 * 起因：`你 吃 火锅 了 吗` 里的「火锅」在语料里从没出现过，切词器只能按字切成
 * 火 + 锅。于是槽位取值变成两个单字，帧要么答出「我 吃 了 火 锅」（难看），
 * 要么干脆不认。可**用户的这句话本身就是证据** —— 他把「火锅」放在了
 * 「你 吃 <X> 了 吗」的 X 位上，系统当时就能学到「火锅是这类东西」。
 *
 * 所以：拼回一个词 → 进池子 → **现场关联**：
 *   ① 接到这个槽位声明的全部示范词上（正反两向），覆盖率立刻 100%；
 *   ② 记进这个槽位的白名单 —— 帧的填充表在对话里长大，下次直接走 Gate A。
 *
 * 只对「走 Gate B 才过关的多词跨度」做（调用方保证）：如果 Gate A 就认了，
 * 说明第一个词本身合法，不该硬拼成一个新词（`你 喜欢 雪 花 吗` 里 雪/花
 * 都在白名单，拼成「雪花」是把用户的意思改了）。
 *
 * 三道自我约束：跨度 2~4 个词、每个词都是内容词、拼出来 2~6 个汉字。
 * 返回新词（失败返回 NULL）。 */
static inline Token *fr_coin(AI *ai, Frame *fr, size_t k, FrValue *fill,
                             Token **decl, size_t n_decl) {
    if (!ai || ai->coin_off || !fr || k >= FR_MAX_SLOT) return NULL;
    if (fill[k].n < 2 || fill[k].n > 4) return NULL;
    char buf[128]; size_t bl = 0;
    for (size_t i = 0; i < fill[k].n; ++i) {
        Token *t = fill[k].tok[i];
        if (!t || !t->name || t->name[0] == '[') return NULL;
        if (!fr_one_cjk(t->name)) return NULL;      /* 整段都是单字才是切词产物 */
        size_t nl = strlen(t->name);
        if (bl + nl + 1 >= sizeof buf) return NULL;
        memcpy(buf + bl, t->name, nl); bl += nl;
    }
    buf[bl] = 0;
    if (!fr_wellformed(buf)) return NULL;

    Token *nt = pool_token_intern(ai->pool, buf);
    if (!nt) return NULL;

    Token *const *set = fr->slot[k].fill;
    size_t nset = fr->slot[k].n_fill;
    if (nset == 0) { set = decl; nset = n_decl; }
    for (size_t i = 0; i < nset; ++i) {
        if (!set[i] || set[i] == nt) continue;
        /* 正反两向，角色标成 COINED —— 这条标记就是「这个词属于这一类」的
         * 判据（见 fr_cover100），也是存档重载后仍认得出的依据。 */
        token_tag_full(nt, set[i], ASSOC_ROLE_COINED, ASSOC_TOPIC_NONE,
                       ASSOC_PAT_NONE, FR_COIN_BUMP);
        token_tag_full(set[i], nt, ASSOC_ROLE_COINED, ASSOC_TOPIC_NONE,
                       ASSOC_PAT_NONE, FR_COIN_BUMP);
    }
    int known = 0;
    for (size_t i = 0; i < fr->slot[k].n_fill; ++i)
        if (fr->slot[k].fill[i] == nt) { known = 1; break; }
    if (!known && fr->slot[k].n_fill < FR_MAX_FILL)
        fr->slot[k].fill[fr->slot[k].n_fill++] = nt;
    return nt;
}

/* strdup 不是 C11 标准函数，自己来一个 */
static inline char *fr_dup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---------------------------------------------------------------- 模板切分 */

/* 把 "你 喜欢 <X> 吗" 切成符号节点序列；遇到 <X> 时记下槽位位置。
 * slot_at[k] = 该位置是哪个槽位（-1 表示普通词）。 */
static inline int fr_split(AI *ai, const char *text, Token **out, int *slot_at,
                           size_t max_tok, const char **slot_names, size_t *n_slot,
                           size_t *out_n) {
    *out_n = 0;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", text);
    char *p = buf;
    while (*p && *out_n < max_tok) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '<') {                              /* <X> 槽位 */
            char *close = strchr(p, '>');
            if (!close) break;
            size_t nl = (size_t)(close - p - 1);
            if (nl == 0 || nl > 31) break;
            char nm[32];
            memcpy(nm, p + 1, nl); nm[nl] = 0;
            int idx = -1;
            for (size_t i = 0; i < *n_slot; ++i)
                if (strcmp(slot_names[i], nm) == 0) { idx = (int)i; break; }
            if (idx < 0) {
                if (*n_slot >= FR_MAX_SLOT) break;
                slot_names[*n_slot] = fr_dup(nm);
                idx = (int)(*n_slot)++;
            }
            out[*out_n] = NULL;                       /* 槽位占位 */
            slot_at[*out_n] = idx;
            (*out_n)++;
            p = close + 1;
            continue;
        }
        /* 普通词：按空白切一段，交给自己那套切词 */
        char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        char save = *p; *p = 0;
        size_t wn = 0;
        char **w = ai_tokenize(ai, s, &wn);
        for (size_t i = 0; i < wn && *out_n < max_tok; ++i) {
            out[*out_n] = pool_token_intern(ai->pool, w[i]);
            slot_at[*out_n] = -1;
            (*out_n)++;
        }
        ai_free_words(w, wn);
        *p = save;
    }
    return 0;
}

/* ---------------------------------------------------------------- 读帧文件 */

static inline Frame *fr_get_or_add(FrameSet *fs, const char *name) {
    for (size_t i = 0; i < fs->n_fr; ++i)
        if (strcmp(fs->fr[i].name, name) == 0) return &fs->fr[i];
    if (fs->n_fr >= FR_MAX_FRAMES) return NULL;
    Frame *fr = &fs->fr[fs->n_fr++];
    memset(fr, 0, sizeof *fr);
    fr->name = fr_dup(name);
    return fr;
}

static inline FrSlot *fr_slot(Frame *fr, const char *name, int add) {
    for (size_t i = 0; i < fr->n_slot; ++i)
        if (strcmp(fr->slot[i].name, name) == 0) return &fr->slot[i];
    if (!add || fr->n_slot >= FR_MAX_SLOT) return NULL;
    FrSlot *sl = &fr->slot[fr->n_slot++];
    sl->name = fr_dup(name);
    sl->n_fill = 0;
    return sl;
}

/* 解析 frames.txt。返回解析出的帧数。 */
static inline size_t fr_load(AI *ai, FrameSet *fs, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048];
    Frame *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (!l) continue;

        /* 前缀长度一律用 strlen 算，别手数 —— 「帧」是 3 字节，
         * `[帧:` 一共 5 字节，写成 7 会读错偏移（这个坑刚踩过）。 */
        if (strncmp(line, "[帧:", 5) == 0) {
            char *close = strchr(line, ']');
            if (!close) continue;
            char nm[128];
            size_t nl = (size_t)(close - line - 5);
            if (nl >= sizeof nm) nl = sizeof nm - 1;
            memcpy(nm, line + 5, nl); nm[nl] = 0;
            cur = fr_get_or_add(fs, nm);
            continue;
        }
        if (strncmp(line, "[槽:", 5) == 0) {          /* 共用对象候选 */
            char *close = strchr(line, ']');
            if (!close) continue;
            char body[1024];
            size_t nl = (size_t)(close - line - 5);
            if (nl >= sizeof body) nl = sizeof body - 1;
            memcpy(body, line + 5, nl); body[nl] = 0;
            char *p = body;
            while (*p) {
                char *s = p;
                while (*p && *p != ',' && *p != ' ') p++;
                char save = *p; *p = 0;
                if (*s) {
                    /* 用「整段」当符号节点，不交给 ai_tokenize ——
                     * 后者对「画画」会按池里认识的最长词切成 画+画，
                     * 声明里就会变成两个单字，反而匹配不上用户输入的「画画」。 */
                    Token *t = pool_token_intern(ai->pool, s);
                    if (t && fs->n_decl < FR_MAX_FILL) {
                        int dup = 0;
                        for (size_t i = 0; i < fs->n_decl; ++i)
                            if (fs->decl[i] == t) { dup = 1; break; }
                        if (!dup) fs->decl[fs->n_decl++] = t;
                    }
                }
                *p = save;
                if (*p == ',' || *p == ' ') p++;
            }
            continue;
        }
        /* [填:X=<hex>,<hex>,...] —— 每帧给某个槽位声明允许填充集合。
         * 用 hex 编码传中文，避免「按什么切分」的坑。 */
        if (strncmp(line, "[填:", 5) == 0 && cur) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            char snm[32];
            size_t sl = (size_t)(eq - (line + 5));
            if (sl == 0 || sl >= sizeof snm) continue;
            memcpy(snm, line + 5, sl); snm[sl] = 0;
            /* 支持 [填:X#2=...]：#N 指定这个槽位最多吸收 N 个词 */
            size_t maxlen = 0;
            {
                char *hash = strchr(snm, '#');
                if (hash) { maxlen = (size_t)atoi(hash + 1); *hash = 0; }
            }
            FrSlot *sl2 = fr_slot(cur, snm, 1);
            if (!sl2) continue;
            if (maxlen) sl2->max_len = maxlen;
            char *p2 = eq + 1;
            while (*p2) {
                while (*p2 == ',' || *p2 == ' ') p2++;
                if (!*p2) break;
                char *s2 = p2;
                while (*p2 && *p2 != ',') p2++;
                char save2 = *p2; *p2 = 0;
                /* hex 解码 */
                char dec[256]; size_t dn = 0;
                for (char *h = s2; h[0] && h[1] && dn + 1 < sizeof dec; h += 2) {
                    int hi = (h[0] >= '0' && h[0] <= '9') ? h[0] - '0'
                           : (h[0] >= 'a' && h[0] <= 'f') ? h[0] - 'a' + 10
                           : (h[0] >= 'A' && h[0] <= 'F') ? h[0] - 'A' + 10 : -1;
                    int lo = (h[1] >= '0' && h[1] <= '9') ? h[1] - '0'
                           : (h[1] >= 'a' && h[1] <= 'f') ? h[1] - 'a' + 10
                           : (h[1] >= 'A' && h[1] <= 'F') ? h[1] - 'A' + 10 : -1;
                    if (hi < 0 || lo < 0) break;
                    dec[dn++] = (char)((hi << 4) | lo);
                }
                dec[dn] = 0;
                *p2 = save2;
                if (dn) {
                    Token *t2 = pool_token_intern(ai->pool, dec);
                    if (t2 && sl2->n_fill < FR_MAX_FILL) {
                        int dup2 = 0;
                        for (size_t q = 0; q < sl2->n_fill; ++q)
                            if (sl2->fill[q] == t2) { dup2 = 1; break; }
                        if (!dup2) sl2->fill[sl2->n_fill++] = t2;
                    }
                }
                if (*p2 == ',') p2++;
            }
            continue;
        }
        if (!cur) continue;
        /* [降级答] 必须先判 —— 它的前缀和 [答] 不同，但都从 '[' 开始，
         * 顺序反了就会被下面的 [问]/[答] 分支吃掉。长度用 sizeof-1 算，
         * 不手数（「[帧:」只有 5 字节那类坑踩过）。 */
        if (strncmp(line, FR_FALL_PFX, sizeof FR_FALL_PFX - 1) == 0) {
            FrTemplates *T = &cur->fall;
            if (T->n_tmpl >= FR_MAX_TMPL) continue;
            const char *slot_names[FR_MAX_SLOT];
            size_t n_slot = cur->n_slot;
            for (size_t i = 0; i < cur->n_slot; ++i) slot_names[i] = cur->slot[i].name;
            Token *toks[FR_MAX_TOK];
            int slot_at[FR_MAX_TOK];
            size_t tn = 0;
            fr_split(ai, line + sizeof FR_FALL_PFX - 1, toks, slot_at, FR_MAX_TOK,
                     slot_names, &n_slot, &tn);
            /* §6.190 `fr_split` 给**新**槽位名 `fr_dup` 过一份；`fr_slot` 自己会再 dup 一份存进帧，
             * 所以这份临时名要**还回去**（以前没还 ⇒ 每个新槽位泄一条，ASan 实测 28 条/56 字节）。 */
            for (size_t i = cur->n_slot; i < n_slot; ++i) {
                fr_slot(cur, slot_names[i], 1);
                free((void *)slot_names[i]);
            }
            for (size_t i = 0; i < tn; ++i) {
                T->toks[T->n_tmpl][i] = toks[i];
                T->slot_at[T->n_tmpl][i] = (int8_t)slot_at[i];
            }
            T->n_toks[T->n_tmpl] = tn;
            T->n_tmpl++;
            continue;
        }
        if (strncmp(line, "[问] ", 6) == 0 || strncmp(line, "[答] ", 6) == 0) {
            /* '答' 紧跟在 '[' 后面，占 line[1..3]；问 是 0xE9 开头所以天然区分。
             * 之前写成 line[2] 起判，读的是 '答' 的第 2 个字节，永远为假。 */
            int is_ans = ((unsigned char)line[1] == 0xE7 &&
                          (unsigned char)line[2] == 0xAD &&
                          (unsigned char)line[3] == 0x94);
            FrTemplates *T = is_ans ? &cur->ans : &cur->ask;
            if (T->n_tmpl >= FR_MAX_TMPL) continue;
            const char *slot_names[FR_MAX_SLOT];
            size_t n_slot = cur->n_slot;
            for (size_t i = 0; i < cur->n_slot; ++i) slot_names[i] = cur->slot[i].name;
            Token *toks[FR_MAX_TOK];
            int slot_at[FR_MAX_TOK];
            size_t tn = 0;
            fr_split(ai, line + 6, toks, slot_at, FR_MAX_TOK,
                     slot_names, &n_slot, &tn);
            /* 新建的槽位登记进帧（临时名用完就还，同上 §6.190） */
            for (size_t i = cur->n_slot; i < n_slot; ++i) {
                fr_slot(cur, slot_names[i], 1);
                free((void *)slot_names[i]);
            }
            for (size_t i = 0; i < tn; ++i) {
                T->toks[T->n_tmpl][i] = toks[i];
                T->slot_at[T->n_tmpl][i] = (int8_t)slot_at[i];
            }
            T->n_toks[T->n_tmpl] = tn;
            T->n_tmpl++;
            continue;
        }
        if (strncmp(line, "---", 3) == 0) { cur = NULL; continue; }
    }
    fclose(f);
    return fs->n_fr;
}

/* ------------------------------------------------- 中转词（linking word） */

/* 只起连接作用、换一个说法意思不变的那批词：和/跟/与、还是/或者、会/能/可以…
 *
 * 这是**语言知识**，不是从图里学得到的 —— 图只知道「跟」和「和」都跟
 * 猫/狗 共现过，推不出它们可以互换。所以这张表是手写的，和 FR_FUNC 一样。
 *
 * 它解决的问题很具体（实测）：模板 `[帧:Z和Y哪个好]` 写死了「和」，
 * 于是 `猫跟狗哪个好` 匹配不上，答「这个我 不 太 懂」；
 * 容一个中转词就能接住 —— 答「我 觉得 猫 更 好」。 */
static const char *const FR_RELAY[][6] = {
    { "和", "跟", "与", "及", "同", NULL },
    { "还是", "或者", "或是", NULL, NULL, NULL },
    { "觉得", "认为", "以为", NULL, NULL, NULL },
    { "会", "能", "可以", "该", NULL, NULL },
    { "喜欢", "爱", NULL, NULL, NULL, NULL },
    { "什么", "啥", NULL, NULL, NULL, NULL },
    { "怎么", "如何", "怎样", NULL, NULL, NULL },
    { "哪里", "哪儿", NULL, NULL, NULL, NULL },
    { "因为", "由于", NULL, NULL, NULL, NULL },
    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/* a 和 b 是不是同一类中转词（相同也算） */
static inline int fr_relay_same(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;
    for (size_t i = 0; FR_RELAY[i][0]; ++i) {
        int ha = 0, hb = 0;
        for (size_t j = 0; j < 6 && FR_RELAY[i][j]; ++j) {
            if (strcmp(FR_RELAY[i][j], a) == 0) ha = 1;
            if (strcmp(FR_RELAY[i][j], b) == 0) hb = 1;
        }
        if (ha && hb) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 拆解 */

/* 把一个多字词按**字符**切开，逐个 intern 返回。
 * 「拆解」唯一的判据来源：切词器偏爱最长匹配，所以过长的匹配是常见错误 ——
 * 实测 `你 睡 得 好吗`：`好吗` 是一个词典词，于是模板 `你 睡 得 <X> 吗`
 * 永远匹配不上（它要一个独立的 `吗`），而拆开就是 好 + 吗，一拆就中。
 * 返回切出的词数（1 表示切不动）。 */
static inline size_t fr_split_token(AI *ai, const Token *t, Token **out, size_t cap) {
    if (!ai || !t || !t->name || cap == 0) return 0;
    const char *s = t->name;
    if (s[0] == '[') return 0;                    /* 标记词不拆 */
    size_t n = 0;
    char buf[8];
    for (const char *p = s; *p && n < cap; ) {
        unsigned char c = (unsigned char)*p;
        size_t cl = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                  : ((c & 0xF0) == 0xE0) ? 3 : 4;
        /* §6.193 **截断/非法 UTF-8 不许越界读**：上面只按**首字节**猜长度，
         * 而名字可能是半截序列（实测：2 字节的 `\xE7\x8C` 被当成 3 字节 ⇒ memcpy 读越界，
         * ASan 报 heap-buffer-overflow）。剩下多少字节就最多读多少。 */
        size_t rem = strlen(p);
        if (cl > 4 || cl + 1 > sizeof buf || cl > rem) break;
        memcpy(buf, p, cl); buf[cl] = 0;
        Token *nt = pool_token_intern(ai->pool, buf);
        if (!nt) break;
        out[n++] = nt;
        p += cl;
    }
    return n;
}

/* ---------------------------------------------------------------- 渲染 */

/* 取模板第 ti 条第 i 个位置的槽位下标（-1 = 普通词）。
 * 不再用「哨兵字符串」表示槽位 —— 之前那版踩了字符串长度/字节偏移的坑，
 * 直接存下标既简单又不会错。 */
static inline int fr_slot_at(const FrTemplates *T, size_t ti, size_t i) {
    if (!T || ti >= T->n_tmpl || i >= T->n_toks[ti]) return -1;
    return T->slot_at[ti][i];
}

/* 用 fill[] 渲染模板：fill[i] 是第 i 个槽位的取值 */
static inline char *fr_render(const Frame *fr, const FrTemplates *T,
                              size_t ti, const FrValue *fill) {
    if (!fr || !T || ti >= T->n_tmpl) return NULL;
    Token *out[FR_MAX_TOK * 4];
    size_t on = 0;
    for (size_t i = 0; i < T->n_toks[ti] && on < FR_MAX_TOK * 4; ++i) {
        int si = fr_slot_at(T, ti, i);
        if (si >= 0 && fill && fill[si].n > 0) {
            for (size_t t = 0; t < fill[si].n && on < FR_MAX_TOK * 4; ++t)
                out[on++] = fill[si].tok[t];
            continue;
        }
        if (si >= 0) continue;                 /* 没给取值就跳过 */
        out[on++] = T->toks[ti][i];
    }
    (void)fr;
    return ai_tokens_to_text(out, on);
}

/* 用「降级答复」模板渲染一句诚实答复。
 * 和普通答复的区别只有一点：**不要求槽位取值通过类证据校验** ——
 * 「我 不 太 懂 <X>」没有对 X 做任何断言，X 是用户自己的词，照说就行。
 * 返回渲染好的句子（没有降级模板或槽位填不全则 NULL）。 */
/* 把槽位里的**单字部件**拼回一个整词（`北 京` → `北京`）。
 * 和 fr_coin 的区别：这里**不要求类证据** —— 降级答复没有对取值做任何断言，
 * 「我 不 太 懂 北京」里的「北京」纯粹是复述用户说的词，成不成词只看它像不像词。
 * 好处是顺带把用户的新词收进池子（下次 ::think 北京 就是一个符号节点了），
 * 代价是这类词**没有边**，所以不会污染游走。
 * 受 ::coin off 管：不想让它造词就一起关掉。 */
static inline Token *fr_join_fill(AI *ai, FrValue *v) {
    if (!ai || ai->coin_off || !v || v->n < 2 || v->n > 4) return NULL;
    char buf[64]; size_t bl = 0;
    for (size_t i = 0; i < v->n; ++i) {
        Token *t = v->tok[i];
        if (!t || !t->name) return NULL;
        if (!fr_one_cjk(t->name)) return NULL;
        size_t nl = strlen(t->name);
        if (bl + nl + 1 >= sizeof buf) return NULL;
        memcpy(buf + bl, t->name, nl); bl += nl;
    }
    buf[bl] = 0;
    if (!fr_wellformed(buf)) return NULL;
    Token *nt = pool_token_intern(ai->pool, buf);
    if (!nt) return NULL;
    v->tok[0] = nt; v->n = 1;
    return nt;
}

static inline char *fr_try_fall(AI *ai, const Frame *fr, FrValue *fill) {
    if (!fr) return NULL;
    for (size_t a = 0; a < fr->fall.n_tmpl; ++a) {
        int ok = 1;
        for (size_t k = 0; k < fr->fall.n_toks[a] && ok; ++k) {
            int si = fr_slot_at(&fr->fall, a, k);
            if (si >= 0 && (si >= FR_MAX_SLOT || fill[si].n == 0)) ok = 0;
        }
        if (!ok) continue;
        /* 拼整词（能拼就拼，拼不动就照原样渲染） */
        int joined = 0;
        for (size_t k = 0; k < fr->fall.n_toks[a]; ++k) {
            int si = fr_slot_at(&fr->fall, a, k);
            if (si < 0 || si >= FR_MAX_SLOT) continue;
            if (fr_join_fill(ai, &fill[si])) joined = 1;
        }
        char *r = fr_render(fr, &fr->fall, a, fill);
        if (r) { if (joined && ai) ai->n_coined++; return r; }
    }
    return NULL;
}

/* 一个问法模板和输入对齐：返回「用户改了哪些槽位」以及取值。
 * 要求长度一致（问法变体之间本来就该长度一致），逐位比较：
 *   槽位位置 → 记下用户那一侧的符号节点
 *   普通位置 → 必须相同，否则不匹配 */
/* 模板与输入做**严格的位置走法**：
 *   - 普通词必须和输入当前词**完全相同**，否则不匹配；
 *   - 槽位吸收「从当前位置到下一个普通词出现处」之间的所有输入词。
 *
 * 之前用 LCS 对齐，太松：实测「你 会 支持 我 吗」会撞进「你 想 学 <X> 吗」
 * （因为 你/我/吗 三个虚词恰好对上）。严格走法下 会≠想 直接否决。 */
static inline int fr_match_ask(const Frame *fr, const FrTemplates *T, size_t ti,
                               Token *const *words, size_t n,
                               FrValue *out_fill, int allow_relay,
                               int *used_relay) {
    (void)fr;
    size_t m = T->n_toks[ti];
    if (m == 0 || n == 0) return 0;
    size_t wi = 0;
    size_t filled = 0;
    for (size_t k = 0; k < m; ++k) {
        int si = fr_slot_at(T, ti, k);
        if (si < 0) {
            if (wi >= n) return 0;
            if (T->toks[ti][k] != words[wi]) {
                /* 中转词容错：模板写「和」，用户说「跟」，算同一个位置。
                 * 换掉一个连接词不改变句子的骨架，所以这里放行一次。 */
                const char *tw = T->toks[ti][k] ? T->toks[ti][k]->name : NULL;
                const char *uw = words[wi] ? words[wi]->name : NULL;
                if (!allow_relay || !fr_relay_same(tw, uw)) return 0;
                if (used_relay) *used_relay = 1;
            }
            wi++;
            continue;
        }
        /* 槽位：找下一个普通词在输入里的位置 */
        size_t next_fixed = m;
        for (size_t b = k + 1; b < m; ++b)
            if (fr_slot_at(T, ti, b) < 0) { next_fixed = b; break; }
        size_t take = 0;
        if (next_fixed < m) {
            Token *want = T->toks[ti][next_fixed];
            for (size_t t = wi; t < n && t < wi + 4; ++t) {
                if (words[t] == want) { take = t - wi; break; }
                if (allow_relay && words[t] && want &&
                    fr_relay_same(want->name, words[t]->name)) {
                    take = t - wi;
                    if (used_relay) *used_relay = 1;
                    break;
                }
            }
        } else {
            take = n - wi;                       /* 槽位在末尾：吃掉剩下的 */
        }
        if (take == 0) {
            if (wi >= n) return 0;               /* 没东西可填 */
            take = 1;
        }
        /* 多词取值必须是「内容词连排」。
         * 反例：「你 会 支持 我 吗」—— 模板「你 会 <X> 吗」会找上「吗」，
         * 于是 <X> 吞掉「支持 我」两个词，渲染成「我 想 学 支持 我」。
         * 槽位取值是名词/动词短语，里面不该夹着光杆代词或助词：
         * 吞进去的每个词都必须是内容词，否则这次匹配作废。 */
        if (take > 1) {
            for (size_t t = 0; t < take; ++t) {
                const char *nm = words[wi + t] ? words[wi + t]->name : NULL;
                if (!nm || nm[0] == '[' || fr_is_func(nm)) return 0;
            }
        }
        /* 槽位吸收长度上限。目的是逼匹配器选**更贴合的模板**：
         * 「你 觉得 猫 和 狗 哪个 好」里有 `你 觉得` 这个前缀，
         * 如果让 <Z> 吞掉「你 觉得」（3 个词），它会去撞更短的模板
         * 「<Z> 和 <Y> 哪个 好」，把 <Z> 填成「你 觉得」——错的。
         * 实体/动作这类槽位本身就只有 1~2 个词。 */
        size_t cap = fr->slot[si].max_len ? fr->slot[si].max_len : 2;
        if (take > cap) return 0;
        if (wi + take > n) return 0;
        if (out_fill[si].n > 0) return 0;        /* 同槽出现两次，放弃 */
        for (size_t t = 0; t < take; ++t)
            out_fill[si].tok[out_fill[si].n++] = words[wi + t];
        wi += take;
        filled++;
    }
    if (wi != n) return 0;                       /* 输入还有剩 → 不匹配 */
    return filled > 0;                           /* 至少填了一个槽位 */
}

/* 核心：用结构化帧生成一句新话。
 * 返回渲染好的句子（调用方 free），匹配不到返回 NULL。 */
static inline char *fr_generate_one(AI *ai, FrameSet *fs, Token *const *words, size_t n,
                                    int *used_relay, const int *origin,
                                    char **fall_out, FrTrace *tr) {
    if (!ai || !fs || !words || n == 0) return NULL;
    for (size_t i = 0; i < fs->n_fr; ++i) {
        Frame *fr = &fs->fr[i];
        for (size_t q = 0; q < fr->ask.n_tmpl; ++q) {
            FrValue fill[FR_MAX_SLOT];
            memset(fill, 0, sizeof fill);
            if (!fr_match_ask(fr, &fr->ask, q, words, n, fill,
                              ai->relay_off ? 0 : 1, used_relay)) continue;
            /* 问法认出来了 —— 和「最终用了这一帧」是两件事：槽位可能过不了
             * 校验，那时走降级答复，链上要分别记。 */
            if (tr && !tr->ask_matched) { tr->ask_matched = 1; tr->ask_frame = fr->name; }
            /* 对齐强度检查：**固定词要占输入的大头**。
             * 否则一条「你 会 <X> 吗」能把「太阳 离 我们 多 远」整个吞进槽位，
             * 产出「远 别 事 的」这种垃圾（实测踩过）。
             * 要求 固定词数 / 输入词数 >= 1/3。 */
            {
                size_t fixed = 0;
                for (size_t k = 0; k < fr->ask.n_toks[q]; ++k)
                    if (fr_slot_at(&fr->ask, q, k) < 0) fixed++;
                if (fixed * 3 < n) continue;
            }
            /* 槽位取值必须在**声明的允许集合**里，否则不认这次匹配。
             * 这是防止「你 会 <X> 吗」把「支持」当成技能的关键 ——
             * 不限制的话它会对「你会支持我吗」答「我想学支持我」。 */
            {
                /* 允许集合：帧自己的槽位声明优先，否则用文件顶部的全局声明
                 * （[槽:...]）。全局声明是兜底 —— 之前忘了接上，
                 * 于是槽位 n_fill=0、白名单形同虚设。 */
                int bad = 0;
                for (size_t k = 0; k < fr->n_slot && !bad; ++k) {
                    Token *const *set = fr->slot[k].fill;
                    size_t nset = fr->slot[k].n_fill;
                    if (nset == 0) { set = fs->decl; nset = fs->n_decl; }
                    if (nset == 0) continue;                    /* 真的没声明才不限 */
                    if (fill[k].n == 0) continue;
                    if (!fr_in_set(set, nset, fill[k].tok[0]))
                        bad = 1;
                }
                /* Gate B：白名单落空时，改看关联链证据（见文件上半部分的说明）。
                 * 只有在「这个槽位真的声明了示范集」时才敢放行 —— 没示范集
                 * 就没有「类」可参照，放行等于没门槛，那正是 ai_synthesize 的坑。 */
                if (bad && !ai->assoc_slot_off) {
                    int all_ok = 1;
                    unsigned used_b = 0;      /* 位图：哪些槽位是靠 Gate B 过关的 */
                    for (size_t k = 0; k < fr->n_slot && all_ok; ++k) {
                        if (fill[k].n == 0) continue;
                        Token *const *set = fr->slot[k].fill;
                        size_t nset = fr->slot[k].n_fill;
                        if (nset == 0) { set = fs->decl; nset = fs->n_decl; }
                        if (nset == 0) continue;
                        if (fr_in_set(set, nset, fill[k].tok[0])) continue;
                        int ok_k = (fill[k].n >= 2)
                                       ? fr_span_admit(&fill[k], set, nset)
                                       : fr_assoc_admit(fill[k].tok[0], set, nset);
                        if (ok_k) used_b |= (1u << k);
                        else all_ok = 0;
                    }
                    if (all_ok && used_b) { bad = 0; ai->n_frame_assoc++; }
                    /* Gate B 救下来的**多词**槽位：现场造词。
                     * 只对 Gate B 的槽位做 —— Gate A 就认了的说明首词本身合法，
                     * 不该把两个合法词硬拼成新词（会改掉用户的意思）。 */
                    if (all_ok && used_b) {
                        for (size_t k = 0; k < fr->n_slot && k < FR_MAX_SLOT; ++k) {
                            if (!(used_b & (1u << k))) continue;
                            if (fill[k].n < 2) continue;
                            Token *nt = fr_coin(ai, fr, k, fill,
                                                fs->decl, fs->n_decl);
                            if (!nt) continue;
                            fill[k].tok[0] = nt;
                            fill[k].n = 1;
                            ai->n_coined++;
                        }
                    }
                }
                /* 降级答复：类证据过不去，但这条问法有**不需要类证据**的诚实答复
                 * （「我 不 太 懂 <X>」对任何 X 都成立）。不立刻返回，先记下来 ——
                 * 后面还有别的帧，真有骨架能接就不能被降级答复抢先。 */
                if (bad) {
                    if (tr) tr->gate_failed = 1;
                    if (!*fall_out && !ai->fall_off) {
                        char *f = fr_try_fall(ai, fr, fill);
                        if (f) { *fall_out = f; if (tr) tr->used_fall = 1; }
                    }
                    continue;
                }
            }
            /* 拆解的护栏：**不许把拆开的碎片一起塞进同一个槽位**。
             * origin[i] = 这个符号节点是第几个原词拆出来的（-1 = 没拆过）。
             * 反例（实测）：`你 怕 老师 吗` 在拆解候选里变成 你/怕/老/师/吗，
             * 模板「你 怕 <X> 吗」把 老+师 一起吞进 X，而 老 正好在恐惧类白名单里
             * → Gate A 放行 → 答出「我 怕 老 师」。槽位取值是两个碎片，
             * 明显不对。
             * 正例：`你 睡 得 好吗` 拆成 你/睡/得/好/吗 —— 好 进槽位、
             * 吗 落在**固定词**位置上，槽位里只有一个碎片，照样放行。
             * 一句话：拆解是为了让骨架对上，不是为了把词拆碎塞进槽位。 */
            if (origin) {
                int frag = 0;
                for (size_t k = 0; k < fr->n_slot && !frag; ++k) {
                    if (fill[k].n < 2) continue;
                    int o = -1, same = 1;
                    for (size_t t = 0; t < fill[k].n; ++t) {
                        /* 恢复出每个填充符号节点在本次输入里的位置 */
                        int oi = -1;
                        for (size_t q = 0; q < n; ++q)
                            if (words[q] == fill[k].tok[t]) { oi = origin[q]; break; }
                        if (oi < 0) { same = 0; break; }
                        if (o < 0) o = oi;
                        else if (o != oi) { same = 0; break; }
                    }
                    if (same && o >= 0) frag = 1;
                }
                if (frag) continue;
            }
            /* 选回复模板：不只要「槽位都能填」，还要**和问法内容一致**。
             * 实测不挑的话，「你 会 支持 我 吗」会落到答案「我 想 学 <X>」
             * （因为该帧第一个可渲染的答复就是它），出来是「我 想 学 支持 我」。
             * 挑法：答案里出现、而问法里没有的**实词**越少越好。 */
            size_t ask_words[48]; size_t n_askw = 0;
            for (size_t k = 0; k < fr->ask.n_toks[q] && n_askw < 48; ++k)
                if (fr_slot_at(&fr->ask, q, k) < 0 && fr->ask.toks[q][k])
                    ask_words[n_askw++] = (size_t)fr->ask.toks[q][k]->id;
            int best_a = -1; long best_pen = -1, best_sc = -1, best_nc = -1;
            for (size_t a = 0; a < fr->ans.n_tmpl; ++a) {
                int ok = 1;
                for (size_t k = 0; k < fr->ans.n_toks[a] && ok; ++k) {
                    int si = fr_slot_at(&fr->ans, a, k);
                    if (si >= 0 && fill[si].n == 0) ok = 0;
                }
                if (!ok) continue;
                /* 只比**实词**，虚词一律不算，而且**按不重复的词**算 ——
                 * 这一维是踩过三次才定下来的：
                 *   ① 只比「新实词少」：「你 会 <X> 吗」下
                 *      「我 想 学 <X>」(新 想/学) 和「我 会 试 着 <X>」(新 试)
                 *      会平票（着/会 都是虚词），平票按声明顺序取第一条，
                 *      于是答成「我 想 学 支持 我」。
                 *   ② 改成「先比共用词多」又过头：虚词也能共用，
                 *      「你 知道 <X> 吗」会去选「你 能 给 我 讲 讲 <X> 吗」
                 *      （你/吗 两个虚词都共用），把正确的「我 知道 <X>」挤掉。
                 *   ③ 再改成「只比实词」但按**出现次数**算，
                 *      「你 在 学 <X> 吗」会选「我 学 <X> 学 得 很 慢」
                 *      （学 出现两次，共用计数虚高），把「我 在 学 <X>」挤掉。
                 * 结论：先比**共用的不同实词**多，再比**新实词**少，最后才看总新词数。 */
                long shared_c = 0, new_c = 0, pen = 0;
                size_t seen_id[FR_MAX_TOK]; size_t n_seen = 0;
                for (size_t k = 0; k < fr->ans.n_toks[a]; ++k) {
                    if (fr_slot_at(&fr->ans, a, k) >= 0) continue;
                    Token *t = fr->ans.toks[a][k];
                    if (!t || !t->name) continue;
                    int in_ask = 0;
                    for (size_t z = 0; z < n_askw; ++z)
                        if (ask_words[z] == (size_t)t->id) { in_ask = 1; break; }
                    if (!in_ask) pen++;
                    if (fr_is_func(t->name)) continue;
                    int dup = 0;
                    for (size_t z = 0; z < n_seen; ++z)
                        if (seen_id[z] == (size_t)t->id) { dup = 1; break; }
                    if (dup) continue;                     /* 同一个词只算一次 */
                    if (n_seen < FR_MAX_TOK) seen_id[n_seen++] = (size_t)t->id;
                    if (in_ask) shared_c++;
                    else new_c++;
                }
                if (best_a < 0 || shared_c > best_sc ||
                    (shared_c == best_sc && new_c < best_nc) ||
                    (shared_c == best_sc && new_c == best_nc && pen < best_pen)) {
                    best_a = (int)a;
                    best_sc = shared_c; best_nc = new_c; best_pen = pen;
                }
            }
            if (best_a >= 0) {
                char *r = fr_render(fr, &fr->ans, (size_t)best_a, fill);
                if (r) {
                    ai->n_frames++;
                    if (tr) { tr->matched = 1; tr->frame = fr->name; }
                    return r;
                }
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------- 切分搜索（拼接/拆解） */


/* 同一条输入，**切分方式不止一种**，而哪种对要看「能不能被理解」。
 * 切词器只给一个答案（最长匹配），这里把它当成一个搜索：
 *
 *   ① 主切分     —— 切词器的答案，先试它（不改变现有行为）
 *   ② 拆解候选   —— 把某个多字词按字拆开（切词器偏爱最长匹配，
 *                   所以「匹配过长」是最常见的错误）
 *   ③ 合并候选   —— 把相邻 2~3 个词拼起来，**但拼出来的必须已经是池子里的词**
 *                   （用户在词中间敲了空格，或者早先现场造过这个词）
 *
 * 判定标准就是「这一版切分能不能让结构化帧渲染出一句话」—— 能就是好切分。
 * 这就是「如果判定应该拆解就拆解、该合并就合并」。都不行就返回 NULL，
 * 交回检索/游走，用主切分处理。
 *
 * 为什么把判定交给帧：帧是唯一能对「骨架是否合法」表态的层。检索只比整句相似度，
 * 切分怎么变它都差不多；逐词游走更是不管切分对错都往前走。 */
static inline char *fr_generate(AI *ai, FrameSet *fs, Token *const *words, size_t n,
                                FrTrace *tr) {
    if (!ai || !fs || !words || n == 0) return NULL;
    int ur = 0;
    char *fall_out = NULL;
    char *r = fr_generate_one(ai, fs, words, n, &ur, NULL, &fall_out, tr);
    if (ur) ai->n_relay++;
    if (r) return r;
    if (ai->seg_off) {
        if (fall_out) { ai->n_frames++; ai->n_hedge++; }
        return fall_out;                 /* 不换切分了，但降级答复还是要用 */
    }

    Token *v[FR_MAX_TOK * 4];
    int org[FR_MAX_TOK * 4];

    /* ② 拆解：逐个多字词试着拆开 */
    for (size_t i = 0; i < n; ++i) {
        Token *parts[FR_MAX_TOK * 4];
        size_t np = fr_split_token(ai, words[i], parts, FR_MAX_TOK * 4);
        if (np < 2 || np > 4) continue;
        if (n - 1 + np > FR_MAX_TOK * 4) continue;
        size_t k = 0;
        for (size_t j = 0; j < i; ++j) { v[k] = words[j]; org[k] = -1; k++; }
        for (size_t j = 0; j < np; ++j) { v[k] = parts[j]; org[k] = (int)i; k++; }
        for (size_t j = i + 1; j < n; ++j) { v[k] = words[j]; org[k] = -1; k++; }
        ur = 0;
        r = fr_generate_one(ai, fs, v, k, &ur, org, &fall_out, tr);
        if (ur) ai->n_relay++;
        if (r) { ai->n_split++; return r; }
    }

    /* ③ 合并：相邻 2~4 个词拼成一个**池子里已有的**词。
     * 上界取 4 是因为「人工智能」正好 4 个字：切词器只认词典，而
     * 「人工智能」是 frames.txt 里 intern 出来的，词典里没有 ——
     * 于是 `你知道人工 智能吗` 被切成 人/工/智/能 四个单字，
     * 只有 allow 4 才能拼回去。 */
    for (size_t i = 0; i + 1 < n; ++i) {
        for (size_t take = 2; take <= 4 && i + take <= n; ++take) {
            char buf[256]; size_t bl = 0; int ok = 1;
            for (size_t j = 0; j < take; ++j) {
                Token *t = words[i + j];
                if (!t || !t->name || t->name[0] == '[') { ok = 0; break; }
                size_t nl = strlen(t->name);
                if (bl + nl + 1 >= sizeof buf) { ok = 0; break; }
                memcpy(buf + bl, t->name, nl); bl += nl;
            }
            if (!ok) continue;
            buf[bl] = 0;
            Token *m = pool_find(ai->pool, buf);
            if (!m) continue;             /* 池子里没这个词 → 不许合并 */
            size_t k = 0;
            for (size_t j = 0; j < i; ++j) { v[k] = words[j]; org[k] = -1; k++; }
            v[k] = m; org[k] = -1; k++;
            for (size_t j = i + take; j < n; ++j) { v[k] = words[j]; org[k] = -1; k++; }
            ur = 0;
            r = fr_generate_one(ai, fs, v, k, &ur, org, &fall_out, tr);
            if (ur) ai->n_relay++;
            if (r) { ai->n_merge++; return r; }
        }
    }
    /* 真骨架一个都没接住，才轮到降级答复。 */
    if (fall_out) { ai->n_frames++; ai->n_hedge++; }
    return fall_out;
}

#endif /* FRAME_H */
