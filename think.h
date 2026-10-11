/* think.h — 思维循环（deliberation）
 *
 * 这一层回答的是：「它**怎么想**，而不只是它答了什么」。
 *
 * 与既有的思维链（ai.h 的 ChainOfThought）的区别，必须说清楚：
 *   - COT 是**一轮输出过程**的记录：读入→骨架→校验→降级→成句（四种策略咬合）。
 *   - think 是一段**有议程的循环**：立目标 → 取一个目标 → 挑一个手段去够 →
 *     够不着就把目标**拆成子目标**压回议程 → 再取下一个 → 直到有结论、
 *     或没进展（停滞）、或步数预算用完。停下来的**原因**要写明。
 * 前者回答「这一步在干什么」，后者回答「它在追什么、追到哪、为什么停下」。
 *
 * 铁律（和 derive.h 的立场一致）：
 *   ① **想出来的不等于真的**。推出来的结论只是「在这个知识库下成立」，
 *      而且必须经过验证器（der_verify）复核、`gaps == 0` 才敢当结论用。
 *   ② **思维不往图里写猜出来的东西**。假设、推测、传递出来的结论都只是
 *      「这一轮的想法」；只有**观测**（用户说的、外部补丁给的）才配进图。
 *      思维唯一能改脑的方式是往**修改意图队列**里提议，
 *      由安全内核审查后落地（见 micro.h）——而且提议的只有两类：
 *        · 把「想不通的问题」登记成一个可辨识的节点（[自问:…]）
 *        · 把图里**已有证据**（邻居重叠）却还没写下来的连结固化
 *   ③ **想不通是合法结果**。「不知道」比编一个答案强，这是这个项目一贯的立场。
 *
 * 跨轮：想不通的问题会挂进议程（`Think.open`），后续轮次拿新的知识再试；
 * 试够次数或放太久就过期，并留下提醒 —— 元认知不只看主认知，也看**思维本身**
 * （原地打转 / 挂太多问题 / 知识自相矛盾）。
 *
 * 依赖：token.h、ai.h、micro.h（意图队列）、rel.h、derive.h（求解与验证）。
 * 不含：输出策略（默认不让思维接管输出；接管只处理两种既有流水线给不出的情况：
 *       知识自相矛盾、以及「上次没想通的问题现在想通了」）。
 */
#ifndef THINK_H
#define THINK_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ============================ 常量 ============================ */

#define THINK_MAX_STEP   24     /* 一次思考最多走多少步（硬上限） */
#define THINK_DEF_STEPS  12     /* 默认步数预算 */
#define THINK_MAX_GOAL   24     /* 议程容量 */
#define THINK_MAX_OPEN   16     /* 最多同时挂多少个未解问题 */
#define THINK_TRACE_MAX  32     /* 一步一条，留痕 */
#define THINK_TEXT       96     /* 步骤「在做什么」 */
#define THINK_WHY       224     /* 步骤「为什么」/ 结论投影（中文一字 3 字节，别按字符估） */
#define THINK_CONCL     256     /* 结论的句子 */
#define THINK_OPEN_TTL  600     /* 未解问题放多少 tick 过期 */
#define THINK_STALL_LIM 3       /* 同一件事试几次没进展就算停滞 */
#define THINK_NAME_MAX   48     /* [自问:…] 这个名字最多多少字节（按 UTF-8 边界截） */
#define THINK_CACHE_MAX   8     /* 一轮最多沉淀/撤销几条（和意图队列的预算一起限流） */
#define THINK_DRAFT_MAX   3     /* N2 轮中草稿：一轮最多往草稿区写几行（中间步） */
#define THINK_REDO_MAX    2     /* N3 回溯：一轮最多退几次（防止退来退去原地转） */

/* 一条「事实」：A -[rid]-> B 还是它的否定
 * ev/hops/cred 是**候选自己的证据**（只给"AI 自己想沉淀"那条路用，见 §6.43）：
 *   ev   证据档（THINK_EV_*）：候选至少是规则推导
 *   hops 推导链长（步数）：每多一跳多一个可能出错的地方
 *   cred 链底那条边的逐条可信度（der_base_credibility，0..85；50 = 中性/不知道）
 * 这三个数**不存盘**（存档格式不动）—— 它们是这一轮 deliberation 的产物。 */
typedef struct {
    Token  *a, *b;
    uint8_t rid, neg;
    uint8_t ev, hops;
    uint8_t cred;
} ThinkFact;

/* 候选证据（在 think_finish 那条路上传递；hops=0 表示"不是推来的/不知道"） */
typedef struct {
    int ev, cache, hops, cred;
} ThinkCand;

/* 造一个候选证据（原地一行，省得每个调用点写四行赋值） */
static inline ThinkCand think_cand_make(int ev, int cache, int hops, int cred) {
    ThinkCand c;
    c.ev = ev; c.cache = cache; c.hops = hops; c.cred = cred;
    return c;
}

/* ============================ 类型 ============================ */

typedef enum {
    TH_READ = 1,     /* 读入：把这句话变成要弄清的事 */
    TH_GOAL,         /* 立目标 */
    TH_LOOKUP,       /* 查：直接查图（含假设覆盖层） */
    TH_DERIVE,       /* 推：交给求解器（传递/继承/逆/缺前提），再用验证器复核 */
    TH_ANALOGY,      /* 类比：最近邻借答案（**是猜的**，要标出来） */
    TH_CONFLICT,     /* 查矛盾：反方向是不是也推得出来 */
    TH_DECOMPOSE,    /* 分解：够不着 → 拆成子目标压回议程 */
    TH_LEARN,        /* 想通了 / 登记：结果落到哪 */
    TH_ASK,          /* 自问：想不通 → 挂一个未解问题 */
    TH_REFLECT,      /* 反省：证据强度 vs 把握，决定说还是不说 */
    TH_REST,         /* 停顿：这一步没推进（停滞计数的来源） */
    TH_STOP          /* 收束：为什么停 */
} ThinkKind;

static inline const char *think_kind_name(ThinkKind k) {
    switch (k) {
    case TH_READ:      return "读入";
    case TH_GOAL:      return "立目标";
    case TH_LOOKUP:    return "查";
    case TH_DERIVE:    return "推";
    case TH_ANALOGY:   return "类比";
    case TH_CONFLICT:  return "查矛盾";
    case TH_DECOMPOSE: return "分解";
    case TH_LEARN:     return "登记";
    case TH_ASK:       return "自问";
    case TH_REFLECT:   return "反省";
    case TH_REST:      return "停顿";
    case TH_STOP:      return "收束";
    default:           return "?";
    }
}

/* 停止原因：**必须写明**，否则「想完了」和「想不下去」看起来一样 */
typedef enum {
    TH_STOP_NONE = 0,
    TH_STOP_CONCLUDED,   /* 有结论了（真/假） */
    TH_STOP_GUESSED,     /* 只有类比推测（标成猜的） */
    TH_STOP_UNKNOWN,     /* 推不出来 —— 诚实地停 */
    TH_STOP_CONFLICT,    /* 正反都推得出来：知识自相矛盾 */
    TH_STOP_NO_GOAL,     /* 这一轮没有要弄清的事（普通陈述句） */
    TH_STOP_BUDGET,      /* 步数预算用完 */
    TH_STOP_STALL,       /* 原地打转（连续几步没有推进） */
    TH_STOP_OFF          /* 思维循环关着 */
} ThinkStop;

static inline const char *think_stop_name(ThinkStop s) {
    switch (s) {
    case TH_STOP_NONE:      return "-";
    case TH_STOP_CONCLUDED: return "有结论";
    case TH_STOP_GUESSED:   return "只有猜测（标明是猜的）";
    case TH_STOP_UNKNOWN:   return "推不出来";
    case TH_STOP_CONFLICT:  return "正反都能推出来（矛盾）";
    case TH_STOP_NO_GOAL:   return "没有要弄清的事";
    case TH_STOP_BUDGET:    return "步数预算用完";
    case TH_STOP_STALL:     return "原地打转";
    case TH_STOP_OFF:       return "思维循环关着";
    default:                return "?";
    }
}

/* 目标：议程上的一件事 */
typedef enum {
    TG_REL = 1,      /* A -[rid]-> B 成不成立（want_neg = 问的是不是否定） */
    /* 扩展机制用这个值：调度按**注册表下标**走，不按 kind，
     * 所以外部机制可以共用一个 kind（见 think_mech_register）。 */
    TG_EXT = 100,
    TG_OPPOSITE,     /* 换问法（机制：R7/R8/R9 这类规则只在问否定时开火） */
    TG_ANALOGY,      /* 借一个最近的词的意见（机制：要求表示层在） */
    TG_NEED,         /* 缺哪条前提（**由关系表声明 rd->prereq 决定**） */
    TG_PREMISE,      /* 问前提：这条连结有前提 token，前提成立吗（R10 的闸门） */
    TG_THROUGH,      /* 经由一个**图里给出的**中间词（由 rd->trans / rd->inherit 决定） */
    TG_WORD          /* 这个词我不认识 —— 认识一下（挂成未解问题） */
} ThinkGoalKind;

typedef struct {
    ThinkGoalKind kind;
    Token *a, *b;
    uint8_t rid, want_neg;
    int  depth;
    /* TG_THROUGH 用：中间词、它那条边是不是来自假设层、第一跳走哪个关系
     * （传递 = 同一个关系；继承 = 先走「属于」）。都由表和图给，不是这里写死的。 */
    Token  *via;
    uint8_t via_hyp, via_rel;
    /* 这个目标由**注册表里的哪条机制**处理（-1 = 核心的「查/推」那条路）。
     * 按下标调度而不是按 kind：外部机制就不用去抢内核的枚举值。 */
    int     mech;
} ThinkGoal;

typedef struct {
    ThinkKind kind;
    int       ok;
    int       score;          /* 0..100 */
    char      what[THINK_TEXT];
    char      why[THINK_WHY];
} ThinkStep;

/* 未解问题：跨轮活着 */
typedef struct {
    int      active;
    Token   *subj, *obj;      /* 关系的两端（可以只有 subject） */
    uint8_t  rid, want_neg;
    uint64_t born_tick, last_try;
    size_t   tries;
    int      stalled;         /* 试够了次数，停止再试（但不删） */
    char     text[THINK_TEXT];/* 原话（给人看的） */
} ThinkOpen;

/* **思考对象**（`[思考:N]`，N1）：一轮思考的产物 —— 目标 + 结构化步骤 + 复核结果。
 * 只进内存（`model_io.h` 序列化的是池层轨迹，`Think` 一个字都不进存档 ⇒ 零存档风险）；
 * 默认关（`::thinkobj on` / `AIP_THINKOBJ=1`）。步骤直接用 derive.h 的 `Derivation`：
 * 不新造格式，`::thinkobj` / `::delib` 拿 `der_dump()` 打出来就是那套既有渲染。 */
typedef struct {
    int        have;              /* 这一轮产出对象了吗 */
    uint64_t   seq;               /* N：第几个对象（从 1 起） */
    uint64_t   tick;              /* 哪一轮（pool tick） */
    int        have_goal;         /* 主目标在不在（普通陈述句那轮只有读入） */
    ThinkGoal  goal;              /* 主目标：这一轮要弄清的事 */
    Derivation der;               /* 结构化步骤（FACT/RULE/HYP/DEFAULT/GAP/CONCLUDE…） */
    int        verified;          /* 定稿时跑过 der_verify */
    int        gaps;              /* 复核结果：缺口数（0 = 站得住） */
    int        guessed;           /* 对象里有"猜的"步骤 */
    size_t     n_trace;           /* 定稿时本轮 trace 有几条（痕迹↔对象对得上） */
    char       stop[64];          /* 停因（文本） */
    char       concl[THINK_CONCL];/* 结论投影的快照 */
    char       chain[THINK_WHY];  /* 依据链的快照 */
} ThinkObj;

typedef struct {
    /* ---- 议程 ---- */
    ThinkGoal agenda[THINK_MAX_GOAL];
    size_t    ag_head, ag_n;
    ThinkGoal seen[THINK_MAX_GOAL];     /* 进过议程的（去重 + 停滞判断） */
    size_t    seen_n;

    /* ---- 未解问题 ---- */
    ThinkOpen open[THINK_MAX_OPEN];
    size_t    n_open;                   /* 还活着的 */
    size_t    n_asked, n_resolved, n_expired, n_open_blocked;
    size_t    n_restored;               /* 装载时从 [自问:…] 标记里恢复回来的条数 */

    /* ---- 留痕 ---- */
    ThinkStep trace[THINK_TRACE_MAX];
    size_t    trace_n;
    size_t    n_think, n_steps_total;

    /* ---- 这一轮的结果 ---- */
    int       concl_class;              /* 结论的证据强度等级（见 think_evidence_class） */
    int       concl_cache;              /* 结论是不是「缓存」（同一档强度但来源不同） */
    int       other_class;              /* 「反方向」那条链的等级 */
    int       other_cache;              /* 「反方向」那条链是不是缓存 */
    int       concl;                    /* +1 真 / -1 假 / 0 不知道 */
    int       conf;                     /* 0..100 */
    int       guessed;                  /* 结论是类比猜的 */
    int       conflict;                 /* 正反都能推出来 */
    ThinkStop stop;
    int       took_output;              /* 这一轮思维有没有接管输出 */
    int       flipped;                  /* 这一轮把「缓存」改判成了反的（要当场改口） */
    /* ---- 前提（R10 的闸门）留给答复的话 ----
     * 这一轮有连结因为**前提条件不成立**而没用上时，把"为什么"记下来；
     * `::premiseout on` 之后它会**追加**在正常回答后面（默认关，先测量再翻）。 */
    int       premise_blocked;
    char      premise_text[THINK_CONCL];
    int       mention_pending;          /* 有待说的「上次没想通的问题」 */
    char      concl_text[THINK_CONCL];  /* 结论的投影 */
    char      chain_text[THINK_WHY];    /* 依据链 */
    char      chain_text2[THINK_WHY];   /* 矛盾时：另一条链 */
    char      mention_text[THINK_CONCL];/* 要说出口的那句（跨轮想通的） */

    /* ---- 推理沉淀（想出来的东西留下来）----
     * 这一轮想通、并且**现场证据够硬**的事实，收尾时提议沉淀成边上的一条缓存；
     * 被强依据推翻的缓存，收尾时提议撤销。都走意图队列 + 安全内核。 */
    ThinkFact pend_cache[THINK_CACHE_MAX];
    size_t    n_pend_cache;
    ThinkFact pend_retract[THINK_CACHE_MAX];
    size_t    n_pend_retract;
    size_t    n_cache_blocked;      /* 沉淀的账在 AI 上（n_cached/n_retracted）：来源档可信度要用 */
    size_t    n_stale;                  /* 「前提没了」被撤掉的缓存条数 */

    /* ---- N1：**思考对象**（`[思考:N]`）----
     * 一轮思考除了留痕，还落一个对象：目标 + 结构化步骤（`Derivation`）+ 复核结果。
     * 只进内存（不动存档）；默认关（`::thinkobj on` / `AIP_THINKOBJ=1`）。
     * 关着时这条路一行都不走 —— trace/::delib 与今天逐字节相同。 */
    ThinkObj  obj;
    int       obj_on;                   /* 1 = 产出对象（默认 0） */
    uint64_t  n_obj;                    /* 一共产出过几个对象（`[思考:N]` 的 N） */

    /* ---- N4：**空闲思考**（没人说话时自己挑一个未解问题来想）----
     * `think_run` 的第二条入口：只从 `Think.open`（`[自问:…]` 那批）里挑，**只读**
     * （临时打开 `learn_off` ⇒ 不提议任何意图；也不调 think_settle ⇒ 不沉淀/不撤销）。
     * 默认关（`idle_every == 0`）；节拍 `::idle every <N>` / `AIP_IDLE=1`。 */
    int       idle_every;               /* 0 = 关；>0 = 每 N 个 tick 最多空闲想一次 */
    uint64_t  idle_last_tick;
    size_t    n_idle;                   /* 空闲想了几轮 */
    char      idle_note[THINK_TEXT];    /* 最近一次挑的是哪个问题（给人看） */

    /* ---- N2：**轮中草稿**（一边想一边把中间步写进草稿区）----
     * 缺口（§6.54 N2）：草稿只在**轮末**打（think_settle 是"想完 → 写草稿"）；
     * 现在把中间步也写进去 —— 走**受审通道**（MOD_PATCH_CALL 调 `记事本` 的 `写`），
     * 不是直写（§6.55 K4）。默认关（`draft_mid == 0`）。 */
    int       draft_mid;                /* 1 = 轮中打草稿（默认 0） */
    size_t    n_draft_mid;              /* 这一轮写了几行（每轮清零） */
    size_t    n_draft_total;            /* 累计写了几行（给人看） */

    /* ---- N3：**回溯 + 在线修订**（思考循环层面的 save/restore）----
     * 缺口（§6.54 N3）：走到一半发现前提不成立 → 今天**继续往下想**（不改主意、不回退）。
     * 现在（默认关）：把这一支压进来的子目标**撤回分支起点**，换上**另一条**（同一个问题问反面），
     * 痕上写明"为什么退"；**保留失败分支的痕**，而且**预算不返还**（回溯不许突破预算，K2）。 */
    int       redo_on;                  /* 1 = 允许回溯/在线修订（默认 0） */
    /* **换法扩展**（§6.101，默认 0）：退第二次时不只"问反面"，还试**同类互推**
     * （`甲熊/乙熊 属于 熊类` + `乙熊 会 冬眠` ⇒ 甲熊**大概**会冬眠）—— 这是规则层不做的那一类。 */
    int       redo_alt;
    /* **多层快照栈**（§6.102，默认 0）：现在只有 `snap_n` 一个快照 ⇒ 退来退去都退回**同一个**
     * 分支起点。开着时把"这一支的起点"存进小栈，第二次回退**退到更外一层**（真正的多层回退）。 */
    int       redo_stack;
    size_t    redo_snap[THINK_REDO_MAX];
    int       redo_sp;
    size_t    n_backtrack;              /* 这一轮退了几次（每轮清零） */
    size_t    n_backtrack_total;        /* 累计退了几次（给人看） */

    /* ---- 顺手件②：**机制这条路上的「候选证据」**（链长 / 链底可信度）----
     * §6.43 量出来的缺口：核心那条路（查/推）早就有 hops/cred 了，走**机制注册表**
     * 那条路（换问法 / 经由中间词 / 借最近邻…）却一律按中性（hops=0、cred=50）算，
     * 于是闸门评分里"这条推论有多少底气"对机制结论是瞎的。
     * 这里给内置机制一个**挂证据的口子**：机制把它挂在 `mech_cand` 上，调用点有就用、
     * 没有就按中性 —— **`ThinkMech.process` 的签名不动**（外部注册的机制不用改）。 */
    ThinkCand mech_cand;
    int       mech_cand_set;

    /* ---- 元认知（看思维本身） ---- */
    size_t    n_stalls, n_over_budget, n_conflicts, n_blocked;
    size_t    stall_run;                /* 本轮连续没推进的步数 */

    /* ---- 开关（反向命名：calloc 就是默认值）---- */
    int       run_off;      /* 0 = 每轮都想一遍（默认开） */
    int       out_off;      /* 1 = 不让思维接管输出（默认：先只做内部思考） */
    int       learn_off;    /* 0 = 思维的结果可以改脑（默认开，但只经意图队列） */
    int       cache_off;    /* 0 = 把想通的东西沉淀下来（默认开；::thinkcache off 关） */
    int       premise_out_on; /* 1 = 把「有前提、没成立」追加进答复（默认开；A/B 量过 0/8） */
    /* ---- AI 自己用记事本（输出侧补丁）：0 = 关（默认）/ 1 = 写结论并发送 / 2 = 没结论就按住不说 ----
     * 用户要的「AI 可以控制什么时候发送」。它只在**挂载了 `记事本` 补丁**时才可能生效，
     * 而且默认关 —— 它改变的是"说不说、说什么"，按规矩先实现 + 先测量。 */
    int       notepad;
    size_t    n_notepad;      /* 本轮它自己动了几次笔 */
    /* 逐条可信度是否参与判定（默认关）：打开后，矛盾两边差得多时按更可信的答。 */
    int       trust_on;
    int       trust_c1, trust_c2;   /* 这一轮矛盾两边的可信度（给报告用） */
    int       trust_pick;           /* +1/-1：更信哪一边（0 = 不比） */
    size_t    max_steps;    /* 0 → THINK_DEF_STEPS */
    size_t    stall_limit;  /* 0 → THINK_STALL_LIM */
} Think;

/* 缓存依据复核（定义在下面「推理沉淀」一节）：直查也要用它 ——
 * 求解器里那道实时闸管不到思维层自己扫边的这条路（测试逮到过）。 */
static inline int think_cache_supported(AI *ai, Token *a, uint8_t rid, Token *b, int neg);

/* 机制注册表（定义在下面「机制注册表」一节）：这里只前向声明，
 * 因为 think_of 要保证内建机制已经注册好。 */
static inline void think_mech_init(void);

/* ============================ 取状态 ============================ */

static inline Think *think_of(AI *ai) {
    if (!ai) return NULL;
    think_mech_init();       /* 机制注册表：只注册一次（内建四条） */
    if (!ai->think) {
        Think *t = (Think *)calloc(1, sizeof *t);
        if (!t) return NULL;
        t->max_steps = THINK_DEF_STEPS;
        /* 预算修正：内核**只知道有个数字**（谁算的不问 —— 驱动层在内核之外，见 drive.h）。
         * 层关着时这个数字是 0，于是"层关着"和"开着但一行都没命中"在这里完全一样。 */
        if (ai->drive_d_steps) {
            int v = t->max_steps + ai->drive_d_steps;
            t->max_steps = (v < 3) ? 3 : v;
        }
        t->stall_limit = THINK_STALL_LIM;
        /* 反向命名的坑：calloc 出来是 0，而 out_off 的 0 是「接管开」。
         * 默认要的是**只做内部思考**，所以这里显式置 1
         * （和 micro.h 里 prune_off 一样，默认值写在这儿，不靠结构体清零）。 */
        /* 接管输出**默认开**。这不是拍脑袋，是量出来的：
         * 先按「默认关」把 23 套跑绿，再用 AIP_THINKOUT=1 打开跑一遍 A/B，
         * 结果 8 套里 5 套变红（hypothesis / logic4 / exclusivity / hypo_frames /
         * thinking）。查下来不是输出变差，而是思维层自己有三个真 bug ——
         *   ① `rel_solve` 的 status 是**正关系的真值**，我当成了「问法的答案」；
         *   ② 没分证据强度：独占/闭合世界/假设推出来的东西，被拿去和事实并列判矛盾；
         *   ③ 只查到直接否定就下结论 —— 规则或假设能推翻它（`猫 不会 飞` vs
         *      假设「猫 属于 鸟 + 鸟 会飞 ⟹ 猫 会飞」）。
         * 三个都修掉之后 A/B 变成 7/7 行为套全绿（外加 thinking 自己的默认值断言，
         * 那条已改成显式开关），于是默认才敢翻。详见 docs/thinking.md 第十节。
         *
         * 想反过来量（默认关）：AIP_THINKOUT=0 sh tests/all.sh
         * 这个钩子和 main.c 里的 PAIRDBG 是同一路子：只给测量用，不进业务逻辑。 */
        t->out_off = 0;
        {
            const char *ab = getenv("AIP_THINKOUT");
            if (ab && ab[0] == '1') t->out_off = 0;
            else if (ab && ab[0] == '0') t->out_off = 1;
        }
        /* 「把前提说明追加进答复」**默认开** —— 和 thinkout 一样，是**量出来的**：
         * 先按默认关实现，再用 tests/premiseout_ab.sh 把 8 套行为看护跑两遍，
         * 结果 **0/8 变了**（现成模型/语料里一条前提链接都没有，这个开关对既有会话
         * 本来就是惰性的）；而探针那一侧严格更好：
         *   关：`猫 应该 会 做饭`
         *   开：`猫 应该 会 做饭（不过这件事有个前提：人（没成立） —— 你刚才没提到它…）`
         * 于是默认翻成开。想反过来量（默认关）：AIP_PREMISEOUT=0 sh tests/all.sh
         * 关掉也有命令：`::premiseout off`。 */
        t->premise_out_on = 1;
        /* 「AI 自己用记事本」默认关；测量钩子 AIP_NOTEPAD=on|hold（只给 A/B 用）。 */
        t->notepad = 0;
        t->trust_on = 0;
        /* N1「思考对象」**默认关**：开着才产出 `[思考:N]`（内存对象 + 复核）；
         * 关着时 `::delib` / trace 与今天逐字节相同。测量钩子 AIP_THINKOBJ=1。 */
        t->obj_on = 0;
        t->n_obj = 0;
        { const char *ob = getenv("AIP_THINKOBJ"); if (ob && ob[0] == '1') t->obj_on = 1; }
        /* N4 空闲思考**默认关**（`idle_every = 0`）；`AIP_IDLE=1` = 每个空闲轮都想一次。 */
        t->idle_every = 0;
        t->n_idle = 0;
        t->idle_note[0] = 0;
        { const char *ib = getenv("AIP_IDLE"); if (ib && ib[0] == '1') t->idle_every = 1; }
        /* N2 轮中草稿**默认关**；`AIP_DRAFTMID=1` 打开（还要挂上 `记事本` 补丁才动得了笔）。 */
        t->draft_mid = 0;
        t->n_draft_mid = 0;
        t->n_draft_total = 0;
        { const char *dm = getenv("AIP_DRAFTMID"); if (dm && dm[0] == '1') t->draft_mid = 1; }
        /* N3 回溯 / 在线修订**默认关**；`AIP_THINKREDO=1` 打开。 */
        t->redo_on = 0;
        t->n_backtrack = 0;
        t->n_backtrack_total = 0;
        { const char *rd = getenv("AIP_THINKREDO"); if (rd && rd[0] == '1') t->redo_on = 1; }
        t->redo_alt = 0;
        { const char *ra = getenv("AIP_REDOALT"); if (ra && ra[0] == '1') t->redo_alt = 1; }
        t->redo_stack = 0; t->redo_sp = 0;
        { const char *rsx = getenv("AIP_REDOSTACK"); if (rsx && rsx[0] == '1') t->redo_stack = 1; }
        { const char *tb = getenv("AIP_TRUST"); if (tb && tb[0] == '1') t->trust_on = 1; }
        {
            const char *ab = getenv("AIP_NOTEPAD");
            if (ab && !strcmp(ab, "on"))        t->notepad = 1;
            else if (ab && !strcmp(ab, "hold")) t->notepad = 2;
            else if (ab && !strcmp(ab, "off"))  t->notepad = 0;
        }
        {
            const char *ab = getenv("AIP_PREMISEOUT");
            if (ab && ab[0] == '1') t->premise_out_on = 1;
            else if (ab && ab[0] == '0') t->premise_out_on = 0;
        }
        ai->think = t;
    }
    return (Think *)ai->think;
}

/* ============================ 小工具 ============================ */

/* UTF-8 安全截断：只在完整字符边界上收尾。
 * （这个项目踩过「按字符数估缓冲区，结果截在汉字中间出乱码」，
 *   所以凡是往定长缓冲里放中文，都走这个。） */
static inline void think_utf8_clip(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!src) return;
    size_t i = 0;
    while (src[i] && i + 1 < cap) {
        unsigned char c = (unsigned char)src[i];
        size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (i + len >= cap) break;
        memcpy(dst + i, src + i, len);
        i += len;
    }
    dst[i] = 0;
}

/* 把这一轮的词拼成一句给人看的话（UTF-8 安全） */
static inline void think_words_text(char *const *words, size_t n, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    size_t off = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!words[i] || !words[i][0]) continue;
        int w = snprintf(out + off, cap - off, "%s%s", off ? " " : "", words[i]);
        if (w < 0 || (size_t)w >= cap - off) { out[off] = 0; break; }
        off += (size_t)w;
    }
}

static inline void think_step(Think *t, ThinkKind kind, int ok, int score,
                              const char *what, const char *why) {
    if (!t) return;
    if (t->trace_n >= THINK_TRACE_MAX) {
        /* 满了就丢掉最早的一条，保证最近的总看得见 */
        memmove(&t->trace[0], &t->trace[1], (THINK_TRACE_MAX - 1) * sizeof t->trace[0]);
        t->trace_n = THINK_TRACE_MAX - 1;
    }
    ThinkStep *s = &t->trace[t->trace_n++];
    s->kind = kind;
    s->ok = ok;
    s->score = score;
    think_utf8_clip(s->what, sizeof s->what, what ? what : "");
    think_utf8_clip(s->why, sizeof s->why, why ? why : "");
}

/* ============================ N1：思考对象（`[思考:N]`） ============================
 *
 * 缺口（audit §6.54 的 N1）：一轮思考过后只剩一叠**文本**痕迹（`Think.trace[]`，
 * 轮末清空、不进图、不可复用）—— 于是"这是我刚才那一次的思考"说不出口。
 *
 * 现在：一轮思考（`think_run`）除了留痕，还落一个**对象**：
 *   · 目标：这一轮要弄清的事（主目标）；
 *   · 步骤：**结构化**的链 —— 直接用 derive.h 那套 `Derivation`/`DerStep`，不新造格式；
 *   · 复核：对象定稿时**再跑一遍** `der_verify`（独立重推），把缺口数记在对象上；
 *   · 停因 / 结论投影 / 依据链：这一轮怎么收的。
 * 三条边界（铁律推出来的）：
 *   ① **只进内存**：`model_io.h` 序列化的是池层轨迹，`Think` 一个字都不进存档（§6.55 K1）
 *      ⇒ 这个对象**零存档风险**；
 *   ② **只有观测进图**：对象是"想法"，一个字节都不改图（要沉淀照旧走意图队列 + 安全内核）；
 *   ③ **默认关**：`obj_on == 0` 时这条路一行都不走（trace / `::delib` 与今天逐字节相同）。
 *
 * "可复用"在今天 = 别的层能**读**它（`::delib` 打的就是它、`::thinkobj verify` 能当场复核）；
 * "可存图"**没做** —— 那要动存档格式，得用户单独拍板。 */

static inline void think_obj_reset(Think *t) {
    if (!t) return;
    memset(&t->obj, 0, sizeof t->obj);
    der_reset(&t->obj.der);
}

/* 收一条**结构化**链进对象。收的条件是"比现在这条更好"：
 * 复核缺口更少就换（同样少 = 先到先得，所以主目标的链不会被后来拆出来的子目标挤掉）。
 * `claim` = 这条链敢不敢当结论（给了 1，der_verify 会把结论**独立重推**一遍）。 */
static inline void think_obj_take(AI *ai, Think *t, const ThinkGoal *g,
                                  Derivation *d, int claim) {
    if (!ai || !t || !t->obj_on || !d || d->n == 0) return;
    d->has_claim = claim ? 1 : 0;
    int gaps = der_verify(ai, d);        /* 存的必须是**复核过**的链（verifier 不信构造者） */
    if (t->obj.have && t->obj.der.n > 0 && t->obj.gaps <= gaps) return;
    t->obj.der = *d;                     /* 结构体拷贝：步骤是定长数组，没有指针所有权 */
    t->obj.have = 1;
    t->obj.verified = 1;
    t->obj.gaps = gaps;
    t->obj.guessed = d->guessed ? 1 : 0;
    if (g) { t->obj.goal = *g; t->obj.have_goal = 1; }
}

/* 一轮结束：把对象**定稿**（编号 / 停因 / 结论 / 复核一遍）。
 * 没收到任何链时也定稿，但会在对象里留一条**缺口** —— 照实说"这一轮没接出站得住的链"，
 * 而不是把空对象说成"想通了"。 */
static inline void think_obj_finish(AI *ai, Think *t) {
    if (!ai || !t || !t->obj_on) return;
    ThinkObj *o = &t->obj;
    if (!o->have) {
        der_reset(&o->der);
        if (o->have_goal) (void)der_goal(&o->der, o->goal.a, o->goal.rid, o->goal.b);
        (void)der_gap(&o->der, "这一轮没接出一条站得住的链（停因见下）");
        o->have = 1;
    }
    o->seq = ++t->n_obj;
    o->tick = ai->pool ? ai->pool->current_tick : 0;
    o->n_trace = t->trace_n;
    think_utf8_clip(o->stop, sizeof o->stop, think_stop_name(t->stop));
    think_utf8_clip(o->concl, sizeof o->concl, t->concl_text);
    /* **N1 参与决策**用的一层 AI 级缓存（ai.h 调不到 think_of ⇒ 从这儿转手） */
    if (ai) {
        size_t ck = 0;
        while (ck + 1 < sizeof ai->n1_concl && o->concl[ck]) { ai->n1_concl[ck] = o->concl[ck]; ck++; }
        ai->n1_concl[ck] = 0;
        ai->n1_gaps = o->gaps;
        ai->n1_verified = o->verified;
    }
    think_utf8_clip(o->chain, sizeof o->chain, t->chain_text);
    o->gaps = der_verify(ai, &o->der);   /* **复核器**：定稿时独立重推一遍 */
    o->verified = 1;
    o->guessed = o->der.guessed ? 1 : 0;
}

/* 当场复核（`::thinkobj verify`）：**现在**把对象里的链再独立重推一遍。
 * 和定稿那次是同一个函数、同一条路 —— 不另写一份判据。 */
static inline int think_obj_reverify(AI *ai, Think *t) {
    if (!ai || !t || !t->obj_on || !t->obj.have) return -1;
    t->obj.gaps = der_verify(ai, &t->obj.der);
    t->obj.verified = 1;
    t->obj.guessed = t->obj.der.guessed ? 1 : 0;
    return t->obj.gaps;
}

/* 白箱呈现（`::thinkobj` 与 `::delib` 的那一节**共用这一处**；
 * 步骤用 derive.h 的 der_dump 打 —— 打的是**对象**，不是另一份日志）。 */
static inline void think_obj_print(const AI *ai, const Think *t, const char *ind) {
    if (!t) return;
    const ThinkObj *o = &t->obj;
    const char *p = ind ? ind : "";
    if (!t->obj_on) {
        printf("%s[思考对象] 关着（默认关；`::thinkobj on` / AIP_THINKOBJ=1 打开）\n", p);
        return;
    }
    if (!o->have) {
        printf("%s[思考对象] 还没有：这一轮没产出对象（开关刚打开 / 这一轮思维关着）\n", p);
        return;
    }
    printf("%s[思考对象 #%llu] tick=%llu  结构化步骤 %zu 条  复核：%s，缺口 %d%s  停因：%s\n",
           p, (unsigned long long)o->seq, (unsigned long long)o->tick, o->der.n,
           o->verified ? "跑过 der_verify（独立重推）" : "没跑过",
           o->gaps, o->guessed ? "（含猜测步骤）" : "",
           o->stop[0] ? o->stop : "-");
    if (o->have_goal)
        printf("%s       目标：%s %s%s %s\n", p,
               o->goal.a && o->goal.a->name ? o->goal.a->name : "?",
               o->goal.want_neg ? "不 " : "",
               rel_name_of(ai, o->goal.rid) ? rel_name_of(ai, o->goal.rid) : "?",
               o->goal.b && o->goal.b->name ? o->goal.b->name : "?");
    if (o->n_trace) printf("%s       这一轮的痕迹 %zu 条（对象与痕迹对得上）\n", p, o->n_trace);
    if (o->concl[0]) printf("%s       结论投影：%s\n", p, o->concl);
    if (o->chain[0]) printf("%s       依据：%s\n", p, o->chain);
    printf("%s       步骤（derive.h 那套结构化链，打的是对象本身）：\n", p);
    der_dump(ai, &o->der, DER_STYLE_PLAIN, stdout);
}

/* [自问:…] 这个名字要塞进 ModIntent.name（48 字节），必须**按 UTF-8 边界**截，
 * 否则会把一个汉字截成半个 —— 那种名字在 ::edges / ::tokens 里读出来是乱码。 */
static inline void think_ask_name(char *dst, size_t cap, const char *text) {
    if (!dst || cap == 0) return;
    char clip[THINK_TEXT];
    /* 给 "[自问:"(8 字节) 和 "]" 留位置。名字上限跟着 ModIntent.name 走（现在 96），
     * 所以问句能存下 ~86 字节 —— 以前只有 ~38 字节，恢复出来的问句被截得很厉害。 */
    size_t budget = (cap > 12) ? cap - 12 : cap / 2;
    think_utf8_clip(clip, budget < sizeof clip ? budget : sizeof clip, text ? text : "?");
    /* 名字要能当命令行参数用，所以不留空格（::edges [自问:…] 要打得出） */
    for (char *q = clip; *q; ++q) if (*q == ' ') *q = '_';
    snprintf(dst, cap, MICRO_ASK_TOKEN_PREFIX "%.*s]", (int)budget, clip);
}

/* 提醒走的是元认知那一套（micro.h）：同一种提醒有冷却，不刷屏 */
static inline void think_remind(AI *ai, MicroRemind kind, const char *who) {
    micro_remind(ai, kind, who);
}

/* ============================ 议程 ============================ */

static inline uint32_t think_goal_key(const ThinkGoal *g) {
    if (!g) return 0;
    uint32_t h = 2166136261u;
    #define MIXG(v) do { h ^= (uint32_t)(uintptr_t)(v); h *= 16777619u; } while (0)
    MIXG((uintptr_t)g->kind); MIXG((uintptr_t)g->a); MIXG((uintptr_t)g->b);
    MIXG((uintptr_t)g->rid);  MIXG((uintptr_t)g->want_neg);
    MIXG((uintptr_t)g->via);  MIXG((uintptr_t)g->via_rel);  MIXG((uintptr_t)g->mech);
    #undef MIXG
    return h ? h : 1u;
}

/* 压一个目标进议程：**去过重**（同一件事不重复想），
 * 返回 1 = 真的压进去了（调用方据此判断「这一步有没有推进」）。 */
static inline int think_goal_push(Think *t, ThinkGoalKind kind, Token *a, Token *b,
                                  uint8_t rid, int want_neg, int depth) {
    if (!t) return 0;
    ThinkGoal g;
    memset(&g, 0, sizeof g);
    g.kind = kind; g.a = a; g.b = b; g.rid = rid;
    g.want_neg = (uint8_t)(want_neg ? 1 : 0);
    g.depth = depth;
    g.mech = -1;
    uint32_t key = think_goal_key(&g);
    for (size_t i = 0; i < t->seen_n; ++i)
        if (think_goal_key(&t->seen[i]) == key) return 0;
    if (t->seen_n < THINK_MAX_GOAL) t->seen[t->seen_n++] = g;
    if (t->ag_n >= THINK_MAX_GOAL) return 0;
    t->agenda[t->ag_n++] = g;
    return 1;
}

/* 压一个「由某条注册机制处理」的目标（扩展机制的入口）。 */
static inline int think_goal_push_mech(Think *t, int mech, ThinkGoalKind kind,
                                       Token *a, Token *b, uint8_t rid,
                                       int want_neg, int depth) {
    if (!t) return 0;
    size_t before = t->ag_n;
    if (!think_goal_push(t, kind, a, b, rid, want_neg, depth)) return 0;
    for (size_t i = before; i < t->ag_n; ++i) t->agenda[i].mech = mech;
    if (t->seen_n) t->seen[t->seen_n - 1].mech = mech;
    return 1;
}

static inline int think_goal_pop(Think *t, ThinkGoal *out) {
    if (!t || !out || t->ag_head >= t->ag_n) return 0;
    *out = t->agenda[t->ag_head++];
    return 1;
}
static inline void think_goal_reset(Think *t) {
    if (!t) return;
    t->ag_head = t->ag_n = 0;
    t->seen_n = 0;
}

/* ============================ 未解问题 ============================ */

static inline ThinkOpen *think_open_find(Think *t, Token *subj, Token *obj,
                                         uint8_t rid, int want_neg) {
    if (!t) return NULL;
    for (size_t i = 0; i < THINK_MAX_OPEN; ++i) {
        ThinkOpen *o = &t->open[i];
        if (!o->active) continue;
        if (o->subj != subj || o->obj != obj || o->rid != rid) continue;
        if ((int)o->want_neg != (want_neg ? 1 : 0)) continue;
        return o;
    }
    return NULL;
}

/* 挂一个未解问题。满了就不挂（并记账 —— 「挂太多问题」本身是元认知要看的事）。 */
static inline ThinkOpen *think_open_add(Think *t, Token *subj, Token *obj,
                                        uint8_t rid, int want_neg,
                                        const char *text, uint64_t tick) {
    if (!t) return NULL;
    ThinkOpen *same = think_open_find(t, subj, obj, rid, want_neg);
    if (same) return same;
    if (t->n_open >= THINK_MAX_OPEN) { t->n_open_blocked++; return NULL; }
    for (size_t i = 0; i < THINK_MAX_OPEN; ++i) {
        ThinkOpen *o = &t->open[i];
        if (o->active) continue;
        memset(o, 0, sizeof *o);
        o->active = 1;
        o->subj = subj; o->obj = obj;
        o->rid = rid; o->want_neg = (uint8_t)(want_neg ? 1 : 0);
        o->born_tick = tick; o->last_try = tick;
        think_utf8_clip(o->text, sizeof o->text, text ? text : "");
        t->n_open++;
        t->n_asked++;              /* 累计挂过多少条（和「当前还挂着」分开算） */
        return o;
    }
    return NULL;
}

/* ============================ 从存档里恢复未解问题 ============================
 *
 * 未解问题本身**不进存档**（`Think` 是瞬态的），但思维层在挂问题时已经经安全内核
 * 落了一个 `[自问:<问题文本>]` 标记节点（名字里的空格换成 `_`）并连回主语。
 * 那些标记是**普通 token**，随图存盘 —— 所以「关掉再打开还记得没想通的事」
 * **不用升格式、不用重训**：装载后扫一遍这些标记，把问题重新挂回议程。
 *
 * 局限（照实说）：名字里的文本当初被截断过（think_ask_name 的预算），
 * 恢复出来的是**截断后**的问句；解析不出 (主语,关系,宾语) 的跳过（计数里如实报）。 */
static inline size_t think_restore_pending(AI *ai) {
    if (!ai || !ai->pool) return 0;
    Think *t = think_of(ai);
    if (!t) return 0;
    size_t restored = 0;
    const size_t plen = strlen(MICRO_ASK_TOKEN_PREFIX);
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *tk = ai->pool->tokens[i];
        if (!tk || tk->is_garbage || !tk->name) continue;
        if (strncmp(tk->name, MICRO_ASK_TOKEN_PREFIX, plen) != 0) continue;
        /* 取出问题文本：剥掉 [自问: 前缀与结尾的 ]，把 `_` 还原成空格 */
        char text[THINK_TEXT];
        think_utf8_clip(text, sizeof text, tk->name + plen);
        size_t tl = strlen(text);
        if (tl && text[tl - 1] == ']') text[tl - 1] = 0;
        for (char *q = text; *q; ++q) if (*q == '_') *q = ' ';
        /* 切词（最多 8 个词位，够关系问句用）。
         * ⚠ 必须在**副本**上切：切词会把空格写成 NUL，直接切 text 的话
         *   后面传给 think_open_add 的就只剩第一个词了（实测踩过：恢复出来的问句只有「猫」）。 */
        char seg[THINK_TEXT];
        memcpy(seg, text, sizeof seg);
        seg[sizeof seg - 1] = 0;
        char *w[8];
        size_t nw = 0;
        for (char *q = seg; *q && nw < 8; ) {
            while (*q == ' ') q++;
            if (!*q) break;
            w[nw++] = q;
            while (*q && *q != ' ') q++;
            if (*q) *q++ = 0;
        }
        if (nw < 3) continue;
        char jbuf[64];
        const char *aw = NULL, *bw = NULL, *rw = NULL;
        uint8_t rid = 0;
        int neg = 0, isq = 0;
        /* allow_new = 0：装载只**读**，不许因为恢复个问题就登记新谓语 */
        if (!rel_parse(ai, w, nw, jbuf, sizeof jbuf, &aw, &rid, &neg, &bw, &isq, &rw, 0))
            continue;
        Token *A = pool_find(ai->pool, aw);
        Token *B = bw ? pool_find(ai->pool, bw) : NULL;
        if (!A || !B) continue;
        size_t before = t->n_asked;
        (void)think_open_add(t, A, B, rid, neg, text, ai->pool->current_tick);
        if (t->n_asked > before) restored++;      /* 只数**真的新挂上**的（重复的不算） */
    }
    t->n_restored += restored;
    return restored;
}

static inline size_t think_open_count(const Think *t) { return t ? t->n_open : 0; }

/* ============================ 证据强度 ============================
 *
 * 「两条依据打架」和「一条事实对一条默认规则」是两件事。这个项目里
 * 独占（R6）与闭合世界（R8）都是**非单调的默认规则**（健全但不完备），
 * 假设（DER_HYP）更是临时世界，类比（DER_DEFAULT）只是猜。
 * 拿它们去和**事实**并列判「矛盾」是错的 —— 实测就栽在这儿：
 *   猫 属于 植物 吗   → 独占推出否定（弱），被判成「知识自相矛盾」
 *   假设下 猫 会 飞    → 假设推出肯定（弱）撞上图里的事实，同样被误判
 * 所以矛盾只在**两边都不弱于「规则推导」**时才成立（见 think_finish）。
 *
 *   4 = 观测事实（图里的边；或直接查到的边）
 *   3 = 健全规则推导（传递/继承/对称/逆关系/缺前提）
 *   2 = 非单调默认（独占 / 闭合世界）
 *   1 = 假设世界里的东西
 *   0 = 类比推测
 */
/* 证据强度档直接别名 derive.h 的规则表用的那组数字 —— 一处定义，
 * 免得「规则有多硬」在两处各写一份（那正是滥用硬编码）。 */
#define THINK_EV_FACT    DER_EV_FACT
#define THINK_EV_RULE    DER_EV_RULE
#define THINK_EV_DEFAULT DER_EV_DEFAULT
/* 以前推出来沉淀的（缓存，边上标着 ASSOC_ROLE_DERIVED）。
 * 和「非单调默认」同一档强度，但另有一个 is_cache 标记：
 * 两种弱法的**处理方式不同** —— 缓存被强依据推翻时要**撤销**（它是过时的记忆），
 * 而独占/闭合世界只是「另一种讲法」，撤不掉也不该撤。 */
#define THINK_EV_KNOWN   DER_EV_DEFAULT   /* 与「非单调默认」同档；另用 is_cache 区分 */
#define THINK_EV_HYP     DER_EV_HYP
#define THINK_EV_GUESS   DER_EV_GUESS

static inline const char *think_evidence_name(int cls, int cache) {
    /* 缓存和非单调默认**同一档强度**，但来源不同、处理方式也不同
     * （缓存可被推翻并撤销），所以名字必须分开 —— switch 里不能再按 cls 判。 */
    if (cache) return "推导缓存（以前想通并记住的）";
    switch (cls) {
    case THINK_EV_FACT:    return "观测事实";
    case THINK_EV_RULE:    return "规则推导";
    case THINK_EV_DEFAULT: return "非单调默认（独占/闭合世界）";
    case THINK_EV_HYP:     return "假设";
    case THINK_EV_GUESS:   return "类比推测";
    default:               return "?";
    }
}

/* 一条链有多硬：取链上**最弱**的那一步。缺口的链不该走到这儿（gaps>0 已拦）。 */
static inline int think_evidence_class(const Derivation *d) {
    if (!d) return THINK_EV_GUESS;
    int cls = THINK_EV_FACT;
    for (size_t i = 0; i < d->n; ++i) {
        const DerStep *st = &d->step[i];
        int c = THINK_EV_FACT;
        switch (st->kind) {
        case DER_FACT:    c = THINK_EV_FACT; break;
        case DER_KNOWN:   c = THINK_EV_KNOWN; break;
        case DER_RULE:    c = der_rule_ev(st->rule); break;   /* 强度查**规则表**，不写死 */
        case DER_HYP:     c = THINK_EV_HYP; break;
        case DER_DEFAULT: c = THINK_EV_GUESS; break;
        case DER_SOURCE:  continue;              /* 非逻辑路径的来源：不参与强度 */
        case DER_GOAL:
        case DER_GAP:
        case DER_CONCLUDE: continue;
        default:          continue;
        }
        if (c < cls) cls = c;
    }
    return cls;
}

/* 这条链里有没有**缓存**那一跳？（决定它能不能被强依据推翻并撤销） */
static inline int think_evidence_is_cache(const Derivation *d) {
    if (!d) return 0;
    for (size_t i = 0; i < d->n; ++i)
        if (d->step[i].kind == DER_KNOWN) return 1;
    return 0;
}

/* 回答 → 正关系的真值（think_answer_of 的逆） */
static inline int think_truth_of(int answer, int want_neg) {
    if (answer == 0) return 0;
    return (answer > 0) == (want_neg == 0) ? +1 : -1;
}

/* ============================ 结论的投影 ============================ */

/* ============================ 思维的每一步 ============================
 *
 * **`rel_solve` 的 status 到底是什么**（这一条读错过，A/B 一量就露了）：
 *   它是**正关系 `A rid B` 的真值**：+1 = 真，-1 = 假，0 = 未知。
 *   它**不是**「对某个问法的回答」，也和「问的是不是否定」无关。
 *   证据（rel.h 的 R9/R7/R4 三处赋值）：
 *     R9：只在自己 want_neg 时开火，推出「X 不 会 Y」⟹ 正关系为假 ⟹ status = -1
 *     R7：同理（独占推出否定）⟹ status = -1
 *     R4：`status = want_neg ? -1 : +1` —— 问否定而成立 ⟹ 正关系为假
 *   所以「对 `A [不] rid B 吗` 的回答」要把真值和问法对一下：
 *     问的是肯定（want_neg=0）+ 真值真 ⟹ 对；问的是否定 + 真值假 ⟹ 对。
 */
static inline int think_answer_of(int truth, int want_neg) {
    if (truth == 0) return 0;
    return ((truth == +1) == (want_neg == 0)) ? +1 : -1;
}


/* 查：直接事实（含假设覆盖层）。返回 +1/-1/0。 */
static inline int think_step_lookup(AI *ai, Think *t, const ThinkGoal *g,
                                    char *why, size_t whycap, int *out_class,
                                    int *out_overlay_pol, int *out_cache) {
    int kpos = 0, kneg = 0;
    int pos = rel_direct_as(g->a, g->rid, g->b, &kpos);
    int neg = rel_direct_neg_as(g->a, g->rid, g->b, &kneg);
    int hpos = 0, hneg = 0;
    if (!pos) pos = rel_direct_h(ai, g->a, g->rid, g->b, &hpos);
    if (!neg) neg = rel_direct_neg_h(ai, g->a, g->rid, g->b, &hneg);
    /* **实时闸**：查到的是缓存时，先复核依据；没依据就当没查到
     * （收尾扫描会把它清掉，但在那之前也不能拿它当数）。 */
    if (pos && kpos && !hpos && !think_cache_supported(ai, (Token *)g->a, g->rid, (Token *)g->b, 0))
        pos = 0;
    if (neg && kneg && !hneg && !think_cache_supported(ai, (Token *)g->a, g->rid, (Token *)g->b, 1))
        neg = 0;
    if (out_cache) *out_cache = (hpos || hneg) ? 0 : (pos ? kpos : kneg);
    /* 从**假设覆盖层**里查到的，只算「假设」这一档强度 —— 它不是知识。
     * 同时把「哪一边来自假设」带出去：那边就是假设世界里的答案。 */
    if (out_class) *out_class = (hpos || hneg) ? THINK_EV_HYP
                              : (out_cache && *out_cache) ? THINK_EV_KNOWN
                              : THINK_EV_FACT;
    if (out_overlay_pol) *out_overlay_pol = hpos ? +1 : (hneg ? -1 : 0);
    const char *rn = rel_name_of(ai, g->rid);
    if (pos && neg) {
        if (hpos || hneg)
            snprintf(why, whycap,
                     "正反都有，但**有一边来自假设层**：`%s %s %s` 在假设里成立，"
                     "图里是另一边 —— 那是假设与现实的差别，不是知识矛盾",
                     g->a->name ? g->a->name : "?", rn ? rn : "?",
                     g->b->name ? g->b->name : "?");
        else
            snprintf(why, whycap, "图里**同时**有 `%s %s %s` 和它的否定 —— 知识自相矛盾",
                     g->a->name ? g->a->name : "?", rn ? rn : "?", g->b->name ? g->b->name : "?");
        think_step(t, TH_LOOKUP, hpos || hneg ? 1 : 0, 0, "直接查图", why);
        return 3;   /* 特殊：正反都有（是不是矛盾，看强度） */
    }
    if (pos || neg) {
        int truth = neg ? -1 : +1;              /* 正关系的真值 */
        /* **只查到直接否定时不要立刻下结论**：规则或假设都可能推翻它。
         * 实测栽过这一次 —— 图里有 `猫 不会 飞`，但假设「猫 属于 鸟」加上
         * `鸟 会 飞` 能推出 `猫 会 飞`；直接查图的短路会把它答成「不成立」。
         * 所以这种情况交给求解器（它从 R0 直接事实一路走到规则/假设），
         * 求解器也推不出来时，才会经「试反方向」把那句直接否定作为答案。 */
        if (!pos && neg) {
            snprintf(why, whycap,
                     "图里有直接否定 `%s %s%s %s`，但规则或假设可能推翻它 —— 交给求解器",
                     g->a->name ? g->a->name : "?", "不 ", rn ? rn : "?",
                     g->b->name ? g->b->name : "?");
            think_step(t, TH_LOOKUP, 0, 40, "直接查图", why);
            return 0;
        }
        int answer = think_answer_of(truth, g->want_neg);
        snprintf(why, whycap, "%s `%s %s%s %s`（真值=%s ⟹ 对这个问法答「%s」）",
                 (hpos || hneg) ? "假设层里有"
                 : ((kpos || kneg) ? "图里**记着**（推出来的缓存）" : "图里直接有"),
                 g->a->name ? g->a->name : "?", neg ? "不 " : "", rn ? rn : "?",
                 g->b->name ? g->b->name : "?",
                 truth == +1 ? "正关系成立" : "正关系不成立",
                 answer > 0 ? "对" : "不对");
        think_step(t, TH_LOOKUP, 1, 95, "直接查图", why);
        /* N1：这一手也落进思考对象（FACT 或 HYP + CONCLUDE）——
         * 来自**假设层**的记成 DER_HYP（复核器去覆盖层核对），不是查图那条路。 */
        if (t->obj_on) {
            Derivation od;
            der_reset(&od);
            (void)der_goal(&od, g->a, g->rid, g->b);
            if (hpos || hneg) {
                DerStep *h = der_add(&od, DER_HYP);
                if (h) { h->subj = g->a; h->obj = g->b; h->rid = g->rid;
                         h->neg = (uint8_t)(neg ? 1 : 0); h->truth = truth; }
            } else {
                (void)der_fact(&od, g->a, g->rid, g->b, neg, truth);
            }
            der_conclude(&od, truth, (hpos || hneg) ? 55 : 85);
            think_obj_take(ai, t, g, &od, 1);
        }
        return answer;
    }
    snprintf(why, whycap, "直接查不到 `%s %s%s %s`",
             g->a->name ? g->a->name : "?", neg ? "不 " : "", rn ? rn : "?",
             g->b->name ? g->b->name : "?");
    think_step(t, TH_LOOKUP, 0, 0, "直接查图", why);
    return 0;
}

/* 内置机制报「候选证据」：链长 + 链底可信度（+ 证据档/缓存位）。
 * 外部的自定义机制**不用管这个**：不报就按中性算 —— 与今天逐字节相同。 */
static inline void think_mech_report_cand(Think *t, int ev, int cache, int hops, int cred) {
    if (!t) return;
    t->mech_cand = think_cand_make(ev, cache, hops, cred);
    t->mech_cand_set = 1;
}

/* 推：交给求解器，再用**验证器**独立复核（gaps>0 就不算结论）。 */
static inline int think_step_derive(AI *ai, Think *t, const ThinkGoal *g,
                                    char *chain, size_t chaincap, int *verified,
                                    int *out_class, int *out_cache, ThinkCand *out_cand) {
    Proof pf;
    rel_solve(ai, g->a, g->rid, g->b, g->want_neg, &pf);
    if (verified) *verified = 0;
    if (out_class) *out_class = THINK_EV_GUESS;
    if (out_cache) *out_cache = 0;
    if (out_cand) { out_cand->ev = THINK_EV_GUESS; out_cand->cache = 0;
                    out_cand->hops = 0; out_cand->cred = 50; }
    if (pf.status == 0) {
        char why[THINK_WHY];
        snprintf(why, sizeof why, "求解器给不出依据（%zu 步后停）", pf.n);
        think_step(t, TH_DERIVE, 0, 0, "交给求解器", why);
        return 0;
    }
    Derivation d;
    der_reset(&d);
    der_from_proof(ai, &d, g->a, g->rid, g->b, &pf);
    int gaps = der_verify(ai, &d);
    if (gaps > 0) {
        char why[THINK_WHY];
        snprintf(why, sizeof why, "链上有 %d 处缺口（验证器不通过）—— 有缺口就**不下结论**",
                 gaps);
        think_step(t, TH_DERIVE, 0, 0, "交给求解器", why);
        return 0;
    }
    der_conclude(&d, pf.status, pf.guessed ? 55 : 85);
    char why[THINK_WHY];
    proof_text(ai, &pf, why, sizeof why);
    think_step(t, TH_DERIVE, 1, pf.guessed ? 55 : 85, "交给求解器 + 验证器复核", why);
    /* N1：这条**过了验证器**的链收进本轮的对象（`[思考:N]`）。收的条件见 think_obj_take：
     * 缺口更少的才顶替 —— 先到的（主目标）不会被后拆出来的子目标挤掉。 */
    if (t->obj_on) think_obj_take(ai, t, g, &d, 1);
    if (chain && chaincap) think_utf8_clip(chain, chaincap, why);
    if (verified) *verified = 1;
    if (out_class) *out_class = think_evidence_class(&d);
    if (out_cache) *out_cache = think_evidence_is_cache(&d);
    /* **候选自己的证据**：链长 + 链底那条边多可信（`der_base_credibility` 是现成的量，
     * 不新造判据）。这是闸门"判别力窄"的根因修复 —— 以前这两个数在这里就被丢掉了。 */
    if (out_cand) {
        out_cand->ev = think_evidence_class(&d);
        out_cand->cache = think_evidence_is_cache(&d);
        out_cand->hops = (d.n > 255) ? 255 : (int)d.n;
        out_cand->cred = der_base_credibility(ai, &d);
    }
    return pf.status;   /* 正关系的真值：+1 真 / -1 假（都是「有答案」） */
}

/* 类比：借最近邻的意见。**是猜的，必须标出来**。 */
static inline int think_step_analogy(AI *ai, Think *t, const ThinkGoal *g,
                                     char *chain, size_t chaincap) {
    Proof pf;
    if (!rel_guess(ai, g->a, g->rid, g->b, &pf) || pf.status == 0) {
        think_step(t, TH_ANALOGY, 0, 0, "类比（借最近的词）", "没有哪个近邻对这件事有态度");
        return 0;
    }
    char why[THINK_WHY];
    proof_text(ai, &pf, why, sizeof why);
    think_step(t, TH_ANALOGY, 1, 45, "类比（借最近的词）", why);
    /* N1：类比也落进对象（DER_DEFAULT 步骤；复核器照旧会查"是不是真推不出来 + 近邻够不够近"） */
    if (t->obj_on) {
        Derivation od;
        der_reset(&od);
        der_from_proof(ai, &od, g->a, g->rid, g->b, &pf);
        der_conclude(&od, pf.status, 45);
        think_obj_take(ai, t, g, &od, 1);
        /* 顺手件②：类比这条路的"底气" = 它那份推导的步数与链底可信度 */
        think_mech_report_cand(t, THINK_EV_GUESS, 0,
                               (od.n > 255) ? 255 : (int)od.n, der_base_credibility(ai, &od));
    }
    if (chain && chaincap) think_utf8_clip(chain, chaincap, why);
    return pf.status;
}

/* 查矛盾：**结论的反面是不是也成立**（同样要过验证器）。
 *
 * 这里有两个容易读错的地方，都踩过：
 *   ① `rel_solve(..., want)` 的 `pf.status` 说的是「**问的那个方向**成不成立」，
 *      不是「反向成立」。所以查反面要按**结论的真值**去问：
 *        结论是「P 成立」→ 去问「¬P 成立吗」（want_neg=1，要求 status==+1）
 *        结论是「P 不成立」→ 去问「P 成立吗」（want_neg=0，要求 status==+1）
 *      旧版直接拿 `!want_neg` 再问一遍同一个问题，于是「有答案」被当成了「反面成立」，
 *      独占推出来的**否定**（本来就是答案）被误判成矛盾 —— A/B 一量就露了。
 *   ② 反面的**证据强度**要带出来：弱依据（独占/闭合世界/假设/类比）不与事实并列判矛盾。
 * 返回 1 = 反面成立（调用方再按强度决定算不算矛盾），并把强度写进 *out_class。 */
static inline int think_step_conflict(AI *ai, Think *t, const ThinkGoal *g,
                                      int *out_class, int *out_cache,
                                      char *chain2, size_t chain2cap) {
    if (out_class) *out_class = THINK_EV_GUESS;
    if (out_cache) *out_cache = 0;
    /* 矛盾 ⟺ 「正关系为真」和「正关系为假」**都**推得出来。
     * 两个方向都要问：R9/R7 这类规则只在问否定时才开火。 */
    Proof pf;
    rel_solve(ai, g->a, g->rid, g->b, 0, &pf);
    int true_derivable = (pf.status == +1);
    if (!true_derivable) {
        think_step(t, TH_CONFLICT, 1, 70, "查反面",
                   "「正关系成立」推不出来（好，那就不矛盾）");
        return 0;
    }
    rel_solve(ai, g->a, g->rid, g->b, 1, &pf);
    if (pf.status != -1) {
        think_step(t, TH_CONFLICT, 1, 70, "查反面",
                   "「正关系不成立」推不出来（好，那就不矛盾）");
        return 0;
    }
    Derivation dpos;                       /* 正面这条链先留着：报告里要说清"两边各是哪来的" */
    der_reset(&dpos);
    der_from_proof(ai, &dpos, g->a, g->rid, g->b, &pf);
    (void)der_verify(ai, &dpos);
    Derivation d;
    der_reset(&d);
    der_from_proof(ai, &d, g->a, g->rid, g->b, &pf);
    int gaps = der_verify(ai, &d);
    if (gaps > 0) {
        think_step(t, TH_CONFLICT, 1, 60, "查反面",
                   "反面有成链但验证器不通过 —— 不算矛盾");
        return 0;
    }
    int cls = think_evidence_class(&d);
    if (out_cache) *out_cache = think_evidence_is_cache(&d);
    char why[THINK_WHY];
    proof_text(ai, &pf, why, sizeof why);
    char note[THINK_WHY];
    /* **来源监控**：两边的"底"分别是哪来的（基准知识 / 观测 / 推论 / 假设）——
     * 光说"有矛盾"没用，得说清是"我知道的 vs 我推出来的"还是别的。 */
    /* **逐条可信度**：两边的"底"各有多可信（来源档 + 命中 + 新近算出来的）。 */
    {
        t->trust_c1 = der_base_credibility(ai, &dpos);
        t->trust_c2 = der_base_credibility(ai, &d);
        t->trust_pick = 0;
        if (t->trust_on) {
            int d2 = t->trust_c1 - t->trust_c2;
            if (d2 >= 20)       t->trust_pick = +1;
            else if (d2 <= -20) t->trust_pick = -1;
        }
    }
    snprintf(note, sizeof note,
             "正反都推得出来（正面来自 %s·可信度 %d，反面来自 %s·可信度 %d·依据强度 %s）：%s",
             der_base_source(&dpos), t->trust_c1, der_base_source(&d), t->trust_c2,
             think_evidence_name(cls, out_cache ? *out_cache : 0), why);
    think_step(t, TH_CONFLICT, 0, 0, "查反面", note);
    if (out_class) *out_class = cls;
    if (chain2 && chain2cap) think_utf8_clip(chain2, chain2cap, why);
    return 1;
}

/* 试反方向：问「会」推不出来时，试试「不会」——有时答案就在对面（缺前提那条规则）。 */
static inline int think_step_opposite(AI *ai, Think *t, const ThinkGoal *g,
                                      char *chain, size_t chaincap, int *out_class,
                                      int *out_cache) {
    ThinkGoal o = *g;
    o.want_neg = (uint8_t)(g->want_neg ? 0 : 1);
    int verified = 0;
    /* 注意：这一调用会在痕上留一条 TH_DERIVE（写的是「交给求解器」），
     * 所以紧接着补一条说明，免得看痕的人以为它把方向搞反了。 */
    ThinkCand ocand;
    int d = think_step_derive(ai, t, &o, chain, chaincap, &verified, out_class, out_cache, &ocand);
    /* `d` 是**正关系的真值**（+1 真 / -1 假）。原来的问法推不出来、
     * 换成问反方向就问出来了 —— 那说明「答案就在那个真值上」，
     * 由调用方按原来的问法换算（think_answer_of(d, g->want_neg)）。 */
    if (d != 0 && verified) {
        char why[THINK_WHY];
        snprintf(why, sizeof why,
                 "正方向推不出来；换成问反方向就问出来了（%s）—— 真值是「%s」",
                 rel_name_of(ai, o.rid) ? rel_name_of(ai, o.rid) : "?",
                 d == +1 ? "正关系成立" : "正关系不成立");
        think_step(t, TH_REFLECT, 1, 70, "反方向才是答案", why);
        /* 顺手件②：反方向那条链的链长/链底可信度照实报上去 */
        think_mech_report_cand(t, ocand.ev, ocand.cache, ocand.hops, ocand.cred);
        return d;
    }
    return 0;
}

/* 收下一个结论。**顺手查反方向**：两边都成立就是知识矛盾，
 * 那时候正确的行为不是「挑一条说」，而是不下断言、把矛盾说出来。 */
static inline void think_finish(AI *ai, Think *t, const ThinkGoal *g,
                                int concl, int conf, int guessed, const char *chain,
                                const ThinkCand *cand) {
    /* 候选证据：以前这里只收 (cls, cache) 两个数，链长/链底可信度在这一步被丢掉，
     * 于是闸门评分里根本没有"这条推论有多少底气"（§6.43 量出来的根因）。
     * 现在统一走 `cand`；调用方给 NULL 时按中性算（cred=50、hops 不计分）。 */
    ThinkCand def;
    def.ev = THINK_EV_GUESS; def.cache = 0; def.hops = 0; def.cred = 50;
    if (!cand) cand = &def;
    int cls = cand->ev;
    int cache = cand->cache;
    int truth = think_truth_of(concl, g->want_neg);
    t->concl = concl;
    t->conf = conf;
    t->guessed = guessed;
    t->concl_class = cls;
    /* 注意别名：调用方经常把 t->chain_text 本身传进来，
     * 而 think_utf8_clip 会先清 dst —— 自拷贝会得到空串（踩过）。先过一手临时缓冲。 */
    {
        char tmp[THINK_WHY];
        think_utf8_clip(tmp, sizeof tmp, chain ? chain : "");
        think_utf8_clip(t->chain_text, sizeof t->chain_text, tmp);
    }
    snprintf(t->concl_text, sizeof t->concl_text, "%s %s %s %s",
             g->a && g->a->name ? g->a->name : "?",
             concl < 0 ? "不" : "",
             rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
             g->b && g->b->name ? g->b->name : "?");
    int other_cls = 0, other_cache = 0;
    int opposite_holds = think_step_conflict(ai, t, g, &other_cls, &other_cache,
                                             t->chain_text2, sizeof t->chain_text2);
    t->other_class = other_cls;
    t->other_cache = other_cache;
    t->concl_cache = cache;

    /* **缓存被现场推理推翻**：结论是以前记住的，而对面是现在算出来的强依据 ——
     * 那就听现场的（答案翻过来），并把这条过时的缓存记下来、收尾时撤销。
     * 这和「两条独立依据打架」不是一回事：缓存不是独立依据，它是记忆。 */
    if (opposite_holds && cache && !other_cache && other_cls >= THINK_EV_RULE) {
        int old_truth = truth;
        truth = -truth;
        concl = think_answer_of(truth, g->want_neg);
        t->concl = concl;
        t->conf = 70;
        t->concl_class = other_cls;
        cache = 0;
        if (t->n_pend_retract < THINK_CACHE_MAX) {
            ThinkFact *f = &t->pend_retract[t->n_pend_retract++];
            f->a = g->a; f->b = g->b; f->rid = g->rid;
            f->neg = (uint8_t)(old_truth < 0 ? 1 : 0);
        }
        char note[THINK_WHY];
        snprintf(note, sizeof note,
                 "以前想通并记住的那条被现场推理推翻了（%s vs %s）—— 听现场的，并撤销缓存",
                 think_evidence_name(THINK_EV_KNOWN, 1),
                 think_evidence_name(other_cls, other_cache));
        think_step(t, TH_REFLECT, 1, 70, "缓存被推翻", note);
        t->flipped = 1;      /* 这一轮说出去的话要跟着改口，不然用户先听到旧答案 */
        snprintf(t->concl_text, sizeof t->concl_text, "%s %s %s %s",
                 g->a && g->a->name ? g->a->name : "?",
                 concl < 0 ? "不" : "",
                 rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
                 g->b && g->b->name ? g->b->name : "?");
    }
    /* **弱依据不与事实并列判矛盾**（证据强度，见 think_evidence_class）：
     * 独占/闭合世界是默认规则，假设是临时世界，类比是猜 —— 它们推出反面，
     * 只说明「另一个说法也讲得通」，不说明知识自相矛盾。 */
    int real_conflict = (opposite_holds && cls >= THINK_EV_RULE &&
                         other_cls >= THINK_EV_RULE && !cache && !other_cache);
    if (opposite_holds && !real_conflict) {
        char note[THINK_WHY];
        snprintf(note, sizeof note,
                 "反面也讲得通，但那是**弱依据**（%s vs %s）—— 不算矛盾",
                 think_evidence_name(cls, cache),
                 think_evidence_name(other_cls, other_cache));
        think_step(t, TH_REFLECT, 1, 60, "不是矛盾", note);
    }
    if (real_conflict) {
        t->conflict = 1;
        t->concl = 0;
        t->conf = 0;
        t->stop = TH_STOP_CONFLICT;
        t->n_conflicts++;
        snprintf(t->concl_text, sizeof t->concl_text,
                 "关于 %s %s %s 这件事，我这儿有互相矛盾的依据",
                 g->a && g->a->name ? g->a->name : "?",
                 rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
                 g->b && g->b->name ? g->b->name : "?");
        think_remind(ai, MICRO_REMIND_THINK_CONFLICT, NULL);
    } else {
        t->stop = guessed ? TH_STOP_GUESSED : TH_STOP_CONCLUDED;
    }
    /* **推理沉淀**：这一轮是**现场**用健全规则推出来的（不是缓存、不是猜的），
     * 就把这条事实记下来，收尾时提议沉淀成边上的一条缓存 ——
     * 「想通一件事，就记住它」，下次不用再从头推。
     * 注意：只沉淀，不当观测（role 标成推出来的，可被观测推翻、可撤销）。 */
    if (!t->conflict && !guessed && cache == 0 && cls >= THINK_EV_RULE && g->b) {
        if (t->n_pend_cache < THINK_CACHE_MAX) {
            ThinkFact *f = &t->pend_cache[t->n_pend_cache++];
            f->a = g->a; f->b = g->b; f->rid = g->rid;
            f->neg = (uint8_t)(truth < 0 ? 1 : 0);
            f->ev = (uint8_t)(cand->ev < 0 ? 0 : (cand->ev > 255 ? 255 : cand->ev));
            f->hops = (uint8_t)(cand->hops < 0 ? 0 : (cand->hops > 255 ? 255 : cand->hops));
            f->cred = (uint8_t)(cand->cred < 0 ? 0 : (cand->cred > 255 ? 255 : cand->cred));
        }
    }
}

/* 由图给出的中间词接起来：`a -[via_rel]-> via`（图说成立）且 `via -[rid]-> b`，
 * 则 `a -[rid]-> b`。这是循环比单次求解多出来的地方：每一跳都是一次**独立**求解，
 * 所以串起来的深度不受求解器单次调用的深度上限限制。 */
static inline int think_step_through(AI *ai, Think *t, const ThinkGoal *g,
                                     int *out_class, int *out_cache,
                                     char *note, size_t note_cap) {
    if (out_class) *out_class = THINK_EV_GUESS;
    if (out_cache) *out_cache = 0;
    if (!g->via) return 0;
    ThinkGoal sub;
    memset(&sub, 0, sizeof sub);
    sub.kind = TG_REL; sub.a = g->via; sub.b = g->b;
    sub.rid = g->rid; sub.want_neg = 0; sub.depth = 2; sub.mech = -1;
    int scls = THINK_EV_GUESS, sver = 0, scache = 0;
    ThinkCand subcand;
    char hop[THINK_WHY];
    int st = think_step_derive(ai, t, &sub, hop, sizeof hop, &sver, &scls, &scache, &subcand);
    if (out_cache) *out_cache = scache;
    if (st != +1 || !sver) return 0;
    int hop_cls = g->via_hyp ? THINK_EV_HYP : THINK_EV_FACT;
    int cls = scls < hop_cls ? scls : hop_cls;
    if (out_class) *out_class = cls;
    if (note && note_cap)
        snprintf(note, note_cap, "%s -[%s]-> %s -[%s]-> %s",
                 g->a && g->a->name ? g->a->name : "?",
                 rel_name_of(ai, g->via_rel) ? rel_name_of(ai, g->via_rel) : "?",
                 g->via->name ? g->via->name : "?",
                 rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
                 g->b && g->b->name ? g->b->name : "?");
    think_step(t, TH_DECOMPOSE, 1, 70, "经由中间词接上了",
               note && note[0] ? note : "接上了");
    /* 顺手件②：把这条**两跳**链的证据报上去 —— 链长 = 子链 + 1（a→via 那一跳），
     * 链底可信度 = min(子链的链底, a→via 这条边自己的可信度)。
     * a→via 的可信度用现成的 `der_base_credibility`（一趟一步的小推导，不新造判据）。 */
    {
        Derivation hd;
        der_reset(&hd);
        (void)der_goal(&hd, g->a, g->via_rel, g->via);
        (void)der_fact(&hd, g->a, g->via_rel, g->via, 0, +1);
        int hop_cred = der_base_credibility(ai, &hd);
        int hops = (subcand.hops > 0 ? subcand.hops : 2) + 1;
        int cred = (subcand.cred < hop_cred) ? subcand.cred : hop_cred;
        think_mech_report_cand(t, cls, 0, hops, cred);
    }
    return +1;   /* 正关系成立 */
}

/* ============================ 机制注册表 ============================
 *
 * 「追踪一个目标时可以试哪些手段」是**这一层（区域）的结构**；
 * 「这些手段此刻适不适用」必须从关系表 / 图 / 表示层里**读**出来。
 * 所以：
 *   - 机制**怎么干活**写在下面的回调里（新写代码，正当）；
 *   - 机制**什么时候能用**写在 applicable() 里，而它只读表和图；
 *   - 新增一条机制 = 注册一行 + 写它那两三个小函数，
 *     **循环与分解都不用改**（这条由 tools/delibtest.c 里运行时注册的自定义机制证明）。
 * 调度按**注册表下标**（ThinkGoal.mech）走，不按 kind ——
 * 外部机制因此不必去抢内核的枚举值（用 TG_EXT 就行）。 */
/* 前向声明：注册表里的「缺前提」机制要用它，而它的定义在下面 */
static inline void think_step_need(AI *ai, Think *t, const ThinkGoal *g);

#define THINK_MECH_MAX 12

typedef struct ThinkMech ThinkMech;
struct ThinkMech {
    const char *name;        /* 显示名（会打在思维痕迹上） */
    ThinkGoalKind kind;      /* 它产出/处理哪种目标 */
    const char *needs;       /* 人看的：它依赖什么（读自表/图/表示层） */
    int (*applicable)(AI *ai, const ThinkGoal *g);
    int (*expand)(AI *ai, Think *t, const ThinkGoal *g);   /* NULL = 默认压一个同 kind 目标 */
    int (*process)(AI *ai, Think *t, const ThinkGoal *g, int *cls, int *cache,
                   char *note, size_t note_cap);
};

static ThinkMech g_think_mech[THINK_MECH_MAX];
static size_t    g_think_mech_n = 0;

static inline const ThinkMech *think_mech_by_index(size_t i) {
    return i < g_think_mech_n ? &g_think_mech[i] : NULL;
}
static inline size_t think_mech_count(void) { return g_think_mech_n; }

/* 注册一条机制。同名不重复注册（幂等）。返回下标，-1 失败。 */
static inline int think_mech_register(const ThinkMech *m) {
    if (!m || !m->name || !m->process) return -1;
    for (size_t i = 0; i < g_think_mech_n; ++i)
        if (strcmp(g_think_mech[i].name, m->name) == 0) return (int)i;
    if (g_think_mech_n >= THINK_MECH_MAX) return -1;
    g_think_mech[g_think_mech_n] = *m;
    return (int)g_think_mech_n++;
}

/* ---- 机制①：换问法 ----
 * 适用性：关系在表里声明过（动态关系没有规则，换问法问不出新东西）。
 * 依据：规则 R7/R8/R9 只在**问否定**时开火，所以「正着问不出来」时值得反着问。 */
static inline int think_mech_opp_applicable(AI *ai, const ThinkGoal *g) {
    (void)ai;
    return g && rel_by_id(g->rid) != NULL;
}
static inline int think_mech_opp_process(AI *ai, Think *t, const ThinkGoal *g, int *cls,
                                         int *cache, char *note, size_t note_cap) {
    int c = THINK_EV_GUESS, k = 0;
    int truth = think_step_opposite(ai, t, g, t->chain_text, sizeof t->chain_text, &c, &k);
    if (cls) *cls = c;
    if (cache) *cache = k;
    if (truth != 0 && note && note_cap) snprintf(note, note_cap, "%s", t->chain_text);
    return truth;
}

/* ---- 机制②：缺哪条前提 ----
 * 适用性：**关系表**声明了 rd->prereq（以前这里写的是 `rid == REL_CAN`）。 */
static inline int think_mech_need_applicable(AI *ai, const ThinkGoal *g) {
    (void)ai;
    const RelDef *rd = g ? rel_by_id(g->rid) : NULL;
    return rd && rd->prereq;
}
static inline int think_mech_need_process(AI *ai, Think *t, const ThinkGoal *g, int *cls,
                                          int *cache, char *note, size_t note_cap) {
    if (cls) *cls = THINK_EV_RULE;
    if (cache) *cache = 0;
    think_step_need(ai, t, g);           /* 只留痕：缺的是哪一条，不产生结论 */
    if (note && note_cap) {
        Token *w = rel_need_witness(ai, g->a, g->b, 1);
        if (w) snprintf(note, note_cap, "缺的前提是「%s」", w->name ? w->name : "?");
        else   snprintf(note, note_cap, "找不到具体缺哪一条（不说死）");
    }
    return 0;                            /* 不结结论 */
}

/* ---- 机制③：借最近邻 ----
 * 适用性：**表示层在不在**（两端都有 emb 才有这条路）。 */
static inline int think_mech_ana_applicable(AI *ai, const ThinkGoal *g) {
    (void)ai;
    return g && g->a && g->b && g->a->emb && g->b->emb;
}
static inline int think_mech_ana_process(AI *ai, Think *t, const ThinkGoal *g, int *cls,
                                         int *cache, char *note, size_t note_cap) {
    if (cls) *cls = THINK_EV_GUESS;
    if (cache) *cache = 0;
    int truth = think_step_analogy(ai, t, g, t->chain_text, sizeof t->chain_text);
    if (truth != 0 && note && note_cap) snprintf(note, note_cap, "%s", t->chain_text);
    return truth;                        /* 是**真值**，回答的换算交给调用方 */
}

/* ---- 机制④：经由中间词 ----
 * 适用性：**关系表**声明了 trans（同关系传递）或 inherit（先走「属于」）。
 * 中间词**由图给出**（rel_next_cands 连假设覆盖层一起搜）。 */
static inline int think_mech_thr_applicable(AI *ai, const ThinkGoal *g) {
    (void)ai;
    const RelDef *rd = g ? rel_by_id(g->rid) : NULL;
    return rd && (rd->trans || rd->inherit);
}
static inline int think_mech_thr_expand(AI *ai, Think *t, const ThinkGoal *g) {
    const RelDef *rd = rel_by_id(g->rid);
    uint8_t hop_rel = rd && rd->trans ? g->rid : (uint8_t)REL_IS_A;
    Token  *cands[4];
    uint8_t chyp[4];
    size_t nc = rel_next_cands(ai, g->a, hop_rel, 0, cands, chyp, 4);
    int pushed = 0;
    for (size_t i = 0; i < nc && pushed < 2; ++i) {
        if (!cands[i] || cands[i] == g->a || cands[i] == g->b) continue;
        /* 找到本机制在注册表里的下标（扩压出来的目标要带着它去调度） */
        int idx = -1;
        for (size_t k = 0; k < g_think_mech_n; ++k)
            if (strcmp(g_think_mech[k].name, "经由中间词") == 0) { idx = (int)k; break; }
        if (idx < 0) break;
        if (!think_goal_push_mech(t, idx, TG_THROUGH, g->a, g->b, g->rid, 0, g->depth + 1))
            continue;
        ThinkGoal *ng = &t->agenda[t->ag_n - 1];
        ng->via = cands[i]; ng->via_hyp = chyp[i]; ng->via_rel = hop_rel;
        ThinkGoal *ns = &t->seen[t->seen_n - 1];
        ns->via = cands[i]; ns->via_rel = hop_rel;
        pushed++;
    }
    return pushed;
}
static inline int think_mech_thr_process(AI *ai, Think *t, const ThinkGoal *g, int *cls,
                                         int *cache, char *note, size_t note_cap) {
    return think_step_through(ai, t, g, cls, cache, note, note_cap);
}

/* ---- 机制⑤：问前提（R10 的闸门）----
 * 适用性：**图里**这个词有前提链接（只读图，符合机制接口的约束）。
 * 它不产生结论，只把「有前提、前提现在成不成立」写进痕里 ——
 * 这样"推不出来"和"有前提所以没用它"在 ::delib 上分得开。 */
static inline int think_mech_prem_applicable(AI *ai, const ThinkGoal *g) {
    (void)ai;
    return g && token_has_premise_link(g->a);
}
static inline int think_mech_prem_process(AI *ai, Think *t, const ThinkGoal *g, int *cls,
                                          int *cache, char *note, size_t note_cap) {
    if (cls) *cls = THINK_EV_RULE;
    if (*cache) *cache = 0;
    if (!g || !g->a) return 0;
    /* 把每一条前提链接的状态写清楚（可能多条） */
    size_t n = 0, blocked = 0;
    char list[THINK_WHY];
    list[0] = 0;
    for (size_t i = 0; i < g->a->count; ++i) {
        const AssocEntry *e = &g->a->assocs[i];
        int pol = edge_premise_polarity(e);
        if (!pol || !e->main) continue;
        const char *pn = e->main->name ? e->main->name : "?";
        int ok = ai_premise_cond_ok(ai, e->main, pol);
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s%s%s",
                 n ? "、" : "", pol < 0 ? "不 " : "", pn,
                 ok ? "（成立）" : "（没成立）");
        if (!ok) blocked++;
        n++;
    }
    if (n == 0) return 0;
    char why[THINK_WHY];
    if (blocked)
        snprintf(why, sizeof why,
                 "这条连结有前提：%s —— 前提没成立，按前提**不用**它（联想照走，判断不用）", list);
    else
        snprintf(why, sizeof why,
                 "这条连结有前提：%s —— 前提成立，经它推（推不出来才是真推不出来）", list);
    if (blocked && !t->premise_blocked) {   /* 记下"为什么没用它"，给答复用 */
        t->premise_blocked = 1;
        /* **四态里的 DEFER**（借参考实现 lingshu 的 verdict 语义）：条件没成立时，
         * 既不是"假的"、也不是"不知道"，而是**待验** —— 说出来时要把这层区别讲明白，
         * 别让用户以为这事被否掉了。 */
        snprintf(t->premise_text, sizeof t->premise_text,
                 "待验：这件事有个前提「%s」还没成立 —— 不是否定，是还定不了", list);
    }
    think_step(t, TH_LEARN, blocked ? 0 : 1, blocked ? 0 : 60, "问前提", why);
    if (note && note_cap) snprintf(note, note_cap, "%s", why);
    return 0;                       /* 不结结论：结论由 R10 或别的机制给 */
}

/* 内建的五条。**只注册一次**（think_mech_init）。 */
static inline void think_mech_builtin(void) {
    static const ThinkMech builtin[] = {
        /* 顺序 = 优先级：**问前提**排第一，因为它是纯粹的图读 + 焦点查询（最便宜），
         * 而且它解释"为什么这条连结现在不能用" —— 排在后面会被停滞检测吃掉
         * （4 个子目标 + 停滞上限 3 时，最后一个根本轮不到，实测踩过）。 */
        { "问前提", TG_PREMISE, "**图里**这个词有前提链接（R10 的闸门；只读图）",
          think_mech_prem_applicable, NULL,                     think_mech_prem_process },
        { "换问法", TG_OPPOSITE, "关系在表里声明过（R7/R8/R9 只在问否定时开火）",
          think_mech_opp_applicable,  NULL,                     think_mech_opp_process },
        { "缺前提", TG_NEED,     "关系表声明了 prereq",
          think_mech_need_applicable, NULL,                     think_mech_need_process },
        { "借最近邻", TG_ANALOGY, "两端都有表示（emb）",
          think_mech_ana_applicable,  NULL,                     think_mech_ana_process },
        { "经由中间词", TG_THROUGH, "关系表声明了 trans / inherit；中间词来自图",
          think_mech_thr_applicable,  think_mech_thr_expand,    think_mech_thr_process },
    };
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; ++i)
        (void)think_mech_register(&builtin[i]);
}
static inline void think_mech_init(void) {
    if (g_think_mech_n == 0) think_mech_builtin();
}

/* 分解：够不着就拆子目标压回议程 —— 这是「循环」而不是「并列试四种」。
 *
 * **子目标从现有架构里长出来，不在这层写死策略表**：
 *   ① 关系表声明了什么，就有什么路：
 *        rd->trans   → 经由同一个关系的中间词（A R C, C R B ⟹ A R B）
 *        rd->inherit → 先走「属于」再走本关系（A 属于 C, C R B ⟹ A R B）
 *        rd->prereq  → 问「缺的是哪条前提」（R9 用的就是这一位）
 *        rd->inv/sym → 由求解器的规则管，这里不重复造
 *   ② **中间词是图给的**（rel_next_cands 会连假设覆盖层一起搜），不是这里列的；
 *   ③ 换问法、借最近邻是**机制**（与具体关系无关），后者还要求表示层在；
 *   ④ 新增一条关系、或给某条关系加一个性质，是**改表**，不是改这一层。
 * 这样这一层就不需要知道「只有会才有前提」这类知识了。 */
static inline int think_expand_goals(AI *ai, Think *t, const ThinkGoal *g) {
    if (!ai || !t || !g) return 0;
    if (g->depth > 1) return 0;            /* 只拆一层：防组合爆炸 */
    think_mech_init();
    int pushed = 0;
    char names[160];
    names[0] = 0;

    /* 走注册表：哪条机制此刻适用（它自己读表/图/表示层决定），就让它拆。
     * 这一层不再知道「换问法/缺前提/类比/经由」分别是哪几条 —— 那是注册内容。 */
    for (size_t i = 0; i < g_think_mech_n; ++i) {
        const ThinkMech *m = &g_think_mech[i];
        if (!m->applicable || !m->applicable(ai, g)) continue;
        int got = m->expand ? m->expand(ai, t, g)
                            : think_goal_push_mech(t, (int)i, m->kind, g->a, g->b,
                                                   g->rid, g->want_neg, g->depth + 1);
        if (got > 0) {
            pushed += got;
            snprintf(names + strlen(names), sizeof names - strlen(names), "%s ", m->name);
        }
    }

    if (pushed) {
        char why[THINK_WHY];
        snprintf(why, sizeof why, "够不着 → 拆出 %d 个子目标压回议程（%s）", pushed, names);
        think_step(t, TH_DECOMPOSE, 1, 50, "够不着就拆", why);
    } else {
        think_step(t, TH_DECOMPOSE, 0, 0, "够不着就拆",
                   "注册表里没有可用的机制（表和图上都没有别的路）");
    }
    return pushed;
}

/* 「会」推不出来时：缺的是哪个前提（rel.h 的 R9 已经有证人，这里只是去问它） */
static inline void think_step_need(AI *ai, Think *t, const ThinkGoal *g) {
    Token *w = rel_need_witness(ai, g->a, g->b, 1);
    if (w) {
        char why[THINK_WHY];
        snprintf(why, sizeof why, "缺的前提是「%s」", w->name ? w->name : "?");
        think_step(t, TH_LEARN, 1, 60, "问缺什么前提", why);
    } else {
        think_step(t, TH_LEARN, 0, 0, "问缺什么前提", "找不到具体缺哪一条（不说死）");
    }
}

/* ============================ 主循环（前段：想这一轮） ============================ */

/* 把输入解析成一个目标。返回 1 = 有目标（关系问题/陈述），0 = 没有。 */
static inline int think_make_goal(AI *ai, Think *t, char *const *words, size_t n,
                                  ThinkGoal *out, int *is_q, int *is_statement,
                                  char *qtext, size_t qcap) {
    if (!ai || !t || !words || n < 3) return 0;
    if (!rel_shape_ok(words, n)) return 0;
    char jbuf[64];
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int neg = 0, q = 0;
    /* allow_new = 0：**只读**地解析。思维不许因为「想了一下」就把新谓语登记进表。 */
    if (!rel_parse(ai, words, n, jbuf, sizeof jbuf, &aw, &rid, &neg, &bw, &q, &rw, 0))
        return 0;
    Token *A = pool_find(ai->pool, aw);
    Token *B = bw ? pool_find(ai->pool, bw) : NULL;
    if (!A || !B) return 0;
    out->kind = TG_REL;
    out->a = A; out->b = B; out->rid = rid;
    out->want_neg = (uint8_t)(neg ? 1 : 0);
    out->depth = 0;
    if (is_q) *is_q = q;
    if (is_statement) *is_statement = !q;
    if (qtext && qcap) {
        char joined[THINK_TEXT];
        think_words_text(words, n, joined, sizeof joined);
        think_utf8_clip(qtext, qcap, joined);
    }
    return 1;
}

/* ============================ N2：轮中草稿（一边想一边写） ============================
 *
 * 缺口（§6.54 N2）：草稿只在**轮末**打（`think_settle` 是"想完 → 写草稿"）。
 * 用户要的是「草稿是思考的**媒介**，不是副本」—— 所以思考的**中间步**也要能写进去。
 *
 * 四条边界（§6.55 K4 定的）：
 *   · **走受审通道**：一行 = 一条 `MOD_PATCH_CALL`（调 `记事本` 的 `写`），
 *     审查/预算/记账/审计一样不少 —— **不直写**补丁的缓冲；
 *   · **一轮最多 `THINK_DRAFT_MAX` 行**（还要给"结论/停因"留一行）：不刷屏、不刷预算；
 *   · **默认关**（`draft_mid == 0`）：关着时一行都不提（与今天逐字节相同）；
 *   · **没挂 `记事本` 就什么都不做**（补丁没挂时行不通，也不用记账）。
 * 写进去的样子（`::patch op 记事本 读` 看得到）：
 *     [想] 要弄清：猫 会 跳舞 吗
 *     [想] 够不着，先拆子目标
 *     [想] 还没结论（停因：原地打转）
 * 也就是"过程"本身进了草稿，而不是等想完只留一句结论。 */
static inline size_t think_draft_cap(const Think *t) {
    if (!t) return 0;
    /* 安全内核**每步**只批 `max_patch_calls_step`（=4）次补丁操作；
     * `::notepad on|hold` 时轮末那一手要写结论 + 发送/不发 = 2 次 ——
     * 不预留就会把「发送」挤掉（实测：5 条意图 → 第 5 条被预算拒，回答退回流水线，
     * 而且当时审计的拒绝理由还是 `(null)`，因为名字表缺两条）。 */
    if (t->notepad) return 2;
    return THINK_DRAFT_MAX;
}

static inline void think_draft_mid(AI *ai, Think *t, const char *line) {
    if (!ai || !t || !line || !line[0]) return;
    if (!t->draft_mid) return;
    if (t->n_draft_mid >= think_draft_cap(t)) return;
    Micro *mk = micro_of(ai);
    if (!mk || mk->safe.mod_off) return;
    Patch *np = patch_find(&mk->patches, "记事本");
    if (!np || !np->mounted || !np->enabled) return;
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_PATCH_CALL;
    snprintf(m.patch, sizeof m.patch, "记事本");
    snprintf(m.name, sizeof m.name, "写");
    snprintf(m.args[0], sizeof m.args[0], "%s", line);
    m.n_args = 1;
    snprintf(m.source, sizeof m.source, "思维:轮中草稿");
    if (mod_propose(&mk->queue, &m)) {
        t->n_draft_mid++;
        t->n_draft_total++;
        think_step(t, TH_LEARN, 1, 40, "轮中打草稿", line);
    }
}

/* 还能再写几行？给"结论/停因"那行**留一格**（否则长回合会把结论挤掉）。 */
static inline int think_draft_room(const Think *t) {
    return t && t->draft_mid && t->n_draft_mid + 1 < think_draft_cap(t);
}

/* ============================ N3：回溯 + 在线修订（默认关） ============================
 *
 * 缺口（§6.54 N3）：思考**过程中**发现前提不成立，今天只会把"待验"记在痕上然后**继续往下想**
 * （不改主意、不回退）。局部早就有 save/restore 的料：`rel_solve_go` 的路径级存档、
 * `hypo_pop/drop` 的假设栈、`think_goal_pop`/`think_goal_reset` 的议程游标 ——
 * 这一节把它们用在**循环层**（§6.55 K2/K3）：
 *
 *   · **回退到分支起点**：`ag_n = snap_n`（这一支压进来的子目标全撤掉）——
 *     用的是现成的议程游标，不新造栈；
 *   · **换一条**：同一个问题**问反面**（`want_neg` 翻过来）再压回议程 ——
 *     "这条连结的前提不成立"往往正对面才是答案（R9 缺前提那条路）；
 *   · **痕上写明为什么退**：失败那一支的 trace 一条都不删（保留证据），
 *     另加一条 `回退：前提不成立`；
 *   · **不许突破预算**：`steps` 已经花掉了、不返还 —— 退回去的代价就是步数；
 *     一轮最多退 `THINK_REDO_MAX` 次（防止退来退去原地转）。
 * 返回 1 = 真的退了（调用方据此不再把这一步算成"停滞"）。 */
static inline int think_redo_after_premise(AI *ai, Think *t, const ThinkGoal *g,
                                           int want_neg0, size_t snap_n) {
    if (!ai || !t || !g || !t->redo_on) return 0;
    if (t->n_backtrack >= THINK_REDO_MAX) return 0;
    /* 注意：发现"前提不成立"的那一步**不是**主目标本身 ——
     * `think_expand_goals` 会把「问前提」压成一个**子目标**（kind=TG_PREMISE）去跑，
     * 前提状态就是在那里读出来的。所以这里不能只认 TG_REL（第一版就栽在这儿：
     * 开关开了、痕上明明写着"前提没成立"，却一次都没退）。
     * 判据只要"这个目标有完整的三元组"就够；要翻的极性取**原问题**的（want_neg0）。 */
    if (!g->a || !g->b || g->rid == 0) return 0;
    /* **换法 2：同类互推**（`redo_alt` 开着、且已经退过一次 ⇒ 反问题那条路试过了）。
     * 规则层**不做**同类互推，所以这是真的第二个角度：给一个**猜出来的**结论
     * （`concl=1` + `guessed=1` + 置信度压低），痕上写清"依据是同类里的谁"。 */
    if (t->redo_alt && t->n_backtrack >= 1) {
        const Token *sup = NULL;
        const Token *sib = rel_sibling_with(ai, g->a, g->rid, g->b, &sup);
        if (sib) {
            t->concl = 1; t->guessed = 1; t->conf = 0.40f;
            (void)der_conclude(&t->obj.der, 1, 40);
            char why2[THINK_WHY];
            snprintf(why2, sizeof why2,
                     "反问题那条路也走不通 —— 换第二个角度：**同类互推**：%s 也 %s %s"
                     "（%s 和 %s 都 属于 %s）⇒ 猜「成立」（guess，置信 0.40）",
                     sib->name ? sib->name : "?", rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
                     g->b->name ? g->b->name : "?", g->a->name ? g->a->name : "?",
                     sib->name ? sib->name : "?", sup && sup->name ? sup->name : "?");
            think_step(t, TH_REFLECT, 1, 45, "回退：同类互推", why2);
            t->n_backtrack++;
            t->n_backtrack_total++;
            return 1;
        }
    }
    /* **多层快照栈**（默认关）：第一次退把"这一支的起点"存起来；第二次退**退到更外一层**
     * （不再退回同一个起点）—— 这就是"多层回退"的最小形态。 */
    if (t->redo_stack) {
        if (t->redo_sp > 0) {
            int outer = --t->redo_sp;
            t->ag_n = t->redo_snap[outer];
            char why3[THINK_WHY];
            snprintf(why3, sizeof why3,
                     "这一支（含它的反面）都走不通 —— **退到更外一层**（多层回退，栈里还剩 %d 层）",
                     t->redo_sp);
            think_step(t, TH_REFLECT, 1, 45, "回退：退到更外一层", why3);
            t->n_backtrack++;
            t->n_backtrack_total++;
            return 1;
        }
        if (t->redo_sp < (int)THINK_REDO_MAX) t->redo_snap[t->redo_sp++] = snap_n;
    }
    t->ag_n = snap_n;                                  /* ① 退回分支起点 */
    int pushed = think_goal_push(t, TG_REL, g->a, g->b, g->rid,
                                 want_neg0 ? 0 : 1, g->depth + 1);
    if (!pushed) return 0;                             /* 反面的目标去过重了：退不回去就不硬退 */
    t->n_backtrack++;
    t->n_backtrack_total++;
    char why[THINK_WHY];
    snprintf(why, sizeof why,
             "这条连结有前提、前提没成立 —— 退回分支起点换一条：改问「%s %s%s %s」"
             "（失败那一支的痕留着，没删；步数不退）",
             g->a->name ? g->a->name : "?",
             g->want_neg ? "" : "不 ",
             rel_name_of(ai, g->rid) ? rel_name_of(ai, g->rid) : "?",
             g->b->name ? g->b->name : "?");
    think_step(t, TH_REFLECT, 1, 50, "回退：前提不成立", why);
    return 1;
}

static inline void think_run(AI *ai, char *const *words, size_t n) {
    Think *t = think_of(ai);
    if (!t || !ai) return;
    /* 每轮重开：这一轮的痕迹与结论 */
    t->trace_n = 0;
    t->concl = 0; t->conf = 0; t->guessed = 0; t->conflict = 0;
    t->stop = TH_STOP_NONE;
    t->took_output = 0;
    t->flipped = 0;
    t->concl_text[0] = 0; t->chain_text[0] = 0; t->chain_text2[0] = 0;
    t->premise_blocked = 0; t->premise_text[0] = 0;
    t->trust_c1 = t->trust_c2 = 0; t->trust_pick = 0;
    t->stall_run = 0;
    think_goal_reset(t);
    /* N1：这一轮的思考对象从空的开始（开关关着时这一行什么也不做） */
    if (t->obj_on) think_obj_reset(t);
    t->n_draft_mid = 0;                  /* N2：轮中草稿的每轮配额 */
    t->n_backtrack = 0;                  /* N3：回溯的每轮配额 */

    /* 上一轮想通、还没说出口的旧问题：先记在痕上，
     * 否则这一轮的痕迹一重置，就说不清那句补充是哪儿来的。 */
    if (t->mention_pending && t->mention_text[0])
        think_step(t, TH_LEARN, 1, 70, "带着一个想通了的旧问题", t->mention_text);

    if (t->run_off) {
        think_step(t, TH_STOP, 1, 0, "收束", "思维循环关着（::thinkrun on 打开）");
        t->stop = TH_STOP_OFF;
        return;                      /* 没想过 ⇒ 不产出对象（N1：对象=真的想过一轮） */
    }
    t->n_think++;

    /* ① 读入 */
    {
        size_t unknown = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!words[i] || !words[i][0] || words[i][0] == '[') continue;
            Token *w = pool_find(ai->pool, words[i]);
            if (!w || w->is_garbage || w->count == 0) unknown++;
        }
        char why[THINK_WHY];
        if (unknown)
            snprintf(why, sizeof why, "%zu 个词，其中 %zu 个我还不认识（图里没边）", n, unknown);
        else
            snprintf(why, sizeof why, "%zu 个词，都认识", n);
        think_step(t, TH_READ, 1, 100, "读入", why);
    }

    /* ② 立目标 */
    ThinkGoal main_goal;
    memset(&main_goal, 0, sizeof main_goal);
    int is_q = 0, is_stmt = 0;
    char qtext[THINK_TEXT] = {0};
    int have_goal = think_make_goal(ai, t, words, n, &main_goal, &is_q, &is_stmt,
                                    qtext, sizeof qtext);
    if (!have_goal) {
        /* 没有关系形状：这一轮没有「要弄清的事」。但**不认识的词**仍然值得挂一笔。 */
        char why[THINK_WHY];
        snprintf(why, sizeof why, "这句话不是关系问题，没有要推的东西");
        think_step(t, TH_GOAL, 0, 0, "立目标", why);
        size_t unknown_ctx = 0;
        Token *first_unknown = NULL;
        for (size_t i = 0; i < n; ++i) {
            if (!words[i] || !words[i][0] || words[i][0] == '[') continue;
            Token *w = pool_find(ai->pool, words[i]);
            if (!w || w->is_garbage || w->count == 0) {
                if (!first_unknown) first_unknown = w;
                unknown_ctx++;
            }
        }
        if (first_unknown && n > 1) {
            char otext[THINK_TEXT];
            think_words_text(words, n, otext, sizeof otext);
            ThinkOpen *o = think_open_add(t, first_unknown, NULL, 0, 0, otext,
                                         ai->pool->current_tick);
            if (o) {
                think_step(t, TH_ASK, 1, 30, "自问",
                           "有词我还不认识 —— 挂一个未解问题，等以后遇见它");
                if (!t->learn_off && !micro_of(ai)->safe.mod_off) {
                    ModIntent in;
                    memset(&in, 0, sizeof in);
                    in.type = MOD_ADD_TOKEN;
                    in.target = first_unknown;
                    think_ask_name(in.name, sizeof in.name, otext);
                    snprintf(in.source, sizeof in.source, "思维:自问");
                    (void)mod_propose(&micro_of(ai)->queue, &in);
                }
            } else {
                t->n_blocked++;
                think_step(t, TH_ASK, 0, 0, "自问", "未解问题已经挂满了（元认知不许再挂）");
            }
        } else {
            think_step(t, TH_STOP, 1, 30, "收束", "没有要弄清的关系问题（普通陈述句）");
            t->stop = TH_STOP_NO_GOAL;
            think_obj_finish(ai, t);   /* N1：想过一轮就定稿（没链 ⇒ 对象里留一条缺口） */
            return;
        }
        think_step(t, TH_STOP, 1, 30, "收束",
                   "有词我还不认识 —— 挂起来等以后遇见它，这一轮想不深");
        t->stop = TH_STOP_UNKNOWN;
        think_obj_finish(ai, t);
        return;
    }

    /* 关系**陈述**：不是问，是告诉我一件事。要弄清的是「它跟我已知的冲不冲突」。 */
    if (is_stmt) {
        char why[THINK_WHY];
        snprintf(why, sizeof why, "「%s」是一条陈述，我要弄清它和不和已知的冲突",
                 qtext[0] ? qtext : "?");
        think_step(t, TH_GOAL, 1, 70, "立目标（陈述：查冲突）", why);
    } else {
        char why[THINK_WHY];
        snprintf(why, sizeof why, "要弄清：%s %s%s %s 成不成立",
                 main_goal.a->name ? main_goal.a->name : "?",
                 main_goal.want_neg ? "不 " : "",
                 rel_name_of(ai, main_goal.rid) ? rel_name_of(ai, main_goal.rid) : "?",
                 main_goal.b->name ? main_goal.b->name : "?");
        think_step(t, TH_GOAL, 1, 80, "立目标（关系问题）", why);
    }

    /* N2：轮中草稿的第一行 —— "我要弄清什么"（走受审通道，见 think_draft_mid） */
    {
        char dline[THINK_TEXT + 16];
        snprintf(dline, sizeof dline, "[想] 要弄清：%s\n",
                 qtext[0] ? qtext : (main_goal.a && main_goal.a->name ? main_goal.a->name : "?"));
        think_draft_mid(ai, t, dline);
    }

    /* ③ 把主目标压进议程，开始循环 */
    think_goal_push(t, TG_REL, main_goal.a, main_goal.b, main_goal.rid,
                    main_goal.want_neg, 0);

    size_t budget = t->max_steps ? t->max_steps : THINK_DEF_STEPS;
    size_t steps = 0;
    int decided = 0;
    char why[THINK_WHY];
    while (steps < budget) {
        ThinkGoal g;
        if (!think_goal_pop(t, &g)) break;
        steps++;
        t->n_steps_total++;
        int progressed = 0;
        /* N3：分支起点。这一支之后压进议程的子目标都在 `ag_n` 之后 ——
         * 回退就是把 `ag_n` 收回这个值（现成的议程游标，不新造栈）。 */
        size_t snap_ag_n = t->ag_n;

        if (g.kind == TG_REL) {
            int lcls = THINK_EV_FACT, lov = 0, lcache = 0;
            int r = think_step_lookup(ai, t, &g, why, sizeof why, &lcls, &lov, &lcache);
            if (r == 3) {
                if (lov == 0) {
                    /* 两边都是图里的边：事实撞事实。
                     * 不用在这儿自己记矛盾 —— think_finish 会照常查反面，
                     * 两边都是事实 ⟹ 它自己就会判成矛盾（重复记账会算两次）。 */
                    char note[THINK_WHY];
                    snprintf(note, sizeof note, "%s", why);
                    ThinkCand c = think_cand_make(THINK_EV_FACT, 0, 0, 50);
                    think_finish(ai, t, &g, 0, 0, 0, note, &c);
                    decided = 1;
                    break;
                }
                /* 有一边来自假设层：**假设世界那边就是真值**（lov 是正关系的真值方向），
                 * 再按原来的问法换算成回答；强度记「假设」。
                 * think_finish 会照常去查反面，强度不够就不会判成矛盾。 */
                ThinkCand c = think_cand_make(THINK_EV_HYP, 0, 0, 50);
                think_finish(ai, t, &g, think_answer_of(lov, g.want_neg), 60, 0, why, &c);
                decided = 1;
                break;
            }
            if (r != 0) {
                ThinkCand c = think_cand_make(lcls, lcache, 0, 50);
                think_finish(ai, t, &g, r, lcls >= THINK_EV_FACT ? 95 : 60, 0, why, &c);
                decided = 1;
                break;
            }
            /* 查不到 → 推（先推问的那个方向） */
            int verified = 0, dcls = THINK_EV_GUESS, dcache = 0;
            ThinkCand dcand;
            int d = think_step_derive(ai, t, &g, t->chain_text, sizeof t->chain_text,
                                      &verified, &dcls, &dcache, &dcand);
            if (d != 0 && verified) {
                think_finish(ai, t, &g, think_answer_of(d, g.want_neg), 85, 0,
                             t->chain_text, &dcand);
                decided = 1;
                break;
            }
            progressed = think_expand_goals(ai, t, &g);
            if (progressed > 0 && think_draft_room(t))      /* N2：给"结论"那行留一格 */
                think_draft_mid(ai, t, "[想] 够不着，先拆成子目标接着够\n");
        } else {
            /* 不是核心那条路：按**注册表下标**调度（扩展机制因此不用抢枚举值） */
            const ThinkMech *m = (g.mech >= 0) ? think_mech_by_index((size_t)g.mech) : NULL;
            if (!m || !m->process) {
                think_step(t, TH_REST, 0, 0, "停顿", "没有机制能处理这种目标");
            } else {
                int mcls = THINK_EV_GUESS, mcache = 0;
                char note[THINK_WHY];
                note[0] = 0;
                int pb_before = t->premise_blocked;   /* N3：看这一步有没有**刚刚**发现前提不成立 */
                t->mech_cand_set = 0;                 /* 顺手件②：机制自己报的证据，先清空 */
                int truth = m->process(ai, t, &g, &mcls, &mcache, note, sizeof note);
                /* N3（默认关）：前提不成立 ⇒ **当场回退 + 换一条**（在线修订，K3）。
                 * 位置就在这条路上：`think_mech_prem` 本来每一轮都在跑，它把状态写进
                 * `premise_blocked`，这里读同一个信号，不另造判据。 */
                if (truth == 0 && !pb_before && t->premise_blocked &&
                    think_redo_after_premise(ai, t, &g, main_goal.want_neg, snap_ag_n))
                    continue;                          /* 退了：这一步不算"没推进" */
                if (truth != 0) {
                    int guessed = (mcls == THINK_EV_GUESS);
                    /* 顺手件②：机制**自己报**的链长/链底可信度优先（内置的四条都报了）；
                     * 外部的自定义机制不报 ⇒ 按中性给（hops=0、cred=50）——
                     * 与"没接之前"逐字节相同，所以这条改动对不认识它的机制是零影响。 */
                    ThinkCand mc = t->mech_cand_set
                                 ? think_cand_make(mcls, mcache, t->mech_cand.hops, t->mech_cand.cred)
                                 : think_cand_make(mcls, mcache, 0, 50);
                    think_finish(ai, t, &g, think_answer_of(truth, g.want_neg),
                                 guessed ? 45 : 80, guessed,
                                 note[0] ? note : t->chain_text, &mc);
                    /* 猜测与断言并存不算「矛盾」：一个是猜的、一个是真的 */
                    if (guessed && t->conflict) {
                        t->conflict = 0;
                        t->concl = think_answer_of(truth, g.want_neg);
                        t->conf = 45;
                        t->stop = TH_STOP_GUESSED;
                    }
                    decided = 1;
                    break;
                }
            }
        }

        if (!decided && !progressed) {
            t->stall_run++;
            think_step(t, TH_REST, 0, 0, "停顿", "这一步没有推进（没结论、也没拆出新目标）");
            if (t->stall_run >= (t->stall_limit ? t->stall_limit : THINK_STALL_LIM)) {
                t->n_stalls++;
                think_remind(ai, MICRO_REMIND_THINK_STALL, NULL);
                think_step(t, TH_STOP, 0, 0, "收束", "在原地打转，先停下");
                t->stop = TH_STOP_STALL;
                break;
            }
        }
    }
    if (!decided && t->stop == TH_STOP_NONE) {
        if (steps >= budget) {
            t->n_over_budget++;
            think_step(t, TH_STOP, 0, 0, "收束", "步数预算用完，先停下（不硬凑结论）");
            t->stop = TH_STOP_BUDGET;
        } else if (t->stop == TH_STOP_NONE) {
            think_step(t, TH_STOP, 0, 0, "收束", "想过了，还是不知道");
            t->stop = TH_STOP_UNKNOWN;
        }
    }

    /* ④ 反省：想不通又是问句 → 挂一个未解问题（跨轮接着想） */
    if (!decided && !is_stmt && t->stop != TH_STOP_OFF) {
        ThinkOpen *o = think_open_add(t, main_goal.a, main_goal.b, main_goal.rid,
                                      main_goal.want_neg, qtext, ai->pool->current_tick);
        if (o) {
            char note[THINK_WHY];
            snprintf(note, sizeof note, "挂起来跨轮接着想：%s", qtext[0] ? qtext : "?");
            think_step(t, TH_ASK, 1, 30, "自问", note);
            /* 登记成一个可辨识的节点（经意图队列 + 安全内核，思维不直接改图） */
            if (!t->learn_off && !micro_of(ai)->safe.mod_off) {
                ModIntent in;
                memset(&in, 0, sizeof in);
                in.type = MOD_ADD_TOKEN;
                in.target = main_goal.a;
                think_ask_name(in.name, sizeof in.name, qtext[0] ? qtext : "?");
                snprintf(in.source, sizeof in.source, "思维:自问");
                (void)mod_propose(&micro_of(ai)->queue, &in);
            }
        } else {
            t->n_blocked++;
            think_step(t, TH_ASK, 0, 0, "自问", "未解问题挂满了 —— 先解决旧的（元认知拦下）");
        }
    }

    /* ⑤ 反省：证据强度 vs 把握（把握低就明说） */
    {
        char note[THINK_WHY];
        if (t->conflict)
            snprintf(note, sizeof note, "两条依据互相矛盾 → **不下断言**，说出来");
        else if (t->concl != 0 && t->guessed)
            snprintf(note, sizeof note, "只有类比推测（%d%% 把握）→ 说的时候要标明是猜的", t->conf);
        else if (t->concl != 0)
            snprintf(note, sizeof note, "有依据（%d%% 把握）", t->conf);
        else
            snprintf(note, sizeof note, "没依据 → 说不知道，不编（%s）", think_stop_name(t->stop));
        think_step(t, TH_REFLECT, t->concl != 0 || t->conflict, t->conf, "反省", note);
    }

    /* N2：轮中草稿的最后一行 —— 这一轮是怎么收的（结论 / 还没结论 + 停因） */
    {
        char dline[THINK_CONCL + 24];
        if (t->concl != 0 && t->concl_text[0])
            snprintf(dline, sizeof dline, "[想] 有了结论：%s\n", t->concl_text);
        else
            snprintf(dline, sizeof dline, "[想] 还没结论（停因：%s）\n", think_stop_name(t->stop));
        think_draft_mid(ai, t, dline);
    }

    /* N1：这一轮到此为止 —— 把思考对象**定稿**（编号 / 停因 / 结论 / 复核一遍）。
     * 关着开关时这一步什么也不做（行为与今天逐字节相同）。 */
    think_obj_finish(ai, t);
}

/* ============================ 收尾（后段：用新知识再想一遍 + 改脑） ============================ */

/* 这条缓存现在还有依据吗？依据 = **把缓存排除在外**还能推出来，且验证器 gaps==0。
 * 注意这是**单轮**判定：`rel_solve_ignoring_caches` 把所有缓存都当不存在，
 * 所以「缓存拿缓存当依据」这种情况天然不成立 —— 缓存的依据只能是观测 + 规则。
 * 这是有意的保守：**捷径不许嵌套**，否则一条过时的缓存会拖着后面一串。 */
static inline int think_cache_supported(AI *ai, Token *a, uint8_t rid, Token *b, int neg) {
    if (!ai || !a || !b || rid == 0) return 0;
    Proof pf;
    g_rel_ignore_derived = 1;
    rel_solve(ai, a, rid, b, neg, &pf);
    g_rel_ignore_derived = 0;
    if (pf.status == 0) return 0;
    Derivation d;
    der_reset(&d);
    der_from_proof(ai, &d, a, rid, b, &pf);
    return der_verify(ai, &d) == 0;
}

/* **缓存跟着前提走**：扫一遍图上所有缓存，把已经没有依据的提出来撤销。
 * 前提被观测推翻（那条边自己打架了）、被撤掉、或者它的中间跳没了 ——
 * 三种情况都会让依据复核失败，这条缓存就不该再当事实用。
 * 求解器那边还有一道实时闸（R0 里拒绝无依据的缓存），这里负责**清理**，
 * 免得它一直挂在图上、每次都要复核一次。 */
static inline void think_revalidate_caches(AI *ai, Think *t) {
    if (!ai || !t || t->learn_off) return;
    if (micro_of(ai)->safe.mod_off) return;
    ModQueue *q = &micro_of(ai)->queue;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *a = ai->pool->tokens[i];
        if (!a || a->is_garbage) continue;
        for (size_t j = 0; j < a->count; ++j) {
            AssocEntry *e = &a->assocs[j];
            if (e->role != ASSOC_ROLE_DERIVED || !e->main) continue;
            for (uint8_t rid = 1; rid <= REL_CORE_MAX; ++rid) {
                uint16_t bit = REL_BIT(rid);
                int neg;
                if (e->rel & bit)       neg = 0;
                else if (e->nrel & bit) neg = 1;
                else                    continue;
                if (think_cache_supported(ai, a, rid, e->main, neg)) continue;
                ModIntent in;
                memset(&in, 0, sizeof in);
                in.type = MOD_REMOVE_ASSOC;
                in.target = a;
                in.main = e->main;
                snprintf(in.source, sizeof in.source, "思维:前提没了");
                if (mod_propose(q, &in)) {
                    char note[THINK_WHY];
                    snprintf(note, sizeof note,
                             "这条缓存的依据没有了（%s %s%s %s 已经推不出来）—— 撤销它",
                             a->name ? a->name : "?", neg ? "不 " : "",
                             rel_name_of(ai, rid) ? rel_name_of(ai, rid) : "?",
                             e->main->name ? e->main->name : "?");
                    think_step(t, TH_LEARN, 1, 60, "前提没了", note);
                    t->n_stale++;
                } else {
                    t->n_cache_blocked++;
                }
                break;      /* 这条边整条要撤，别再逐位看 */
            }
        }
    }
}

/* 一个挂着的未解问题被想通了：**只动内存**（关掉它 + 记账）。
 * `think_settle` 的重试与 N4 的空闲思考都走这一条 —— 判据只有一份。 */
static inline void think_open_done(Think *t, ThinkOpen *o) {
    if (!t || !o || !o->active) return;
    o->active = 0;
    if (t->n_open) t->n_open--;
    t->n_resolved++;
}

/* 在安全点之前调用：这一轮学到的东西可能让**旧问题**想通了。
 * 也是元认知看思维本身的地方（打转 / 挂太多 / 矛盾）。 */
static inline void think_settle(AI *ai) {
    Think *t = think_of(ai);
    if (!t || !ai || t->run_off) return;
    uint64_t tick = ai->pool->current_tick;

    /* ① 重试未解问题（这就是「思维是循环」而不是「一轮就完」的地方） */
    size_t retried = 0;
    for (size_t i = 0; i < THINK_MAX_OPEN && retried < 4; ++i) {
        ThinkOpen *o = &t->open[i];
        if (!o->active || o->stalled) continue;
        retried++;
        o->tries++;
        o->last_try = tick;
        /* 第一类：「这个词我还不认识」——它现在有边了，就算想通了 */
        if (o->rid == 0 || !o->obj) {
            if (o->subj && o->subj->count > 0) {
                think_open_done(t, o);
                char note[THINK_WHY];
                snprintf(note, sizeof note, "现在我认识「%s」了（它连上了 %zu 条边）",
                         o->subj->name ? o->subj->name : "?", o->subj->count);
                think_step(t, TH_LEARN, 1, 70, "跨轮想通", note);
                t->mention_pending = 1;
                snprintf(t->mention_text, sizeof t->mention_text,
                         "我 现在 认识 %s 了 ： 它 连上 了 %zu 条 边",
                         o->subj->name ? o->subj->name : "?", o->subj->count);
            } else if (o->tries >= (t->stall_limit ? t->stall_limit : THINK_STALL_LIM)) {
                o->stalled = 1;
                t->n_stalls++;
                think_step(t, TH_REST, 0, 0, "停顿", "挂着的词一直没再遇见，先放下");
            }
            continue;
        }
        Proof pf;
        rel_solve(ai, o->subj, o->rid, o->obj, o->want_neg, &pf);
        int status = pf.status;
        int gaps = 0;
        if (status != 0) {
            Derivation d;
            der_reset(&d);
            der_from_proof(ai, &d, o->subj, o->rid, o->obj, &pf);
            gaps = der_verify(ai, &d);
        }
        if (status != 0 && gaps == 0) {
            /* 想通了！但**不把推出来的东西写进图**（观测才配进图）。 */
            think_open_done(t, o);
            char chain[THINK_WHY];
            proof_text(ai, &pf, chain, sizeof chain);
            char note[THINK_WHY];
            snprintf(note, sizeof note, "上次没想通的「%s」现在想通了：%s",
                     o->text, chain[0] ? chain : "有依据了");
            think_step(t, TH_LEARN, 1, 80, "跨轮想通", note);
            t->mention_pending = 1;
            snprintf(t->mention_text, sizeof t->mention_text,
                     "你 上次 问 的 %s，现在 我 知道 了 ： %s %s %s %s",
                     o->text,
                     o->subj && o->subj->name ? o->subj->name : "?",
                     o->want_neg ? "不 " : "",
                     rel_name_of(ai, o->rid) ? rel_name_of(ai, o->rid) : "?",
                     o->obj && o->obj->name ? o->obj->name : "?");
            /* 想通了就把那条自问节点收起来（标记成垃圾不是删；GC 会处理） */
            continue;
        }
        if (o->tries >= (t->stall_limit ? t->stall_limit : THINK_STALL_LIM)) {
            o->stalled = 1;
            t->n_stalls++;
            think_step(t, TH_REST, 0, 0, "停顿", "同一件事试够次数还是没进展 —— 先放下");
            think_remind(ai, MICRO_REMIND_THINK_STALL, NULL);
        }
        if (tick > o->born_tick + THINK_OPEN_TTL) {
            o->active = 0;
            t->n_open--; t->n_expired++;
            think_step(t, TH_REST, 0, 0, "过期", "放太久了，旧问题过期（问过一次就够了）");
        }
    }

    /* ② 思维改脑：只提两类意图（都有依据，且都过安全内核） */
    if (!t->learn_off && !micro_of(ai)->safe.mod_off) {
        ModQueue *q = &micro_of(ai)->queue;
        /* (a) 远距离共现：D1 只看距离 ≤2，这里补上更远的那些；
         *     判据仍然是**图内证据**（邻居重叠），不是「一起出现过」。 */
        if (t->trace_n > 0 && t->n_think % 2 == 0) {
            /* 每两轮才看一次，免得每轮都在提同一类事（省预算也省审查） */
            size_t budget = 1;
            for (size_t i = 0; i < t->seen_n && budget; ++i) {
                /* 用议程里记过的目标两端做候选（它们本来就是这一轮关心的东西） */
                const ThinkGoal *g = &t->seen[i];
                if (!g->a || !g->b || g->a == g->b) continue;
                if (!g->a->name || g->a->name[0] == '[' || !g->b->name || g->b->name[0] == '[') continue;
                if (micro_has_edge(g->a, g->b) || micro_has_edge(g->b, g->a)) continue;
                if (!micro_jaccard_at_least(g->a, g->b, MICRO_D1_MIN_COMMON + 2)) continue;
                ModIntent in;
                memset(&in, 0, sizeof in);
                in.type = MOD_ADD_ASSOC;
                in.target = g->a;
                in.main = g->b;
                in.bump = 1;
                snprintf(in.source, sizeof in.source, "思维:连接");
                if (mod_propose(q, &in)) {
                    char note[THINK_WHY];
                    snprintf(note, sizeof note,
                             "%s 和 %s 的邻居重叠够高（图里已有证据），把这条连结固化",
                             g->a->name, g->b->name);
                    think_step(t, TH_LEARN, 1, 50, "登记连结", note);
                    budget--;
                }
            }
        }
    }

    /* ③ 推理沉淀 / 撤销缓存：这一轮想通的和被推翻的，**现在**落地。
     * 都走意图队列 + 安全内核（沉淀要预算、撤销只能撤推出来的）。 */
    if (!t->cache_off && !micro_of(ai)->safe.mod_off) {
        ModQueue *q = &micro_of(ai)->queue;
        for (size_t i = 0; i < t->n_pend_cache; ++i) {
            const ThinkFact *f = &t->pend_cache[i];
            if (!f->a || !f->b) continue;
            /* 已经是**观测**了就不沉淀：那会把观测降级成缓存（安全内核也会拒） */
            if (micro_has_edge(f->a, f->b) && !micro_edge_is_derived(f->a, f->b)) continue;
            /* **写入分级闸门**（默认关、`::wgate on`）：管的是**AI 自己写知识**
             * 这一侧 —— 用户说的观测**永远照收**（"只有观测进图"是铁律），
             * 但它自己想出来的东西得够分量才配沉淀成缓存。
             * 实测教训：闸门放错地方（放到用户的观测上）会把 `猫 吃 鱼` 这种也挡掉，
             * 而且真模型上 167 条关系边全落在 0.47~0.61，0.40 阈值**根本打不着火**。 */
            if (!ai->write_gate_off) {
                /* 候选自己的证据（链长 / 链底可信度 / 证据档）随 `f` 一起到这儿；
                 * 新项开关关着时 ai_write_importance_ctx 与老公式**逐位相同**（§6.43）。 */
                AiWriteCtx wc;
                wc.ev = (int)f->ev; wc.hops = (int)f->hops; wc.cred = (int)f->cred;
                double imp = ai_write_importance_ctx(ai, f->a, f->b, &wc);
                ai->last_write_imp = imp;
                ai->last_write_hops = wc.hops;
                ai->last_write_cred = wc.cred;
                ai->last_write_ev = wc.ev;
                /* ② 分层判据（默认关）：开着时按"硬门槛 + 三档"决定，而不是一个阈值 */
                int tier_allow = -1;
                char tier_why[96];
                if (ai->wgate_tier) {
                    tier_allow = ai_wgate_tiered(ai, imp, wc.ev, wc.cred, tier_why, sizeof tier_why);
                    if (tier_allow) {
                        think_step(t, TH_LEARN, 1, 55, "写入闸门（分层）", tier_why);
                        ai->n_cached++;                 /* 与老路同账：放行也算一次沉淀 */
                    }
                }
                if (tier_allow == 0 || (tier_allow < 0 && imp < ai_wgate_knowledge(ai))) {
                    char why2[THINK_WHY];
                    if (tier_allow == 0)
                        snprintf(why2, sizeof why2, "写入闸门（分层）：%s —— 想法留在痕上", tier_why);
                    else
                        snprintf(why2, sizeof why2,
                                 "这条太轻（重要度 %.2f，%s；链 %d 跳/链底 %d 分）——"
                                 "先不沉淀，想法留在痕上",
                                 imp, ai_write_layer_of(ai, imp), wc.hops, wc.cred);
                    think_step(t, TH_LEARN, 0, 30, "写入闸门", why2);
                    t->n_cache_blocked++;
                    ai->n_write_blocked++;          /* 账要对得上：拦下的也算一次"想写" */
                    continue;
                }
            }
            ModIntent in;
            memset(&in, 0, sizeof in);
            in.type = MOD_TAG_DERIVED;
            in.target = f->a; in.main = f->b;
            in.rel_id = f->rid; in.neg = f->neg; in.bump = 1;
            snprintf(in.source, sizeof in.source, "思维:沉淀");
            if (mod_propose(q, &in)) {
                char note[THINK_WHY];
                snprintf(note, sizeof note, "%s %s%s %s —— 想通了就记住（标成推出来的）",
                         f->a->name ? f->a->name : "?", f->neg ? "不 " : "",
                         rel_name_of(ai, f->rid) ? rel_name_of(ai, f->rid) : "?",
                         f->b->name ? f->b->name : "?");
                think_step(t, TH_LEARN, 1, 60, "沉淀", note);
                ai->n_cached++;
            } else {
                t->n_cache_blocked++;
            }
        }
        for (size_t i = 0; i < t->n_pend_retract; ++i) {
            const ThinkFact *f = &t->pend_retract[i];
            if (!f->a || !f->b) continue;
            if (!micro_has_edge(f->a, f->b) || !micro_edge_is_derived(f->a, f->b)) continue;
            ModIntent in;
            memset(&in, 0, sizeof in);
            in.type = MOD_REMOVE_ASSOC;
            in.target = f->a; in.main = f->b;
            snprintf(in.source, sizeof in.source, "思维:撤销缓存");
            if (mod_propose(q, &in)) {
                /* **撤销留痕**：再提一条"建标记节点"的意图（同一条受审通道）——
                 * 撤掉的东西图里没痕迹，`n_retracted` 重载后就数不回来（推论档偏乐观）。
                 * 标记名里写上"撤的是哪条"，重载时 `ai_recount_evidence` 数它。 */
                ModIntent mk;
                memset(&mk, 0, sizeof mk);
                mk.type = MOD_ADD_TOKEN;
                {   /* 90 字节的名字缓冲：有界拼接（截断是故意的） */
                    size_t off = 0;
                    const char *head = MICRO_REVOKE_TOKEN_PREFIX;
                    for (const char *q2 = head; *q2 && off + 2 < sizeof mk.name; ++q2) mk.name[off++] = *q2;
                    for (const char *q2 = f->a && f->a->name ? f->a->name : "?"; *q2 && off + 2 < sizeof mk.name; ++q2)
                        mk.name[off++] = *q2;
                    if (off + 2 < sizeof mk.name) mk.name[off++] = '|';
                    for (const char *q2 = f->neg ? "不" : ""; *q2 && off + 2 < sizeof mk.name; ++q2)
                        mk.name[off++] = *q2;
                    for (const char *q2 = rel_name_of(ai, f->rid) ? rel_name_of(ai, f->rid) : "?"; *q2 && off + 2 < sizeof mk.name; ++q2)
                        mk.name[off++] = *q2;
                    if (off + 2 < sizeof mk.name) mk.name[off++] = '|';
                    for (const char *q2 = f->b && f->b->name ? f->b->name : "?"; *q2 && off + 2 < sizeof mk.name; ++q2)
                        mk.name[off++] = *q2;
                    if (off + 1 < sizeof mk.name) mk.name[off++] = ']';
                    mk.name[off] = 0;
                }
                mk.target = ai->self;
                snprintf(mk.source, sizeof mk.source, "思维:撤销留痕");
                (void)mod_propose(q, &mk);
                char note[THINK_WHY];
                snprintf(note, sizeof note, "撤销过时的缓存：%s %s%s %s",
                         f->a->name ? f->a->name : "?", f->neg ? "不 " : "",
                         rel_name_of(ai, f->rid) ? rel_name_of(ai, f->rid) : "?",
                         f->b->name ? f->b->name : "?");
                think_step(t, TH_LEARN, 1, 60, "撤销缓存", note);
                ai->n_retracted++;
            } else {
                t->n_cache_blocked++;
            }
        }
    }
    t->n_pend_cache = 0;
    t->n_pend_retract = 0;

    /* ③b 缓存跟着前提走：先把没依据的清掉，再谈别的 */
    think_revalidate_caches(ai, t);

    /* ④ 元认知看思维本身 */
    if (t->n_open * 4 >= THINK_MAX_OPEN * 3) {
        think_remind(ai, MICRO_REMIND_THINK_OPEN, NULL);
    }
    /* ④ AI 自己用记事本：**什么时候说、说什么**由它自己决定。
     *    `::notepad on`   → 有结论：把结论写进草稿并发送（说出来的是**它自己写的那句**）
     *    `::notepad hold` → 没结论：**按住不说**（宁可不说，也不硬答）
     * 只在挂了 `记事本` 补丁、且这一轮真的想过（有痕）时才动手；一次最多两笔。
     * 操作照样走受审通道（MOD_PATCH_CALL → 审查/预算/审计）。 */
    if (t->notepad && t->trace_n > 0 && !micro_of(ai)->safe.mod_off) {
        Micro *mk4 = micro_of(ai);
        Patch *np = patch_find(&mk4->patches, "记事本");
        if (np && np->mounted && np->enabled) {
            ModQueue *q4 = &mk4->queue;
            /* **有结论**就算（+1 成立 / -1 不成立都算）：不定成 `== 1` 的话，
             * "推出来了但结论是否定的"那一半会被漏掉（实测踩过：猫 不 会 唱歌 就不说）。 */
            if (t->concl != 0 && t->concl_text[0]) {
                /* **把依据一起写进草稿**：结论本身只有一句话，信息量比流水线的
                 * "带规则名的链"少得多（`猫 -[会·缺前提]-> 唱歌`）。以前只写结论，
                 * 出口就只剩干巴巴一句 —— 现在把链附在后面，等于把丢掉的措辞补回来。 */
                char dtext[THINK_TEXT];
                const char *chain = t->chain_text[0] ? t->chain_text : t->chain_text2;
                /* 有界拼（两个来源都可能比缓冲长；截断是故意的，别让 -Wformat-truncation 拦） */
                {
                    size_t off2 = 0;
                    #define DTXT_ADD(str_) do { \
                        const char *q_ = (str_); \
                        while (*q_ && off2 + 1 < sizeof dtext) dtext[off2++] = *q_++; \
                    } while (0)
                    DTXT_ADD(t->concl_text);
                    if (chain[0]) { DTXT_ADD("（依据："); DTXT_ADD(chain); DTXT_ADD("）"); }
                    dtext[off2] = 0;
                    #undef DTXT_ADD
                }
                ModIntent w;
                memset(&w, 0, sizeof w);
                w.type = MOD_PATCH_CALL;
                snprintf(w.patch, sizeof w.patch, "记事本");
                snprintf(w.name, sizeof w.name, "写");
                /* 结论可能比一个参数槽长：**截断是故意的**（草稿区也放得下），
                 * 用有界拷贝而不是 snprintf，免得 -Werror=format-truncation 拦下。 */
                {
                    size_t lim = sizeof w.args[0] - 1;
                    size_t k = 0;
                    while (k < lim && dtext[k]) { w.args[0][k] = dtext[k]; k++; }
                    w.args[0][k] = 0;
                }
                w.n_args = 1;
                snprintf(w.source, sizeof w.source, "思维:打草稿");
                if (mod_propose(q4, &w)) {
                    think_step(t, TH_LEARN, 1, 60, "打草稿",
                               "先把结论写进草稿区（写坏了也不影响发言）");
                    t->n_notepad++;
                }
                ModIntent sd;
                memset(&sd, 0, sizeof sd);
                sd.type = MOD_PATCH_CALL;
                snprintf(sd.patch, sizeof sd.patch, "记事本");
                snprintf(sd.name, sizeof sd.name, "发送");
                snprintf(sd.source, sizeof sd.source, "思维:发送");
                if (mod_propose(q4, &sd)) {
                    think_step(t, TH_LEARN, 1, 60, "发送",
                               "这一轮我来发：说的是我自己写的那句");
                    t->n_notepad++;
                }
            } else if (t->notepad == 2) {
                ModIntent hd;
                memset(&hd, 0, sizeof hd);
                hd.type = MOD_PATCH_CALL;
                snprintf(hd.patch, sizeof hd.patch, "记事本");
                snprintf(hd.name, sizeof hd.name, "不发");
                snprintf(hd.source, sizeof hd.source, "思维:按住不说");
                if (mod_propose(q4, &hd)) {
                    think_step(t, TH_LEARN, 1, 50, "按住不说",
                               "没想清楚就先不说（控制什么时候发送）");
                    t->n_notepad++;
                }
            }
        }
    }

    if (micro_of(ai)->safe.freeze || micro_of(ai)->safe.mod_off) {
        /* 自修改被冻结/关掉时，思维照样想，但**改不了脑** —— 这一点要留在痕上 */
        if (t->trace_n && t->trace[t->trace_n - 1].kind != TH_STOP)
            think_step(t, TH_REFLECT, 0, 0, "改不了脑",
                       "自修改被冻结或关着：想法留在痕上，不落地");
    }
}


/* ============================ N4：空闲思考（只读） ============================
 *
 * 缺口（§6.54 N4）：`think_run` 只由**输入**触发 —— 没人说话就没有"目标导向"的思考，
 * 而背景活动（想象 / 内部对话 / 睡眠 / 叙事 / 漂移 / GC）都不是"追一个目标"。
 *
 * 这一节给它**第二条入口**（边界按 §6.55 K5 定的三条写死）：
 *   · **自己挑**：只从 `Think.open`（`[自问:…]` 那批未解问题）里挑 ——
 *     优先"没停滞、最久没试过"的那个（与 `think_settle` 的重试同一判据）；
 *   · **只读**：跑这一轮之前临时把 `learn_off` 打开 ⇒ **不提议任何意图**
 *     （自问节点、沉淀、远距离共现连接都不提）；也不调 `think_settle`
 *     （不沉淀、不撤销、不写图）。它想出来的东西只留在**痕**（和 N1 的对象）里；
 *   · **预算复用 think 的**（`max_steps`）：不另开预算、不绕过停滞/步数限制；
 *   · **默认关**：`idle_every == 0` 时一个字都不做（那一轮仍是今天那句"……"）。
 * 节拍：`::idle every <N>`（每 N 个 tick 最多一次）/ `::idle on|off` / `AIP_IDLE=1`。
 * 返回 1 = 这一轮真的空闲想过了（调用方据此判断"这一轮不是白过"）。 */
static inline int think_idle(AI *ai) {
    Think *t = think_of(ai);
    if (!ai || !t || t->run_off) return 0;
    if (t->idle_every == 0) return 0;
    uint64_t tick = ai->pool ? ai->pool->current_tick : 0;
    if (t->idle_last_tick && tick < t->idle_last_tick + (uint64_t)t->idle_every) return 0;

    /* 挑一个问题：没停滞、最久没试过的那个 */
    ThinkOpen *pick = NULL;
    for (size_t i = 0; i < THINK_MAX_OPEN; ++i) {
        ThinkOpen *o = &t->open[i];
        if (!o->active || o->stalled || !o->text[0]) continue;
        if (!pick || o->last_try < pick->last_try) pick = o;
    }
    if (!pick) return 0;                     /* 没有挂着的、还能想的问题：这一轮就白过 */

    /* 把问题还原成词（`[自问:…]` 的正文本来就是**空格连起来的原词**） */
    char buf[THINK_TEXT];
    think_utf8_clip(buf, sizeof buf, pick->text);
    char *words[16];
    size_t n = 0;
    for (char *q = buf; *q && n < 16; ) {
        while (*q == ' ') q++;
        if (!*q) break;
        words[n++] = q;
        while (*q && *q != ' ') q++;
        if (*q) *q++ = 0;
    }
    if (n == 0) return 0;

    t->idle_last_tick = tick;
    t->n_idle++;
    pick->tries++;
    pick->last_try = tick;
    think_utf8_clip(t->idle_note, sizeof t->idle_note, pick->text);

    /* **只读**：这一轮不提议任何意图（K5）。想完就还回去。 */
    int learn_save = t->learn_off;
    t->learn_off = 1;
    think_run(ai, words, n);
    t->learn_off = learn_save;

    char note[THINK_WHY];
    if (t->concl != 0 && !t->conflict) {
        /* 想通了：把它从"未解"里收起来（**只动内存**），并留一句下次要说的话 */
        think_open_done(t, pick);
        t->mention_pending = 1;
        snprintf(t->mention_text, sizeof t->mention_text,
                 "你 上次 问 的 %s，我 自己 又 想 了 想 ： %s %s %s %s",
                 pick->text,
                 pick->subj && pick->subj->name ? pick->subj->name : "?",
                 pick->want_neg ? "不 " : "",
                 rel_name_of(ai, pick->rid) ? rel_name_of(ai, pick->rid) : "?",
                 pick->obj && pick->obj->name ? pick->obj->name : "?");
        snprintf(note, sizeof note, "没人说话，我自己挑了一个没想通的问题（%s）—— 想通了",
                 pick->text);
        think_step(t, TH_LEARN, 1, 80, "空闲思考：想通了", note);
    } else {
        snprintf(note, sizeof note, "没人说话，我自己挑了一个没想通的问题来想：%s",
                 pick->text);
        think_step(t, TH_GOAL, 1, 60, "空闲思考：自己挑的", note);
    }
    return 1;
}

/* ============================ 接管输出（默认关） ============================ */

/* 只处理既有流水线**给不出**的两种情况：
 *   ① 知识自相矛盾 —— 正确的行为是说出来，而不是挑一条说；
 *   ② 上次没想通的问题现在想通了 —— 主动提一句。
 * 其它情况一律返回 0，让原来的「关系层/帧层/检索/拒答」照常走。
 * 这样「思维接管输出」就不会去动那条已经被 21 套回归钉住的基线。 */
static inline int think_take_output(AI *ai, char *out, size_t cap) {
    Think *t = think_of(ai);
    if (!t || !out || cap == 0) return 0;
    if (t->out_off) return 0;
    /* 矛盾：**替换**整句。这时候说一句正常回答等于把矛盾盖住。 */
    if (t->conflict) {
        if (t->trust_pick != 0) {
            /* `::trust on`：两边差得够多时，不再说"我这儿矛盾"，而是**按可信度选边**。 */
            snprintf(out, cap, "我 更 信 可 信 度 高 的 那 一 边 （%d 对 %d）：%s",
                     t->trust_c1, t->trust_c2, t->concl_text);
        } else if (t->chain_text2[0])
            snprintf(out, cap, "%s（另 一条 依据 ： %s）", t->concl_text, t->chain_text2);
        else
            snprintf(out, cap, "%s", t->concl_text);
        t->took_output = 1;
        return 1;
    }
    /* 缓存被推翻：**替换**整句 —— 这一轮的答案本来就是错的
     * （那条缓存在 R0 直接被当成事实答了出来），必须当场改口。 */
    if (t->flipped) {
        snprintf(out, cap, "%s（我原先记的是反的，按现在的依据改过来了）",
                 t->concl_text[0] ? t->concl_text : "……");
        t->flipped = 0;
        t->took_output = 1;
        return 1;
    }
    /* 前提没成立：**追加**一句说明（默认关；`::premiseout on` 打开）。
     * 追加而不是替换：原来的回答（哪怕是降级答复）仍然是这一轮的主题，
     * 补一句"为什么我没用那条我知道的连结"是信息，不是抢话。 */
    if (t->premise_out_on && t->premise_blocked && t->premise_text[0]) {
        size_t l = strlen(out);
        if (l && l + 4 < cap) snprintf(out + l, cap - l, "（%s）", t->premise_text);
        else if (!l)          snprintf(out, cap, "%s", t->premise_text);
        t->took_output = 1;
        return 1;
    }
    /* 跨轮想通：**追加**在正常回答后面 —— 它是一条补充，
     * 不该把用户这一轮问的东西挤掉（那样就成了自说自话）。 */
    if (t->mention_pending && t->mention_text[0]) {
        size_t l = strlen(out);
        if (l && l + 4 < cap) snprintf(out + l, cap - l, "（%s）", t->mention_text);
        else if (!l)          snprintf(out, cap, "%s", t->mention_text);
        t->mention_pending = 0;
        t->took_output = 1;
        return 1;
    }
    return 0;
}

/* ============================ 报告 ============================ */

static inline void think_report(const AI *ai) {
    if (!ai || !ai->think) {
        printf("[思维] 还没起来（跑过一轮对话才有痕迹）\n");
        return;
    }
    const Think *t = (const Think *)ai->think;
    printf("[思维] 想过 %zu 轮 / 累计 %zu 步  开关：每轮想=%s 接管输出=%s 可改脑=%s\n",
           t->n_think, t->n_steps_total,
           t->run_off ? "关" : "开", t->out_off ? "关" : "开", t->learn_off ? "关" : "开");
    printf("       沉淀=%s\n", t->cache_off ? "关" : "开");
    printf("       这一轮：%s（把握 %d%%%s）  停止原因：%s\n",
           t->concl > 0 ? "结论=成立" : t->concl < 0 ? "结论=不成立" : "没有结论",
           t->conf, t->guessed ? "，是类比猜的" : "", think_stop_name(t->stop));
    if (t->concl_text[0]) printf("       结论投影：%s\n", t->concl_text);
    if (t->concl != 0 || t->conflict)
        printf("       依据强度：%s\n",
               think_evidence_name(t->concl_class, t->concl_cache));
    if (t->chain_text[0])  printf("       依据：%s\n", t->chain_text);
    if (t->chain_text2[0])
        printf("       反面依据（强度 %s）：%s\n",
               think_evidence_name(t->other_class, t->other_cache), t->chain_text2);
    printf("       未解问题：当前 %zu 个（挂过 %zu / 想通 %zu / 过期 %zu / 挂满被拦 %zu）\n",
           t->n_open, t->n_asked, t->n_resolved, t->n_expired, t->n_open_blocked);
    printf("       推理沉淀：记住 %zu 条 / 撤销 %zu 条 / 前提没了 %zu 条 / 被拦 %zu 条%s\n",
           ai->n_cached, ai->n_retracted, t->n_stale, t->n_cache_blocked,
           t->cache_off ? "（::thinkcache off 关着）" : "");
    printf("       元认知：原地打转 %zu 次  超预算 %zu 次  发现矛盾 %zu 次  拦住 %zu 次"
           "  回退 %zu 次（共 %zu）\n",
           t->n_stalls, t->n_over_budget, t->n_conflicts, t->n_blocked,
           t->n_backtrack, t->n_backtrack_total);
    printf("       机制注册表（%zu 条）：", think_mech_count());
    for (size_t i = 0; i < think_mech_count(); ++i) {
        const ThinkMech *m = think_mech_by_index(i);
        printf("%s ", m && m->name ? m->name : "?");
    }
    printf("\n");
    if (t->trace_n) {
        printf("       这一轮的思维过程：\n");
        for (size_t i = 0; i < t->trace_n; ++i) {
            const ThinkStep *s = &t->trace[i];
            printf("        %2zu. %-8s %s  %s\n", i + 1, think_kind_name(s->kind),
                   s->ok ? "✓" : "✗", s->what);
            if (s->why[0]) printf("            %s\n", s->why);
        }
    }
    /* N1：开着「思考对象」时这一节打的是**对象**（结构化链 + 复核结果），
     * 不是上面那份文本痕迹 —— 白箱呈现只有这一处（`::thinkobj` 用同一个函数）。 */
    if (t->obj_on) {
        printf("       —— 思考对象（`::thinkobj` 看的就是它）——\n");
        think_obj_print(ai, t, "       ");
    }
}

static inline void think_pending_report(const AI *ai) {
    if (!ai || !ai->think) { printf("[未解问题] 还没有（跑过一轮对话才可能有）\n"); return; }
    const Think *t = (const Think *)ai->think;
    if (t->n_open == 0) {
        printf("[未解问题] 没有挂着的（累计挂过 %zu，想通 %zu，过期 %zu）\n",
               t->n_asked, t->n_resolved, t->n_expired);
        return;
    }
    printf("[未解问题] 还挂着 %zu 个（跨轮会拿新知识再试%s）：\n", t->n_open,
           t->n_restored ? "；其中一部分是装载时从 [自问:…] 恢复的" : "");
    for (size_t i = 0; i < THINK_MAX_OPEN; ++i) {
        const ThinkOpen *o = &t->open[i];
        if (!o->active) continue;
        printf("  - %-40s 试过 %zu 次%s\n", o->text, o->tries,
               o->stalled ? "（已停止再试，等它自然过期）" : "");
    }
    printf("     累计：挂过 %zu / 想通 %zu / 过期 %zu / 挂满被拦 %zu / 装载恢复 %zu\n",
           t->n_asked, t->n_resolved, t->n_expired, t->n_open_blocked, t->n_restored);
}

/* 一行摘要，挂在 ::stats 后面（和 micro_stats 同一个位置，一眼看出「它在想什么」） */
static inline void think_stats(const AI *ai) {
    if (!ai || !ai->think) {
        printf("       思维：还没起来（没想过）\n");
        return;
    }
    const Think *t = (const Think *)ai->think;
    printf("       思维：想过 %zu 轮/%zu 步  这一轮 %s（%d%%）  停因=%s  "
           "未解问题=%zu(想通%zu/过期%zu)  打转%zu 矛盾%zu 回退%zu\n",
           t->n_think, t->n_steps_total,
           t->concl > 0 ? "成立" : t->concl < 0 ? "不成立" : "无结论",
           t->conf, think_stop_name(t->stop),
           t->n_open, t->n_resolved, t->n_expired, t->n_stalls, t->n_conflicts,
           t->n_backtrack_total);
}

static inline void think_set_run(AI *ai, int on) {
    Think *t = think_of(ai);
    if (t) t->run_off = on ? 0 : 1;
}
static inline void think_set_out(AI *ai, int on) {
    Think *t = think_of(ai);
    if (t) t->out_off = on ? 0 : 1;
}
static inline void think_set_learn(AI *ai, int on) {
    Think *t = think_of(ai);
    if (t) t->learn_off = on ? 0 : 1;
}
/* 推理沉淀开关：想通的东西要不要记下来（默认开） */
static inline void think_set_cache(AI *ai, int on) {
    Think *t = think_of(ai);
    if (t) t->cache_off = on ? 0 : 1;
}
static inline void think_set_budget(AI *ai, size_t steps) {
    Think *t = think_of(ai);
    if (t && steps > 0) t->max_steps = steps > THINK_MAX_STEP ? THINK_MAX_STEP : steps;
}

#endif /* THINK_H */
