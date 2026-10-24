/* micro.h — 微内核：安全内核 + 自修改（修改意图队列）+ 补丁边界 + 元认知
 *
 * 这是「关联机」定义里第二条（**符号节点和关联在运行时被系统自身修改**）
 * 的落地层。核心的其它部分（token.h 的图、ai.h 的认知）都不动，
 * 结构修改统一走这一条通道：
 *
 *     激活 / 学习 / 补丁
 *          │  只**提议**（ModIntent），不直接改结构
 *          ▼
 *     修改意图队列 ModQueue
 *          │  安全点（一轮的末尾，不在任何遍历里）
 *          ▼
 *     安全内核 safety_review ──拒绝──→ 审计（理由分类，可查）
 *          │  允许
 *          ▼
 *     应用（加符号节点 / 加边 / 打关系 / 剪枝 …）+ 记账（预算）
 *          │
 *          ▼
 *     元认知 micro_metacog：看速率、拒绝率、补丁健康
 *          └─ 必要时**自己踩刹车**（冻结 / 关小提案），并留下提醒
 *
 * 为什么必须「提议 → 安全点应用」而不是就地改：
 *   遍历中 realloc 会让 `AssocEntry *e` 当场失效；`for (i < reach)` 里
 *   `count` 变了会漏边；路径栈里没有新符号节点会被重复激活。
 *   所以 on_visit / 补丁里**只记录想干什么**，结构不动。
 *   （token.h 的注释「on_visit 内不得对当前符号节点做 add_assoc」就是这个边界。）
 *
 * 补丁边界（微内核的「外设」）：
 *   核心只认符号节点。补丁负责把外部信号变成符号节点（sense），
 *   或把核心给的符号节点变成外部动作（act），或提议结构修改（propose）。
 *   三条硬规矩：
 *     ① 补丁只交换符号节点，不碰核心内部结构（要改结构就提议）；
 *     ② 补丁可缺失：没有补丁，内核照跑（本文件里 sense/act 全是空转）；
 *     ③ 补丁可失败：崩溃/超时/连续出错 → 自动停用并留下提醒，不影响内核。
 *   本仓库**不预置任何硬件补丁**（视觉/3D/听觉那些是「外设」，与内核职责无关）。
 *   内建的只有一个 `自省` 补丁：它把**内核自己的状态**变成符号节点，
 *   用来证明这条边界真的能用，且不依赖任何外部设备。
 *
 * 依赖：token.h + ai.h（本文件在 ai.h 末尾被包含，此时 AI 已完整）。
 * 不含：语言层、帧层、关系层 —— 它们照旧，不感知微内核。
 */
#ifndef MICRO_H
#define MICRO_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ============================ 常量 ============================ */

#define MICRO_QUEUE_MAX        64   /* 一轮最多攒多少条意图 */
#define MICRO_APPLY_MAX_STEP   16   /* 一步最多应用多少条（安全点总闸） */
#define MICRO_AUDIT_MAX        48   /* 审计环长度 */
#define MICRO_APPLY_LOG_MAX    16   /* 「刚应用了什么」环长度 */
#define MICRO_PATCH_MAX        16   /* 补丁注册表容量 */
#define MICRO_WINDOW           16   /* 元认知看最近多少轮 */
#define MICRO_REMIND_COOLDOWN   8   /* 同一种提醒至少隔多少轮才再提 */
#define MICRO_D1_MIN_EDGES      6   /* 共现固化的门槛：两个词的出边都不少于这么多 */
#define MICRO_D1_MIN_COMMON     4   /* …且共同邻居不少于这么多 */
#define MICRO_D1_JACCARD_NUM  100   /* …且 Jaccard ≥ 0.30（整数比，不用浮点） */
#define MICRO_D1_JACCARD_DEN  333
#define MICRO_D1_PER_STEP       3   /* 每轮最多固化几条边 */

/* ---- D4「条件依赖」：**前提的生产端** ----
 * 用户要的「前提自动长出来」：不是手打 `::op 加前提`，而是从**反复出现的谈话**里
 * 自己发现"A 与 B 的连结几乎总伴随着 P" → 提议 `A —[前提]→ P → B`（照样过安全内核）。
 * 判据全在**流**里（同轮 = 这一轮的输入词），不新造字段、不动存档格式。 */
#define MICRO_D4_MAX_TRIPLES   256   /* (A,B,P) 表大小（有界）。
                                      * 实测教训：48/64 在真实语料上**秒满**，淘汰一轮之后
                                      * 分母表里那对词早被挤掉 → 比值算出 0% 或 171% 这种鬼数
                                      * （分子还在、分母没了）。现在分母**存在三元组行里**，
                                      * 不跨表查，也就不会再被挤掉。 */
/* 门槛是**在真实语料上量出来的**（tools/d4probe.c，回放 _corpus.txt 2 万行）：
 *   分母 ≥3 时 253 条候选，70% 一档筛出 18 条 —— 但里面有 `4/5 = 80%`、`6/8 = 75%`
 *   这种**小样本巧合**（分母才 5 次就敢当前提，太轻率）。
 *   抬到 分母 ≥8、分子 ≥5 之后剩 ~10 条，留下的都是几十上百次里稳定 ≥70% 的模式。
 *   宁可少提，不提错 —— 它建的是**结构**（还要拆直连），错了比不提糟。 */
#define MICRO_D4_MIN_PAIRS      8    /* A、B 至少同轮 8 次才谈"前提"（小样本不算数） */
#define MICRO_D4_MIN_TRIPLE     5    /* 其中 P 也在场至少 5 次 */
#define MICRO_D4_PCT_NUM        70   /* 且 n_triple/n_pair ≥ 70%（整数比，不用浮点） */
#define MICRO_D4_PCT_DEN       100
#define MICRO_D4_PER_TURN       1    /* 每轮最多提一条（宁可少动，不动错） */
#define MICRO_D4_MIN_PAIR_HITS  2    /* 那条连结本身至少被命中过 2 次才算"观测到的连结"——
                                      * 否则检测器会拿**自己刚造出来的边**当依据
                                      * （和"缓存不许互相为依据"是同一条纪律） */
#define MICRO_D2_EVERY        100   /* 缺口命名：至少隔多少轮才允许造一个符号节点 */
#define MICRO_D2_GAP_NUM       65   /* 空缺感 ≥ 0.65 */
#define MICRO_D2_GAP_DEN      100
#define MICRO_D3_MIN_AGE     2000   /* 剪枝：边至少这么久没被碰过 */
#define MICRO_D3_PER_STEP       1
#define MICRO_PATCH_SILENT     40   /* 感知补丁连续多少轮没输出算「沉默」 */
#define MICRO_OP_MAX_INTENTS    8   /* 一条「符号节点行为」一次最多展开几条修改意图 */

/* ---- 修改意图的种类 ---- */
typedef enum {
    MOD_ADD_TOKEN = 1,      /* 造一个新符号节点，并接回触发它的上下文 */
    MOD_ADD_ASSOC,          /* 加一条共现边 */
    /* **沉淀**：把「推出来并且验证过」的东西记成图里的一条边，
     * 但标上 ASSOC_ROLE_DERIVED —— 它比观测弱、可被观测推翻、可撤销。
     * 这是「学」的第三条路：不是听到的，也不是共现出来的，而是**想出来的**。 */
    MOD_TAG_DERIVED,
    MOD_TAG_REL,            /* 给一条边打上语义关系 */
    MOD_SET_ACTION,         /* 给一条边挂动作 */
    MOD_SET_PARENT,         /* 设父类（类别继承） */
    MOD_SET_CATEGORY,       /* 标成类别 */
    MOD_BUMP_HITS,          /* 给一条边加权 */
    MOD_DECAY,              /* 衰减一个符号节点的所有边 */
    MOD_REMOVE_ASSOC,       /* 删一条边 */
    /* **在 A 与 B 之间插一个符号节点**：A→N→B，并让 A、B **不直连**
     * （直连边要不要拆由安全内核裁决 —— 关系边受铁律 2 保护，拆不掉就如实说）。
     * 这是「给符号节点加行为」里那个复合动作：一次意图做成三件事（造 N + 两条边 + 拆直连），
     * 所以它必须是**一条**意图 —— 分成三条的话，拆直连会在 N 还没建好时就执行。 */
    MOD_INSERT_BETWEEN,
    /* 回收一个**临时符号节点**（草稿）。硬约束：只许删 [临: 命名空间、
     * 且必须已经没有任何边引用它（含指向它的边）—— 别的符号节点一律拒绝。 */
    MOD_REMOVE_TOKEN,
    /* **调用补丁的操作**：AI 想对外设说话（输出侧的记事本、草稿区、发送……
     * 具体是什么由补丁自己解释）。它不改图，但**照样走受审通道** ——
     * 预算、审计、日志、故障隔离与图改动同一规格：补丁再乱来也只能碰自己的缓冲。 */
    MOD_PATCH_CALL,
} ModType;

/* MOD_INSERT_BETWEEN 的 flags */
#define MOD_FLAG_SPLIT_DIRECT 1u   /* 拆掉 A—B 那条直连边（拆不动就保留并说明） */
#define MOD_FLAG_MARK_PREMISE 2u   /* 把 A→N 这条边标成「前提链接」（N 就是前提符号节点） */
#define MOD_FLAG_MARK_PREMISE_NOT 4u /* 标成「**反**前提」：条件是 N 不在注意窗口里 */
#define MOD_FLAG_PROCESS 8u        /* 这条边是**过程边**（role=过程，第 9 条 ①）——
                                    * 由 `::op 加步骤/加调用/加回边` 提出来，走同一条受审通道 */
#define MOD_FLAG_PROC_STEP 16u     /* 动作槽挂 `[步骤:N]`（N 放 `m->rel_id`） */
#define MOD_FLAG_PROC_CALL 32u     /* 动作槽挂 `[调用]` */
#define MOD_FLAG_PROC_BACK 64u     /* 动作槽挂 `[回边]` */
#define MOD_FLAG_PROC_ELSE 128u    /* 动作槽挂 `[否则]`（固定记号，用 flag 位） */
/* `[条件:X]`/`[异常:类型]` 不另占位：**记号名**（带参数）放 `m.action` 里直接带过去。 */

/* ---- 安全内核的拒绝理由（审计按它分类统计）---- */
typedef enum {
    SAFE_OK = 0,
    SAFE_DENY_SWITCH,        /* 自修改总开关关着 */
    SAFE_DENY_FROZEN,        /* 预算长期触顶已冻结 */
    SAFE_DENY_BUDGET_EDGE,   /* 本步新建边超预算 */
    SAFE_DENY_BUDGET_TOKEN,  /* 本步新建符号节点超预算 */
    SAFE_DENY_TOTAL,         /* 池子总量触顶 */
    SAFE_DENY_EDGE_CAP,      /* 单个符号节点的边数触顶 */
    SAFE_DENY_RESERVED,      /* 不许改保留符号节点（[原语:…]/[时刻:…]/[提醒:…]） */
    SAFE_DENY_RELATION,      /* 不许删关系边（那是知识，不是共现） */
    SAFE_DENY_CYCLE,         /* 父节点不许接成环 */
    SAFE_DENY_SELF,          /* 自环 */
    SAFE_DENY_BADREF,        /* 引用了空/垃圾符号节点 */
    SAFE_DENY_TYPE,          /* 这一类修改被开关关掉了 */
    SAFE_DENY_RATE,          /* 速率闸：元认知把提案关小了 */
    SAFE_DENY_NOT_TEMP,      /* 只许回收「临时符号节点」（草稿纸），别的符号节点不许删 */
    SAFE_DENY_BUDGET_PATCH,  /* 本步调用补丁操作的次数超预算 */
    SAFE_DENY_NOPATCH,       /* 目标补丁没挂载/已停用/不认这个操作 */
    SAFE_REASON_N
} SafeReason;

static const char *const SAFE_REASON_NAME[SAFE_REASON_N] = {
    "允许", "自修改总开关关着", "已冻结(预算长期触顶)", "本步新建边超预算",
    "本步新建符号节点超预算", "池子总量触顶", "单符号节点边数触顶",
    "不许改保留符号节点", "不许删关系边", "父节点不许成环",
    "不许自环", "引用了空/垃圾符号节点", "这一类修改被关着", "速率闸(元认知关小)",
    "只许回收临时符号节点",
    /* ⚠ 这两条**曾经漏掉**（枚举里加了、名字表没加）⇒ 一旦真被它们拒，
     * 审计里打出来的是 `(null)`（实测：N2 的轮中草稿把每步补丁预算挤爆时看到）。
     * 名字表必须**逐条**和 SafeReason 对上 —— tools/microtest.c 里有一条自检钉着。 */
    "本步调用补丁操作的次数超预算",
    "目标补丁没挂载/已停用/不认这个操作",
};

/* ---- 元认知提醒的种类 ---- */
typedef enum {
    MICRO_REMIND_OVERHEAT = 0,   /* 自修改速率失控 → 冻结 */
    MICRO_REMIND_FROZEN,         /* 冻结生效（说给用户听） */
    MICRO_REMIND_DENIED_MOST,    /* 提案几乎全被安全内核否掉 → 关小提案 */
    MICRO_REMIND_THROTTLE,       /* 关小生效 */
    MICRO_REMIND_PATCH_ERR,      /* 补丁连续出错 → 已自动停用 */
    MICRO_REMIND_PATCH_SILENT,   /* 补丁沉默（挂着但一直没输出） */
    MICRO_REMIND_PATCH_CONFLICT, /* 两个补丁对同一个符号节点给出相反信号 */
    MICRO_REMIND_TOTAL,          /* 池子总量触顶 */
    /* 思维层（think.h）也要用同一套提醒记账 —— 元认知是一件事，不是两件 */
    MICRO_REMIND_THINK_STALL,    /* 思维在原地打转 */
    MICRO_REMIND_THINK_OPEN,     /* 想不通的问题挂得太多 */
    MICRO_REMIND_THINK_CONFLICT, /* 知识自相矛盾（正反都能推出来） */
    MICRO_REMIND_DIRTY_REPEAT,   /* §6.139 说出口的话在重复（脏思考，已截停） */
    MICRO_REMIND_N
} MicroRemind;

static const char *const MICRO_REMIND_NAME[MICRO_REMIND_N] = {
    "自修改过热", "已冻结", "提案多被否", "已关小提案",
    "补丁异常已停用", "补丁沉默", "补丁信号冲突", "池子触顶",
    "思维原地打转", "未解问题太多", "知识自相矛盾", "输出在重复（脏思考）",
};

/* ============================ 小工具 ============================ */

/* 保留名：以 '[' 开头的是**标记**而不是内容词（[原语:…]/[时刻:N]/[对话:结束]/[提醒:…]…）。
 * 自修改不许拿它们当目标 —— 关系词表、时间线、句界这些是内核的骨架。 */
static inline int micro_reserved(const char *name) {
    return name && name[0] == '[';
}
static inline int micro_token_reserved(const Token *t) {
    return !t || t->is_garbage || micro_reserved(t->name);
}

/* 「造新符号节点」的**命名空间**：自修改造出来的概念统一用这个前缀。
 * 保留名规则本来是一律不许碰 '[' 开头的名字（那些是 [原语:…] / [时刻:…] 这类骨架），
 * 但「把未知具体化」造出来的概念本身也该是可辨识的内部节点，
 * 所以给它开一个专用前缀，其余 '[' 前缀照旧一律拒绝。 */
#define MICRO_NEW_TOKEN_PREFIX "[新概念:"
#define MICRO_ASK_TOKEN_PREFIX "[自问:"
/* §6.188 **等回话的追问**（askback 的 pending）：`[追问:kind|neg|factneg|R|A|B]`。
 * 它是**普通符号节点**（随图存盘）⇒ 存盘再开还能接着答（pending 本身只在内存）。
 * 回收时标 `is_garbage`（不删节点 ⇒ 存档 id 仍然连续，§6.186）。 */
#define MICRO_PENDING_TOKEN_PREFIX "[追问:"
/* §6.143 元认知提醒的节点前缀。**收进内部命名空间**（下面 `micro_internal_ns`）之后，
 * `[提醒:…]` 就不再算"协议骨架" ⇒ 它跟「我」之间那条边**可以过统一通道**（受审 + 记账 + 审计）。
 * 不这么做的话：`micro_endpoint_reserved()` 判真 ⇒ 命中 K6 的静态例外 ⇒ 永远直写
 * （外部审计点名的 P1-2 就是这一处）。 */
#define MICRO_REMIND_TOKEN_PREFIX "[提醒:"
/* **临时符号节点（草稿纸）**：只在会话里活着的中间节点 —— 例如「在这两个词之间插一个
 * 符号节点，让它们不直连、经它中转」时那个中转节点。它**能参与边**，但**不存盘**、
 * 没人引用时会被回收（见 model_io.h 的跳过规则与 MOD_REMOVE_TOKEN）。
 * 命名空间规则和上面两个一模一样：这里放行，别的 '[' 名字照旧拒绝。 */
#define MICRO_TEMP_TOKEN_PREFIX   "[临:"
/* **修正的记忆（来源监控）**：改口时留的一个标记节点，名如
 * `[修正:猫_不_有_毛]`。它是**留痕**，不是知识：不进关系表、不参与走边。 */
#define MICRO_REVISE_TOKEN_PREFIX "[修正:"
/* **撤销留痕**（§6.78）：缓存被撤销时写一条 `[撤销:A|R|B]` 标记节点 ——
 * 撤掉的东西图里本来没有痕迹，于是 `n_retracted` 重载后恢复不了（推论档后验偏乐观）。
 * 有了标记，`ai_recount_evidence` 就能从图里把数**数回来**（零格式改动）。 */
#define MICRO_REVOKE_TOKEN_PREFIX "[撤销:"
/* **草稿持久化**用的标记：`[草稿:<文本>]`（普通符号节点，随图存盘）。
 * 草稿区本身是瞬态的，靠它恢复 —— 和未解问题用 `[自问:…]` 同一个路子，**不改格式**。 */
#define MICRO_DRAFT_TOKEN_PREFIX "[草稿:"
/* **外部执行的观测标记**：`[运行:<rc>]`（普通符号节点，随图存盘）——
 * `action.h` 每跑一次外部命令落一个（rc = 退出码，超时约定 -9）。
 * 和 `[草稿:…]` 一样是**留痕/观测**，不是知识：只连回宿主"我"，不建关系边、不进关系表。 */
#define MICRO_RUN_TOKEN_PREFIX "[运行:"
/* **写代码闭环的观测标记**：`[代码:<最终rc>]`（普通符号节点，随图存盘）——
 * `code.h` 走完一轮"写-跑-看错-改"落一个。和 `[运行:…]` 一样是留痕，不是知识。 */
/* **动作符号节点**（① 档最后一条，§6.82）：`[执行:动作词 参数…]` —— 图上的一个普通节点，
 * 也是一个步骤的**目标**。过程跑到这一步时，如果它真出现在步骤位上，就走 `action_run()`
 * **真的调一次**（白名单 + 超时 + 账，都在 action.h 那层）。干跑时只核过、不动外部世界。 */
#define MICRO_EXEC_TOKEN_PREFIX "[执行:"
#define MICRO_CODE_TOKEN_PREFIX "[代码:"
#define MICRO_TOKEN_PROC_PREFIX PROC_TOKEN_PREFIX   /* 过程根：`[过程:名字]`（第 9 条 ①） */

/* 内核自有的**图内节点**命名空间（可以参与边）。
 * 别的 '[' 名字是协议骨架（[原语:]/[时刻:]/[提醒:]…），一律不许碰。 */
static inline int micro_internal_ns(const char *name) {
    static const char *const ns[] = { MICRO_NEW_TOKEN_PREFIX, MICRO_ASK_TOKEN_PREFIX,
                                      MICRO_PENDING_TOKEN_PREFIX,
                                      MICRO_TEMP_TOKEN_PREFIX, MICRO_REVISE_TOKEN_PREFIX,
                                      MICRO_DRAFT_TOKEN_PREFIX, MICRO_RUN_TOKEN_PREFIX,
                                      MICRO_CODE_TOKEN_PREFIX, MICRO_EXEC_TOKEN_PREFIX,
                                      MICRO_TOKEN_PROC_PREFIX, MICRO_REMIND_TOKEN_PREFIX,
                                      MICRO_REVOKE_TOKEN_PREFIX, NULL };
    if (!name) return 0;
    /* `[前提]` 是**行为符号节点（标记）**：挂在边的动作槽上，表示「那一端是前提」。
     * 它是精确名字，不是前缀命名空间。 */
    if (strcmp(name, AI_PREMISE_MARK) == 0) return 1;
    if (strcmp(name, AI_PREMISE_NOT_MARK) == 0) return 1;
    /* 过程记号（第 9 条 ②）也是**我们自己的记号**：可以当端点 ——
     * 「回边」就是要指回某一步（`[步骤:N]`），不许它当端点的话回边根本建不起来
     * （实测：被拒理由「不许改保留符号节点」）。 */
    if (strncmp(name, PROC_STEP_PREFIX, strlen(PROC_STEP_PREFIX)) == 0) return 1;
    if (strcmp(name, PROC_CALL_MARK) == 0) return 1;
    if (strcmp(name, PROC_BACK_MARK) == 0) return 1;
    if (strncmp(name, PROC_COND_PREFIX, strlen(PROC_COND_PREFIX)) == 0) return 1;
    if (strcmp(name, PROC_ELSE_MARK) == 0) return 1;
    if (strncmp(name, PROC_EXC_PREFIX, strlen(PROC_EXC_PREFIX)) == 0) return 1;
    for (size_t i = 0; ns[i]; ++i)
        if (strncmp(name, ns[i], strlen(ns[i])) == 0) return 1;
    return 0;
}
static inline int micro_reserved_new_name(const char *name) {
    if (!micro_reserved(name)) return 0;
    return !micro_internal_ns(name);
}
/* 这个符号节点是**临时草稿**吗（名字前缀是唯一判据 —— 它必须不依赖存档字段，
 * 这样跳过存盘才是格式安全的）。 */
static inline int token_is_temp(const Token *t) {
    return t && t->name &&
           strncmp(t->name, MICRO_TEMP_TOKEN_PREFIX, strlen(MICRO_TEMP_TOKEN_PREFIX)) == 0;
}
/* **端点**能不能被连线：普通词随便；'[' 名字只许内核自有命名空间。
 * 注意 `micro_token_reserved()`（统计/检测器用）仍然**跳过**这些内部标记 ——
 * 它们是草稿/留痕，不该污染邻居重叠与共现固化。这两件事必须分开：
 * 「能不能当端点」和「算不算内容」不是同一个问题。 */
static inline int micro_endpoint_reserved(const Token *t) {
    if (!t || t->is_garbage) return 1;
    if (!micro_reserved(t->name)) return 0;
    return !micro_internal_ns(t->name);
}

/* a→b 这条**普通边**在不在？返回下标，-1 = 没有。
 * v10：语境字段没了，但判据不是"只看 main" —— **因果猜想边不算**（`[因果]` 标记）。
 * v9 里 `n_subs == 0` 那个条件是同一个意思（猜想边当时带语境）：
 * 少了这一条，MOD_TAG_DERIVED / MOD_REMOVE_ASSOC 会去动猜想边（实测让"撤销缓存"静默失效）。 */
static inline long micro_edge_index(const Token *t, const Token *main) {
    if (!t || !main) return -1;
    for (size_t i = 0; i < t->count; ++i)
        if (token_assocs_ro(t)[i].main == main && edge_is_plain(&token_assocs_ro(t)[i])) return (long)i;
    return -1;
}
static inline int micro_has_edge(const Token *t, const Token *main) {
    return micro_edge_index(t, main) >= 0;
}
/* 说明（历史）：这里原来还有一对 `micro_edge_index_ctx` / `micro_has_edge_ctx`（按语境查边）。
 * 语境退役、字段删掉（v10）之后它们没有意义了 —— 删掉，别留"看着像机制"的空函数。 */
/* 这条边是**推出来沉淀的**吗？（缓存可撤，观测不可撤 —— 安全内核要用） */
static inline int micro_edge_is_derived(const Token *t, const Token *main) {
    return token_edge_is_derived(t, main);
}

/* 出邻居（跳过垃圾与保留符号节点），最多 cap 个 */
static inline size_t micro_neighbors(const Token *t, Token **out, size_t cap) {
    size_t n = 0;
    if (!t) return 0;
    for (size_t i = 0; i < t->count && n < cap; ++i) {
        Token *m = token_assocs_ro(t)[i].main;
        if (micro_token_reserved(m)) continue;
        out[n++] = m;
    }
    return n;
}

/* 两个词的出邻居重合度够不够（共同邻居 + Jaccard，全用整数算）。
 * 这是「它们是不是一类东西」的**图内证据** —— 共现固化拿它当依据，
 * 不是拍脑袋随机接线。 */
static inline int micro_jaccard_at_least(const Token *a, const Token *b, size_t min_common) {
    Token *na[64], *nb[64];
    size_t ca = micro_neighbors(a, na, 64);
    size_t cb = micro_neighbors(b, nb, 64);
    if (ca < MICRO_D1_MIN_EDGES || cb < MICRO_D1_MIN_EDGES) return 0;
    size_t common = 0;
    for (size_t i = 0; i < ca; ++i)
        for (size_t j = 0; j < cb; ++j)
            if (na[i] == nb[j]) { common++; break; }
    if (common < min_common) return 0;
    size_t uni = ca + cb - common;
    /* common/uni ≥ NUM/DEN ⇔ common*DEN ≥ NUM*uni */
    return (common * MICRO_D1_JACCARD_DEN) >= (MICRO_D1_JACCARD_NUM * uni);
}

static inline const char *micro_mod_type_name(ModType t) {
    switch (t) {
    case MOD_ADD_TOKEN:      return "加符号节点";
    case MOD_ADD_ASSOC:      return "加边";
    case MOD_TAG_DERIVED:    return "沉淀";
    case MOD_TAG_REL:        return "打关系";
    case MOD_SET_ACTION:     return "设动作";
    case MOD_SET_PARENT:     return "设父类";
    case MOD_SET_CATEGORY:   return "设类别";
    case MOD_BUMP_HITS:      return "强化";
    case MOD_DECAY:          return "衰减";
    case MOD_REMOVE_ASSOC:   return "删边";
    case MOD_INSERT_BETWEEN: return "插词";   /* 在两个词之间插一个符号节点（经它中转） */
    case MOD_REMOVE_TOKEN:   return "回收临时";
    case MOD_PATCH_CALL:     return "调用补丁";
    default:                 return "?";
    }
}

/* ============================ 修改意图 ============================ */

typedef struct {
    ModType  type;
    Token   *target;                    /* 对哪个符号节点动手 */
    Token   *main;                      /* 边的另一头 / 新符号节点要连回谁 */
    Token   *action;                    /* 设动作 */
    uint8_t  rel_id;                    /* 打关系 / 沉淀 */
    uint8_t  neg;                       /* 沉淀时：记的是不是「不 R」 */
    uint8_t  flags;                     /* 复合意图的开关位（见 MOD_FLAG_*） */
    uint32_t bump;                      /* 加权 */
    char     name[96];                  /* 加符号节点 / 插词：名字；调用补丁：操作名。
                                         * 96 = 给 `[自问:<问句>]` 留 ~86 字节正文
                                         * （以前 48 只够 ~38 字节，恢复出来的问句被截得厉害） */
    char     source[24];                /* 谁提的（检测器名 / 补丁名） */
    char     patch[24];                 /* MOD_PATCH_CALL：找哪个补丁 */
    char     args[4][96];               /* MOD_PATCH_CALL：最多 4 个字符串参数。
                                         * 96 = 让"结论 +（依据：链）"这类文本能整段带过去
                                         * （40 字节会把 `猫 -[会·缺前提]-> 唱歌` 拦腰截断） */
    uint8_t  n_args;
} ModIntent;

typedef struct {
    ModIntent items[MICRO_QUEUE_MAX];
    size_t    n;
    size_t    n_proposed;    /* 累计**提过**多少条（含被去重和溢出丢掉的） */
    size_t    n_dedup;       /* 其中去重掉的 */
    size_t    n_dropped;     /* 其中因队列满丢掉的 */
} ModQueue;

static inline void mod_queue_clear(ModQueue *q) {
    if (q) q->n = 0;
}

/* 去重键：同一类操作 + 同一对端点 = 同一条意图。
 * 没有它，一轮里同一对词被两个检测器同时看中就会建两条边。 */
static inline uint32_t micro_intent_key(const ModIntent *m) {
    if (!m) return 0;
    uint32_t h = 2166136261u;
    #define MIXK(v) do { h ^= (uint32_t)(uintptr_t)(v); h *= 16777619u; } while (0)
    MIXK((uintptr_t)m->type);
    MIXK((uintptr_t)m->target);
    MIXK((uintptr_t)m->main);
    MIXK((uintptr_t)m->rel_id);
    MIXK((uintptr_t)m->neg);          /* 正/负是两条不同的意图（否则同一步里会被去重） */
    MIXK((uintptr_t)m->flags);
    for (const char *q = m->name; q && *q; ++q) MIXK((unsigned char)*q);
    for (const char *q = m->patch; q && *q; ++q) MIXK((unsigned char)*q);
    MIXK((uintptr_t)m->n_args);
    for (uint8_t i = 0; i < m->n_args && i < 4; ++i)
        for (const char *q = m->args[i]; q && *q; ++q) MIXK((unsigned char)*q);
    #undef MIXK
    return h ? h : 1u;
}

/* 提议一条意图：去重 + 限额。返回 1 = 收下，0 = 丢了（重复或队列满）。 */
static inline int mod_propose(ModQueue *q, const ModIntent *m) {
    if (!q || !m) return 0;
    q->n_proposed++;
    if (q->n >= MICRO_QUEUE_MAX) { q->n_dropped++; return 0; }
    uint32_t key = micro_intent_key(m);
    for (size_t i = 0; i < q->n; ++i)
        if (micro_intent_key(&q->items[i]) == key) { q->n_dedup++; return 0; }
    q->items[q->n++] = *m;
    return 1;
}
/* 按最简单的形状提议（补丁侧方便用） */
static inline int mod_propose_simple(ModQueue *q, ModType type, Token *target,
                                     Token *main, const char *source) {
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = type; m.target = target; m.main = main;
    snprintf(m.source, sizeof m.source, "%s", source ? source : "?");
    return mod_propose(q, &m);
}

/* ============================ 安全内核 ============================ */

typedef struct {
    uint64_t tick;
    char     op[16];
    char     target[64];
    int      allowed;
    char     reason[48];
} SafetyAudit;

typedef struct {
    /* ---- 硬预算（可调，但都在 ::safe 里看得见）---- */
    int    max_new_tokens_step;   /* 一步最多造几个符号节点（默认 2） */
    int    max_new_edges_step;    /* 一步最多新建几条边（默认 8） */
    int    max_edges_per_token;   /* 单个符号节点边数上限（默认 512） */
    int    max_patch_calls_step;  /* 一步最多调用几次补丁操作（默认 4；外设也是资源） */
    int    max_tokens_total;      /* 池子总量上限（默认 400000） */
    /* ---- 开关（反向命名：calloc 出来的就是默认值）----
     * 自修改默认**开**。想关就 ::mod off。 */
    int    mod_off;               /* 0 = 自修改通道开（默认） */
    int    new_token_off;         /* 0 = 允许造新符号节点（默认，预算极紧） */
    int    remove_off;            /* 0 = 允许删边（默认，判据苛刻） */
    /* ---- 停止条件 ---- */
    int    freeze;                /* 1 = 冻结：一律拒绝，等手段解除 */
    size_t n_freeze;              /* 冻结过几次 */
    /* ---- 记账 ---- */
    size_t step_new_tokens, step_new_edges, step_patch_calls;
    size_t n_allowed, n_denied;
    size_t deny_by_reason[SAFE_REASON_N];
    size_t n_applied_by_type[16];
    SafetyAudit audit[MICRO_AUDIT_MAX];
    size_t audit_n;
} SafetyKernel;

/* 这条边能不能被拆？**规则只有一份**：REMOVE_ASSOC 与「插词拆直连」都用它。
 * 知识不是共现：观测到的关系边（rel/nrel/dyn）不许剪；推出来沉淀的缓存例外。 */
static inline int micro_edge_removable(const AssocEntry *e) {
    if (!e) return 0;
    /* **过程边是结构，不是共现**（第 9 条 ③）：插词/加前提那些"拆直连"的动作不许拆它 ——
     * 拆了就等于把"第 N 步"从过程里删掉（实测：给一步挂前提时，`加前提` 顺手把步骤边拆了，
     * 过程当场少一步）。和"直连是知识时不拆"同一个道理。 */
    if (edge_is_process(e)) return 0;
    if (e->rel || e->nrel || e->dyn) return e->role == ASSOC_ROLE_DERIVED;
    return 1;
}

/* 有没有**任何** 符号节点的边指向 t？（回收临时草稿前必须确认为假）
 * 全池扫一遍、找到就收；只在回收草稿时调用，代价可忽略。 */
static inline int micro_token_has_in_edge(const TokenPool *p, const Token *t) {
    if (!p || !t) return 1;                      /* 判不准 ⇒ 当作有人引用（保守） */
    for (size_t i = 0; i < p->count; ++i) {
        const Token *s = p->tokens[i];
        if (!s || s == t) continue;
        for (size_t j = 0; j < s->count; ++j)
            if (token_assocs_ro(s)[j].main == t) return 1;
    }
    return 0;
}

/* 纯判定：不动账。**先审查、再记账、再应用**（见 safety_charge 的注释：
 * 记账必须在应用之前，否则「这是不是新边」永远判成假，预算就形同虚设）。 */
static inline SafeReason safety_review(const SafetyKernel *sk, const TokenPool *pool,
                                       const ModIntent *m) {
    if (!sk || !m || !pool) return SAFE_DENY_BADREF;
    if (sk->mod_off) return SAFE_DENY_SWITCH;
    if (sk->freeze) return SAFE_DENY_FROZEN;

    /* 调用补丁：没有图端点，所以**先**于引用检查处理。
     * 只做形状/预算判定 —— 「有没有这个补丁、认不认这个操作」由 apply 阶段查
     * （审查函数拿不到 AI，也就看不到补丁注册表）。 */
    if (m->type == MOD_PATCH_CALL) {
        if (!m->patch[0] || !m->name[0]) return SAFE_DENY_NOPATCH;
        if (m->n_args > 4) return SAFE_DENY_TYPE;
        if (sk->step_patch_calls >= (size_t)sk->max_patch_calls_step)
            return SAFE_DENY_BUDGET_PATCH;
        return SAFE_OK;
    }

    /* 引用合法性：任何一条意图都不许引用空/垃圾符号节点 */
    if (!m->target || m->target->is_garbage) return SAFE_DENY_BADREF;
    if (m->main && m->main->is_garbage) return SAFE_DENY_BADREF;

    switch (m->type) {
    case MOD_ADD_TOKEN:
        if (m->name[0] == 0) return SAFE_DENY_TYPE;
        if (sk->new_token_off) return SAFE_DENY_TYPE;
        if (micro_reserved_new_name(m->name)) return SAFE_DENY_RESERVED;
        if (pool->count >= (size_t)sk->max_tokens_total) return SAFE_DENY_TOTAL;
        if (sk->step_new_tokens >= (size_t)sk->max_new_tokens_step)
            return SAFE_DENY_BUDGET_TOKEN;
        return SAFE_OK;
    case MOD_ADD_ASSOC: {
        /* 另一个端点可以**按名字**给（`::op 加步骤 泡茶 烧水` 的动作词可能还是个新词）：
         * 名字不在池子里 = 这一步要新建一个符号节点 ⇒ 照 INSERT_BETWEEN 的规矩**记预算**，
         * 不许"悄悄建词"绕过记账。 */
        Token *Mn = m->main;        /* 池里已有的端点 */
        int named_new = 0;          /* 端点只给了名字、而且池里还没有 ⇒ 这一步要建它 */
        if (!Mn && m->name[0]) {
            Mn = pool_find(pool, m->name);
            if (!Mn) {
                named_new = 1;
                if (sk->new_token_off) return SAFE_DENY_TYPE;
                if (micro_reserved_new_name(m->name)) return SAFE_DENY_RESERVED;
                if (pool->count >= (size_t)sk->max_tokens_total) return SAFE_DENY_TOTAL;
                if (sk->step_new_tokens >= (size_t)sk->max_new_tokens_step)
                    return SAFE_DENY_BUDGET_TOKEN;
            }
        }
        if (!Mn && !named_new) return SAFE_DENY_BADREF;
        /* 自环：端点已存在就比指针，还不存在就比名字（别让它偷偷建一条自己到自己） */
        if (Mn ? (m->target == Mn)
               : (m->target->name && strcmp(m->target->name, m->name) == 0))
            return SAFE_DENY_SELF;
        if (micro_endpoint_reserved(m->target)) return SAFE_DENY_RESERVED;
        if (Mn && micro_endpoint_reserved(Mn)) return SAFE_DENY_RESERVED;
        if (m->target->count >= (size_t)sk->max_edges_per_token)
            return SAFE_DENY_EDGE_CAP;
        /* 「这条边是不是新的」要按 (main) 判 —— v10 起没有语境可以区分了。 */
        if ((!Mn || !micro_has_edge(m->target, Mn)) &&
            sk->step_new_edges >= (size_t)sk->max_new_edges_step)
            return SAFE_DENY_BUDGET_EDGE;
        return SAFE_OK;
    }
    case MOD_TAG_REL:
        /* 动态关系（清单外谓语）也走这条意图（§6.80）：以前这里直接拒掉 `rel_id >= REL_DYN_BASE`
         * ⇒ `route_writes` 打开时"现场学新谓语"整条被拒（默认关时看不出来）。 */
        if (!m->main || m->rel_id == 0) return SAFE_DENY_TYPE;
        if (m->target == m->main) return SAFE_DENY_SELF;
        if (micro_endpoint_reserved(m->target) || micro_endpoint_reserved(m->main))
            return SAFE_DENY_RESERVED;
        return SAFE_OK;
    case MOD_TAG_DERIVED:
        /* 沉淀比「打关系」多一条：不许把**观测到**的东西标成推出来的
         * （那会把观测降级，正好反了）。已存在且不是缓存的边 → 拒绝。 */
        if (!m->main || m->rel_id == 0 || m->rel_id >= REL_DYN_BASE) return SAFE_DENY_TYPE;
        if (m->target == m->main) return SAFE_DENY_SELF;
        if (micro_endpoint_reserved(m->target) || micro_endpoint_reserved(m->main))
            return SAFE_DENY_RESERVED;
        if (micro_has_edge(m->target, m->main) &&
            !micro_edge_is_derived(m->target, m->main))
            return SAFE_DENY_TYPE;
        if (m->target->count >= (size_t)sk->max_edges_per_token)
            return SAFE_DENY_EDGE_CAP;
        if (!micro_has_edge(m->target, m->main) &&
            sk->step_new_edges >= (size_t)sk->max_new_edges_step)
            return SAFE_DENY_BUDGET_EDGE;
        return SAFE_OK;
    case MOD_BUMP_HITS:
    case MOD_DECAY:
        if (micro_endpoint_reserved(m->target)) return SAFE_DENY_RESERVED;
        return SAFE_OK;
    case MOD_SET_ACTION:
        if (!m->main || !m->action) return SAFE_DENY_BADREF;
        if (micro_endpoint_reserved(m->target)) return SAFE_DENY_RESERVED;
        return SAFE_OK;
    case MOD_SET_PARENT:
        if (!m->main) return SAFE_DENY_BADREF;
        if (m->target == m->main) return SAFE_DENY_SELF;
        if (micro_endpoint_reserved(m->target) || micro_endpoint_reserved(m->main))
            return SAFE_DENY_RESERVED;
        /* 父节点不许接成环（否则类别继承会无限递归） */
        if (token_is_descendant(m->main, m->target)) return SAFE_DENY_CYCLE;
        return SAFE_OK;
    case MOD_SET_CATEGORY:
        if (micro_endpoint_reserved(m->target)) return SAFE_DENY_RESERVED;
        return SAFE_OK;
    case MOD_REMOVE_ASSOC: {
        if (sk->remove_off) return SAFE_DENY_TYPE;
        if (!m->main) return SAFE_DENY_BADREF;
        if (micro_endpoint_reserved(m->target) || micro_endpoint_reserved(m->main))
            return SAFE_DENY_RESERVED;
        long i = micro_edge_index(m->target, m->main);
        if (i < 0) return SAFE_DENY_BADREF;                 /* 本来就没有，不算「删」 */
        /* **知识不是共现**：观测到的关系边（人工写的 relations.txt 或对话里学的）
         * 不许被自修改剪掉。这条是硬底线，不是参数。
         * **例外**：推出来沉淀的（ASSOC_ROLE_DERIVED）是缓存，
         * 它本来就可能过时 —— 被推翻时允许撤销（见 docs/thinking.md）。
         * 判据只有一份：micro_edge_removable()。 */
        if (!micro_edge_removable(&token_assocs(m->target)[i])) return SAFE_DENY_RELATION;
        return SAFE_OK;
    }
    /* **在 A 与 B 之间插一个符号节点**：A→N→B，且（可选）让 A、B 不直连。
     * 它是一次复合改动，所以按「新符号节点 + 两条新边」一起算预算。 */
    case MOD_INSERT_BETWEEN: {
        if (!m->main) return SAFE_DENY_BADREF;
        if (m->target == m->main) return SAFE_DENY_SELF;
        if (micro_endpoint_reserved(m->target) || micro_endpoint_reserved(m->main))
            return SAFE_DENY_RESERVED;
        if (m->name[0] == 0) return SAFE_DENY_TYPE;
        if (micro_reserved_new_name(m->name)) return SAFE_DENY_RESERVED;
        Token *N = pool_find(pool, m->name);
        if (!N) {
            if (sk->new_token_off) return SAFE_DENY_TYPE;
            if (pool->count >= (size_t)sk->max_tokens_total) return SAFE_DENY_TOTAL;
            if (sk->step_new_tokens >= (size_t)sk->max_new_tokens_step)
                return SAFE_DENY_BUDGET_TOKEN;
        }
        if (m->target->count >= (size_t)sk->max_edges_per_token) return SAFE_DENY_EDGE_CAP;
        if (N && N->count >= (size_t)sk->max_edges_per_token) return SAFE_DENY_EDGE_CAP;
        size_t ne = 0;
        if (!N) ne = 2;                                  /* A→N 与 N→B 都是新的 */
        else {
            if (!micro_has_edge(m->target, N)) ne++;
            if (!micro_has_edge(N, m->main)) ne++;
        }
        if (sk->step_new_edges + ne > (size_t)sk->max_new_edges_step)
            return SAFE_DENY_BUDGET_EDGE;
        return SAFE_OK;
    }
    /* 回收**临时草稿**：只许 [临: 命名空间，而且必须已经没人引用它。 */
    case MOD_REMOVE_TOKEN: {
        if (sk->remove_off) return SAFE_DENY_TYPE;
        if (!token_is_temp(m->target)) return SAFE_DENY_NOT_TEMP;
        if (m->target->count != 0) return SAFE_DENY_TYPE;          /* 自己还连着谁 */
        if (micro_token_has_in_edge(pool, m->target)) return SAFE_DENY_TYPE;
        return SAFE_OK;
    }
    default:
        return SAFE_DENY_TYPE;
    }
}

/* 记账。**必须在 micro_apply_one() 之前调用**（所以要拿到 pool 才能判「新的吗」）。
 * 踩过的坑：原来放在应用**之后**，而 MOD_ADD_ASSOC 靠 `!micro_has_edge()` 判断
 * 「这条边是不是新的」—— 应用完那条边已经在图上，判断永远是假，
 * 于是 step_new_edges 一直是 0、**每步新建边的预算从来没被扣过**
 * （安全内核的预算形同虚设）。应用失败时用 safety_refund() 退回去。 */
static inline void safety_charge(SafetyKernel *sk, const TokenPool *pool, const ModIntent *m) {
    if (!sk || !m) return;
    sk->n_allowed++;
    if ((unsigned)m->type < 16u) sk->n_applied_by_type[m->type]++;
    switch (m->type) {
    case MOD_ADD_TOKEN: sk->step_new_tokens++; break;
    case MOD_ADD_ASSOC: {
        Token *Mn = m->main;
        if (!Mn && m->name[0]) {
            Mn = pool_find(pool, m->name);
            if (!Mn) sk->step_new_tokens++;        /* 名字端点：这一步确实要建它 */
        }
        if (!Mn || !micro_has_edge(m->target, Mn))
            sk->step_new_edges++;
        break;
    };
    case MOD_TAG_DERIVED:      /* 沉淀也是「新建一条边」，照占预算 */
        if (!micro_has_edge(m->target, m->main)) sk->step_new_edges++;
        break;
    case MOD_PATCH_CALL: sk->step_patch_calls++; break;
    case MOD_INSERT_BETWEEN: {
        Token *N = pool_find(pool, m->name);
        if (!N) sk->step_new_tokens++;
        if (!N || !micro_has_edge(m->target, N)) sk->step_new_edges++;
        if (!N || !micro_has_edge(N, m->main))    sk->step_new_edges++;
        break;
    }
    default: break;
    }
}
/* 把 safety_charge 记下的账退回去（应用失败时 —— 被**拒**的意图从不记账）。 */
static inline void safety_refund(SafetyKernel *sk, const TokenPool *pool, const ModIntent *m) {
    if (!sk || !m) return;
    if (sk->n_allowed) sk->n_allowed--;
    if ((unsigned)m->type < 16u && sk->n_applied_by_type[m->type])
        sk->n_applied_by_type[m->type]--;
    switch (m->type) {
    case MOD_ADD_TOKEN:
        if (sk->step_new_tokens) sk->step_new_tokens--;
        break;
    case MOD_ADD_ASSOC:
    case MOD_TAG_DERIVED:
        if (sk->step_new_edges) sk->step_new_edges--;
        break;
    case MOD_PATCH_CALL:
        if (sk->step_patch_calls) sk->step_patch_calls--;
        break;
    case MOD_INSERT_BETWEEN: {
        Token *N = pool_find(pool, m->name);
        if (!N && sk->step_new_tokens) sk->step_new_tokens--;
        if (sk->step_new_edges >= 2) sk->step_new_edges -= 2;
        else sk->step_new_edges = 0;
        break;
    }
    default: break;
    }
}

static inline void safety_audit(SafetyKernel *sk, uint64_t tick, const ModIntent *m,
                                int allowed, SafeReason r) {
    if (!sk) return;
    size_t i = sk->audit_n % MICRO_AUDIT_MAX;
    SafetyAudit *a = &sk->audit[i];
    a->tick = tick;
    snprintf(a->op, sizeof a->op, "%s", m ? micro_mod_type_name(m->type) : "?");
    if (m && m->type == MOD_ADD_TOKEN) {
        /* 名字现在可以到 96 字节（[自问:<问句>]），审计栏只有 64 → **有界拷贝**，
         * 截断是故意的（审计是给人看的，不是权威副本）。 */
        size_t off = 0;
        while (m->name[off] && off + 1 < sizeof a->target) { a->target[off] = m->name[off]; off++; }
        a->target[off] = 0;
    }
    else if (m && m->target)
        snprintf(a->target, sizeof a->target, "%s→%s",
                 m->target->name ? m->target->name : "?",
                 m->main && m->main->name ? m->main->name : "-");
    else
        a->target[0] = 0;
    a->allowed = allowed;
    snprintf(a->reason, sizeof a->reason, "%s", SAFE_REASON_NAME[r]);
    sk->audit_n++;
}

/* 停止条件：池子总量触顶（长期失控）就冻结。
 * 注意阈值设得比正常检测器的上限高得多 —— 正常负载永远不会碰到它，
 * 它的作用是拦住「补丁狂推意图」这种失控。 */
static inline int safety_check_stop(SafetyKernel *sk, const TokenPool *pool) {
    if (!sk || !pool) return 0;
    if (!sk->freeze && pool->count >= (size_t)sk->max_tokens_total) {
        sk->freeze = 1; sk->n_freeze++;
        return 1;
    }
    return 0;
}

/* ============================ 补丁边界 ============================ */

typedef enum { PATCH_SENSE = 1, PATCH_ACT = 2, PATCH_BOTH = 3 } PatchKind;

/* 补丁交给内核的一条信号。neg = 「这条是**否定**信号」（补丁自己带极性，
 * 内核不解释语义，只负责把它们变成符号节点放进同一轮里）。 */
typedef struct {
    Token *tok;
    int    neg;
    float  w;
} PatchOut;

typedef struct Patch Patch;
struct Patch {
    const char *name;
    PatchKind   kind;
    int         enabled;        /* 0 = 挂载着但停用 */
    int         mounted;
    uint64_t    mount_tick;

    /* ---- 生命周期（都是可选的，NULL = 不做这件事）---- */
    int    (*init)(Patch *p, TokenPool *pool, void *cfg);
    size_t (*sense)(Patch *p, TokenPool *pool, PatchOut *out, size_t cap);
    void   (*act)(Patch *p, TokenPool *pool, Token *const *in, size_t n);
    void   (*update)(Patch *p, TokenPool *pool, float dt);
    int    (*propose)(Patch *p, void *mq);   /* void* 是 ModQueue*：接口不反向依赖 mod */
    void   (*destroy)(Patch *p);

    /* ---- 输出侧的两个口子（外设不只是感官，也有"发声"的一侧）----
     * 内核自己不会写字，它只把这一轮**本来要说的话**交给补丁暂存，然后再问
     * "这一轮到底发什么"。没有补丁/补丁不接管 → 保持流水线文本（内核不依赖补丁）。 */
    /* 暂存：把这一轮流水线本来要输出的原文交给补丁（补丁想改就改自己的副本）。 */
    void   (*stage)(Patch *p, const char *text);
    /* 取输出：1 = 用我写的 out；0 = 我不接管；-1 = **明确这一轮不说话**。 */
    int    (*emit)(Patch *p, char *out, size_t cap);
    /* 补丁自己的操作入口：内核**不解释** op 的语义，只负责有界派发 + 故障隔离。
     * 返回 ≥0 成功、<0 出错；out 里可以放一行给用户看的说明。 */
    int    (*op)(Patch *p, const char *op, char *const *args, size_t n,
                 char *out, size_t cap);

    /* ---- 统计（元认知看这个）---- */
    size_t   n_sense, n_act, n_update, n_propose, n_err;
    size_t   n_stage, n_emit, n_op, n_op_err;
    size_t   n_out_total, n_out_last;
    int      err_streak;
    uint64_t last_out_tick;
    char     err[64];
    void    *priv;
};

typedef struct {
    Patch *items[MICRO_PATCH_MAX];
    size_t n;
} PatchRegistry;

static inline Patch *patch_find(const PatchRegistry *r, const char *name) {
    if (!r || !name) return NULL;
    for (size_t i = 0; i < r->n; ++i)
        if (r->items[i] && r->items[i]->name && strcmp(r->items[i]->name, name) == 0)
            return r->items[i];
    return NULL;
}

/* 挂载：注册 + init。同名只允许一个（换补丁要显式卸）。
 * init 失败不算灾难 —— 补丁没挂上，内核照跑。 */
static inline int patch_register(PatchRegistry *r, Patch *p, TokenPool *pool, void *cfg) {
    if (!r || !p || !p->name) return -1;
    if (r->n >= MICRO_PATCH_MAX) return -1;
    if (patch_find(r, p->name)) return -1;
    p->mounted = 1;
    p->enabled = 1;
    p->err_streak = 0;
    p->err[0] = 0;
    p->mount_tick = pool ? pool->current_tick : 0;
    p->last_out_tick = p->mount_tick;
    if (p->init && p->init(p, pool, cfg) != 0) {
        p->mounted = 0;
        return -2;
    }
    r->items[r->n++] = p;
    return 0;
}

/* 卸载：从表里摘掉 + destroy（腾资源）。注意**表是定长数组**，
 * 摘除要左移，不然 patch_find 会在空洞里乱走。 */
static inline int patch_unmount(PatchRegistry *r, const char *name) {
    if (!r || !name) return -1;
    for (size_t i = 0; i < r->n; ++i) {
        Patch *p = r->items[i];
        if (!p || !p->name || strcmp(p->name, name) != 0) continue;
        if (p->destroy) p->destroy(p);
        p->mounted = 0;
        for (size_t j = i + 1; j < r->n; ++j) r->items[j - 1] = r->items[j];
        r->n--;
        return 0;
    }
    return -1;
}

/* 故障隔离：补丁出错**只影响它自己**。
 * 连续 3 次出错就自动停用并留下提醒 —— 内核不会因为一个坏外设停摆。 */
static inline void patch_note_error(Patch *p, const char *what) {
    if (!p) return;
    p->n_err++;
    p->err_streak++;
    {
        /* `what` 里可能带 96 字节的名字（[自问:…] 这类），err 只有 64 —— 有界拷贝 */
        const char *q = what ? what : "?";
        size_t k = 0;
        while (q[k] && k + 1 < sizeof p->err) { p->err[k] = q[k]; k++; }
        p->err[k] = 0;
    }
    if (p->err_streak >= 3) p->enabled = 0;
}
static inline void patch_note_ok(Patch *p) {
    if (!p) return;
    p->err_streak = 0;
    p->err[0] = 0;
}

/* ============================ 微内核状态 ============================ */

/* D4 的有界表（命名类型：匿名结构体即使成员一样也**不是**同一个类型）。
 * 一行的两个计数：`n_pair` = A、B 同轮的次数（**分母**，存在本行里），
 * `n_triple` = 其中 P 也在场的次数（分子）。 */
typedef struct { Token *a, *b, *p; uint32_t n_pair, n_triple; uint8_t fired; } CondTriple;

typedef struct {
    size_t n_steps;            /* 走过多少次安全点 */
    size_t n_proposed, n_applied, n_denied, n_dedup, n_dropped;
    size_t n_new_tokens, n_new_edges, n_removed, n_bumped, n_rel, n_derived;
    size_t win[MICRO_WINDOW];  /* 最近每步应用了几条（看速率） */
    int    patch_auto_tried;   /* AIP_PATCH 这个测量钩子只试一次 */
    size_t win_i, win_filled, win_applied;
    size_t denied_window;
    float  last_rate;
    size_t n_throttles;        /* 元认知关小过几次 */
    /* ---- D4 条件依赖的会话内统计（瞬态：不进存档，装载后重新计数）---- */
    CondTriple cond_tri[MICRO_D4_MAX_TRIPLES];
    size_t cond_tri_n;
    size_t n_cond_proposed, n_cond_applied;
    int    cond_off;           /* D4 开关（默认值写在 micro_of 里，不靠清零） */
    char   apply_log[MICRO_APPLY_LOG_MAX][192];
    size_t apply_n;
    size_t remind_at[MICRO_REMIND_N];
    size_t n_reminders[MICRO_REMIND_N];
} MicroMeta;

typedef struct {
    SafetyKernel  safe;
    ModQueue      queue;
    PatchRegistry patches;
    MicroMeta     meta;
    /* 检测器开关（反向命名：calloc = 默认开） */
    int coact_off;      /* 共现固化 */
    int gap_off;        /* 缺口命名 */
    int prune_off;      /* 休眠剪枝 */
    int coact_off_by_meta;  /* 1 = 当前这次「关小」是元认知干的（要能恢复） */
    int propose_off;    /* >0 = 元认知临时关小提案，倒数若干步 */
} Micro;

static inline Micro *micro_of(AI *ai) {
    if (!ai) return NULL;
    if (!ai->micro) {
        Micro *mk = (Micro *)calloc(1, sizeof *mk);
        if (!mk) return NULL;
        /* 硬预算的默认值。刻意写在这里（calloc 之后），
         * 免得将来有人以为结构体清零就等于默认值还在。 */
        mk->safe.max_new_tokens_step = 2;
        mk->safe.max_new_edges_step  = 8;
        mk->safe.max_edges_per_token = 512;
        /* §6.175 **测量钩子**（只当初始默认，`AIP_EDGECAP=<n>` 覆盖；默认仍 512）。
         * 为什么要它：会话图聊厚之后 `我` 的边数会越过 512（实测 674），
         * 于是往 `我` 上写的自撰（想象/内部对话/自我提醒）全被安全内核拒。
         * 这是**安全阀**不是 bug，但"上限该不该随图长"要先有数据再谈。 */
        { const char *ec = getenv("AIP_EDGECAP");
          if (ec && ec[0]) { long v = strtol(ec, NULL, 10); if (v > 0) mk->safe.max_edges_per_token = (int)v; } }
        mk->safe.max_patch_calls_step = 4;
        mk->safe.max_tokens_total    = 400000;
        /* mod_off / new_token_off / remove_off = 0：自修改默认开 */
        /* 但**内建的休眠剪枝检测器默认关**：删错东西比不删更糟。
         * 安全内核那边「删边」是允许的（补丁可以提议删，照样要过审查），
         * 这里关的是「内核自己主动去剪」这条来源。::mod prune on 打开。 */
        mk->prune_off = 1;
        /* D4 条件依赖**默认关**：它会在对话里**改结构**（建前提链接、拆直连），
         * 和"删错比不删更糟"同理 —— 先实现、先测量（AIP_D4=1 跑整包），
         * 再谈要不要翻默认。`:mod cond on` 手动打开。 */
        mk->meta.cond_off = 1;
        {   /* 测量钩子只当**初始默认**：`AIP_D4=1` 打开。
             * 放在这里（而不是每步里）是为了让显式的 `::mod cond off` 一定能盖过它。 */
            const char *d4 = getenv("AIP_D4");
            if (d4 && d4[0] == '1') mk->meta.cond_off = 0;
            else if (d4 && d4[0] == '0') mk->meta.cond_off = 1;
        }
        ai->micro = mk;
    }
    return (Micro *)ai->micro;
}

/* ---- 元认知提醒：既写进图（我→[提醒:…]），也记进日志 ----
 * 同一种提醒有冷却，免得刷屏把图塞满。 */
static inline void micro_remind(AI *ai, MicroRemind kind, const char *who) {
    Micro *mk = micro_of(ai);
    if (!mk || kind < 0 || kind >= MICRO_REMIND_N) return;
    size_t step = mk->meta.n_steps;
    if (mk->meta.n_reminders[kind] > 0 &&
        step < mk->meta.remind_at[kind] + MICRO_REMIND_COOLDOWN) return;
    mk->meta.remind_at[kind] = step;
    mk->meta.n_reminders[kind]++;
    char nm[96];
    if (who && who[0])
        snprintf(nm, sizeof nm, MICRO_REMIND_TOKEN_PREFIX "%s:%s]", MICRO_REMIND_NAME[kind], who);
    else
        snprintf(nm, sizeof nm, MICRO_REMIND_TOKEN_PREFIX "%s]", MICRO_REMIND_NAME[kind]);
    /* 用 intern 而不是 pool_token：pool_token 是「创建」（允许同名重复），
     * 提醒要是每次冷却到点就多一个同名节点，图会被刷烂。 */
    Token *t = pool_token_intern(ai->pool, nm);
    if (t && ai->self) {
        /* §6.143 `remind_route`（默认**关**）：开着时这条**自撰写入**走统一通道
         * （受审 + 记进自撰账 + 进审计），与想象/内部对话（§6.77）同一条路。
         * 关着时**一个字都不变**：还是直写（今天的行为）。 */
        if (ai->remind_route) (void)ai_write_assoc(ai, ai->self, t, AI_WSRC_INNER);
        else                  token_add_assoc_unique(ai->self, t);
        token_touch(t);
    }
}

/* 解冻 / 复位（::safe reset）。放在 micro_of 之后 —— 它要用到 Micro。 */
static inline void safety_reset(AI *ai) {
    Micro *mk = micro_of(ai);
    if (!mk) return;
    mk->safe.freeze = 0;
    mk->safe.n_freeze = 0;
    mk->safe.step_new_tokens = mk->safe.step_new_edges = 0;
    mk->safe.step_patch_calls = 0;
    mk->propose_off = 0;
    if (mk->coact_off_by_meta) { mk->coact_off = 0; mk->coact_off_by_meta = 0; }
}

/* 安全内核怎么调（::safe 里逐项打出来） */
static inline void safety_set(AI *ai, const char *what, int on) {
    Micro *mk = micro_of(ai);
    if (!mk || !what) return;
    SafetyKernel *sk = &mk->safe;
    /* 反向命名：on 表示「允许」，off 表示「禁止」 */
    if (!strcmp(what, "mod"))           sk->mod_off = on ? 0 : 1;
    else if (!strcmp(what, "newtoken")) sk->new_token_off = on ? 0 : 1;
    else if (!strcmp(what, "remove"))   sk->remove_off = on ? 0 : 1;
    else if (!strcmp(what, "freeze"))   sk->freeze = on ? 1 : 0;
    else if (!strcmp(what, "reset"))    safety_reset(ai);
}

/* ============================ 补丁：内建 + 收集 ============================ */

/* ---- 内建补丁：自省 ----
 * 把**内核自己的状态**变成符号节点。它证明了补丁边界能用，
 * 而且不依赖任何外部设备（没有摄像头、没有麦克风、没有 3D）。
 * 内核完全不知道「自省」是什么 —— 它只是收到了一批符号节点。 */
static inline size_t patch_introspect_sense(Patch *p, TokenPool *pool,
                                            PatchOut *out, size_t cap) {
    AI *ai = (AI *)p->priv;
    if (!ai || !pool || !out || cap == 0) return 0;
    size_t n = 0;
    /* 宏参数别叫 tok —— 它会替换掉 out[n].tok 里的成员名（踩过） */
    #define EMIT(T) do { if (n < cap) { out[n].tok = (T); out[n].neg = 0; out[n].w = 1.0f; n++; } } while (0)
    if (ai->inner.fatigue > 0.60f) EMIT(pool_token_intern(pool, "[自省:疲劳高]"));
    if (ai->last_confidence < 0.35f) EMIT(pool_token_intern(pool, "[自省:把握低]"));
    if (ai->meta.total < 0.40f) EMIT(pool_token_intern(pool, "[自省:知道感低]"));
    if (ai->others_n == 0 && ai->turns > 8) EMIT(pool_token_intern(pool, "[自省:没有他者]"));
    #undef EMIT
    return n;
}

/* ---- 收集所有感知补丁的输出 ----
 * 返回写进 out 的条数。补丁返回 -1（出错）会被记账；连续出错自动停用。
 * 出错的补丁这一步**当作没有输出**，不影响别的补丁。 */
static inline size_t patch_sense_all(AI *ai, PatchOut *out, size_t cap) {
    Micro *mk = micro_of(ai);
    if (!mk || !out || cap == 0) return 0;
    PatchRegistry *r = &mk->patches;
    size_t n = 0;
    for (size_t i = 0; i < r->n; ++i) {
        Patch *p = r->items[i];
        if (!p || !p->mounted || !p->enabled) continue;
        if (!(p->kind & PATCH_SENSE) || !p->sense) continue;
        p->n_sense++;
        size_t got = p->sense(p, ai->pool, out + n, cap - n);
        if (got == (size_t)-1) {
            patch_note_error(p, "sense 返回错误");
            if (!p->enabled) micro_remind(ai, MICRO_REMIND_PATCH_ERR, p->name);
            continue;
        }
        if (got > cap - n) got = cap - n;   /* 补丁越界写：按容量截断，不信任它 */
        patch_note_ok(p);
        p->n_out_last = got;
        p->n_out_total += got;
        if (got) p->last_out_tick = ai->pool->current_tick;
        /* 冲突检测：两个补丁对同一个符号节点一个说「有」一个说「没有」 */
        for (size_t a = 0; a < got; ++a) {
            if (!out[n + a].tok) continue;
            for (size_t b = 0; b < n; ++b) {
                if (out[b].tok == out[n + a].tok && out[b].neg != out[n + a].neg) {
                    micro_remind(ai, MICRO_REMIND_PATCH_CONFLICT, p->name);
                    break;
                }
            }
        }
        n += got;
    }
    return n;
}

static inline void patch_act_all(AI *ai, Token *const *in, size_t n_in) {
    Micro *mk = micro_of(ai);
    if (!mk) return;
    PatchRegistry *r = &mk->patches;
    for (size_t i = 0; i < r->n; ++i) {
        Patch *p = r->items[i];
        if (!p || !p->mounted || !p->enabled) continue;
        if (!(p->kind & PATCH_ACT) || !p->act) continue;
        p->n_act++;
        p->act(p, ai->pool, in, n_in);
    }
}

static inline void patch_update_all(AI *ai, float dt) {
    Micro *mk = micro_of(ai);
    if (!mk) return;
    PatchRegistry *r = &mk->patches;
    for (size_t i = 0; i < r->n; ++i) {
        Patch *p = r->items[i];
        if (!p || !p->mounted || !p->enabled || !p->update) continue;
        p->n_update++;
        p->update(p, ai->pool, dt);
    }
}

/* 补丁提议结构修改：走的是同一条意图队列，安全内核照样审。
 * 补丁**不能**直接改结构 —— 这是微内核的边界。 */
static inline size_t patch_propose_all(AI *ai, ModQueue *q) {
    Micro *mk = micro_of(ai);
    if (!mk || !q) return 0;
    PatchRegistry *r = &mk->patches;
    size_t n = 0;
    for (size_t i = 0; i < r->n; ++i) {
        Patch *p = r->items[i];
        if (!p || !p->mounted || !p->enabled || !p->propose) continue;
        p->n_propose++;
        int got = p->propose(p, q);
        if (got < 0) patch_note_error(p, "propose 返回错误");
        else if (got > 0) n += (size_t)got;
    }
    return n;
}

/* ---- 输出侧：暂存 / 取输出 / 调用补丁操作 ----
 *
 * 三条都是**有界**的：注册表最多 MICRO_PATCH_MAX 个，参数最多 4 个、每个 40 字节。
 * 补丁出错只记在它自己头上（连续 3 次自动停用），内核照跑。 */

static inline void patch_stage_all(AI *ai, const char *text) {
    if (!ai || !text) return;
    Micro *mk = micro_of(ai);
    if (!mk || mk->patches.n == 0) return;
    for (size_t i = 0; i < mk->patches.n; ++i) {
        Patch *p = mk->patches.items[i];
        if (!p || !p->enabled || !p->mounted || !p->stage) continue;
        p->stage(p, text);
        p->n_stage++;
    }
}

/* 问补丁："这一轮发什么？" 按注册顺序问，第一个接管的赢。
 * 返回 1（用补丁文本）/ 0（谁也不接管，内核用流水线文本）/ -1（明确沉默）。 */
static inline int patch_emit_all(AI *ai, char *out, size_t cap) {
    if (!ai || !out || cap == 0) return 0;
    out[0] = 0;
    Micro *mk = micro_of(ai);
    if (!mk || mk->patches.n == 0) return 0;
    for (size_t i = 0; i < mk->patches.n; ++i) {
        Patch *p = mk->patches.items[i];
        if (!p || !p->enabled || !p->mounted || !p->emit) continue;
        int r = p->emit(p, out, cap);
        p->n_emit++;
        if (r == 0) continue;               /* 不接管，问下一个 */
        patch_note_ok(p);
        return r;                            /* 1 = 用它；-1 = 明确不说话 */
    }
    return 0;
}

/* 调用一个补丁的操作（内核不解释语义）。
 * 返回 ≥0 = 补丁自己的返回值；-1 = 没这个补丁/没挂/停用/不认操作。 */
static inline int patch_op_call_by_name(AI *ai, const char *patch, const char *op,
                                        char *const *args, size_t n,
                                        char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!ai || !patch || !op) return -1;
    Micro *mk = micro_of(ai);
    if (!mk) return -1;
    Patch *p = patch_find(&mk->patches, patch);
    if (!p || !p->mounted || !p->enabled || !p->op) return -1;
    int r = p->op(p, op, args, n, out, cap);
    p->n_op++;
    if (r < 0) { p->n_op_err++; patch_note_error(p, op); }
    else       patch_note_ok(p);
    return r;
}

/* ============================ 内建补丁：记事本（输出侧外设） ============================
 *
 * 用户要的两件事（作为**补丁**，不是塞进流水线）：
 *   ① 回答的输出是 **AI 可以编辑的文本**（像记事本）：它能对自己要说的话做
 *      插入/删除/替换，还能控制**什么时候发送**；
 *   ② 一个**草稿区**：先打草稿、改满意了再提交、再发。
 *
 * 两个缓冲：
 *   `草稿`   —— AI 随便写（`写 / 插入 / 删 / 改 / 清空`），不影响发言；
 *   `待发送` —— 这一轮真的要说的话（`插入发言 / 删发言 / 改发言` 改它）；
 *               `提交` 把草稿搬进来，`发送` / `不发` 决定这一轮发不发。
 *
 * 两种模式：
 *   `mode=auto`（默认）—— 流水线本来要说的文本**原样**进待发送并发送：
 *                        没编辑时**逐字节等于**没有补丁时的输出（挂上它也不改行为）；
 *   `mode=manual`      —— 流水线文本先落进**草稿**，AI 不显式 `发送` 就**不说话**
 *                        （"控制什么时候发送"；默认不开，要 A/B 才谈翻默认）。
 *
 * 内核侧只认三个口子（stage / emit / op），**完全不知道"记事本"是什么**。 */
#define PATCH_NOTE_CAP 512

typedef struct {
    char   draft[PATCH_NOTE_CAP];     /* 草稿区 */
    char   outbox[PATCH_NOTE_CAP];    /* 待发送 */
    int    manual;                    /* 0 = auto（默认，行为与无补丁一致） */
    int    send_flag;                 /* 本轮发送过吗（emit 用） */
    int    decided;                   /* 本轮 AI **明确表过态**（发送/不发）：流水线不许再改主意 */
    /* **草稿持久化**（不改存档格式）：`存草稿` 把草稿写成一个 `[草稿:…]` **标记节点**
     * （普通符号节点，随图存盘），装载/挂载时扫回来。和未解问题用 `[自问:…]` 恢复同一个路子。 */
    void  *ai;
    char   save_pending[PATCH_NOTE_CAP];
    int    save_wanted;
    int    restored;
    size_t n_edit, n_sent;
} NoteState;

static NoteState g_note_state;
static Patch     g_patch_note;

static inline size_t note_append(char *buf, size_t cap, const char *txt) {
    size_t l = buf[0] ? strlen(buf) : 0;
    size_t add = txt ? strlen(txt) : 0;
    if (l + add + 1 > cap) add = (cap > l + 1) ? cap - l - 1 : 0;
    if (add) memcpy(buf + l, txt, add);
    buf[l + add] = 0;
    return add;
}
static inline void note_replace(char *buf, size_t cap, const char *old, const char *neu) {
    if (!old || !old[0]) return;
    char *hit = strstr(buf, old);
    if (!hit) return;
    size_t head = (size_t)(hit - buf);
    size_t ol = strlen(old), nl = neu ? strlen(neu) : 0;
    char tail[PATCH_NOTE_CAP];
    snprintf(tail, sizeof tail, "%s", hit + ol);
    if (head + nl + strlen(tail) + 1 > cap) return;      /* 放不下就整条不动（宁可不改） */
    memcpy(buf + head, neu ? neu : "", nl);
    snprintf(buf + head + nl, cap - head - nl, "%s", tail);
}
static inline void note_insert(char *buf, size_t cap, size_t pos, const char *txt) {
    size_t l = strlen(buf);
    if (pos > l) pos = l;
    if (!txt) return;
    size_t add = strlen(txt);
    if (l + add + 1 > cap) add = (cap > l + 1) ? cap - l - 1 : 0;
    if (!add) return;
    memmove(buf + pos + add, buf + pos, l - pos + 1);
    memcpy(buf + pos, txt, add);
}
/* 位置参数按**字符**（UTF-8 码点）算，不按字节：中文一个字 3 字节，
 * 按字节删会把一个汉字劈成两半（实测踩过）。返回第 idx 个字符的字节偏移。 */
static inline size_t note_utf8_off(const char *buf, long idx) {
    size_t b = 0;
    long c = 0;
    if (!buf || idx <= 0) return 0;
    while (buf[b] && c < idx) {
        unsigned char ch = (unsigned char)buf[b];
        size_t step = (ch < 0x80) ? 1
                    : ((ch & 0xE0) == 0xC0) ? 2
                    : ((ch & 0xF0) == 0xE0) ? 3
                    : ((ch & 0xF8) == 0xF0) ? 4 : 1;
        b += step;
        c++;
    }
    return b;
}

static inline void note_delete(char *buf, size_t pos, size_t len) {
    size_t l = strlen(buf);
    if (pos >= l) return;
    if (pos + len > l) len = l - pos;
    memmove(buf + pos, buf + pos + len, l - pos - len + 1);
}

static inline int patch_note_op(Patch *p, const char *op, char *const *args, size_t n,
                                char *out, size_t cap) {
    NoteState *st = (NoteState *)p->priv;
    if (!st || !op) return -1;
    st->n_edit++;
    char txt[PATCH_NOTE_CAP];                 /* 参数拼成一段文本（`写 你 好` = 写「你 好」） */
    txt[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i) note_append(txt, sizeof txt, " ");
        note_append(txt, sizeof txt, args[i]);
    }
    if (!strcmp(op, "写")) {
        note_append(st->draft, sizeof st->draft, txt);
        if (out && cap) snprintf(out, cap, "草稿 %zu 字节", strlen(st->draft));
        return 0;
    }
    if (!strcmp(op, "插入")) {
        long pos = (n >= 1) ? strtol(args[0], NULL, 10) : 0;
        char t2[PATCH_NOTE_CAP];
        t2[0] = 0;
        for (size_t i = 1; i < n; ++i) {
            if (i > 1) note_append(t2, sizeof t2, " ");
            note_append(t2, sizeof t2, args[i]);
        }
        note_insert(st->draft, sizeof st->draft, note_utf8_off(st->draft, pos), t2);
        if (out && cap) snprintf(out, cap, "草稿 %zu 字节", strlen(st->draft));
        return 0;
    }
    if (!strcmp(op, "删")) {
        long pos = (n >= 1) ? strtol(args[0], NULL, 10) : 0;
        long len = (n >= 2) ? strtol(args[1], NULL, 10) : 1;
        /* §6.126（外部审计 P2-3）：**钳住位置与长度**。原来 `pos + len` 会**有符号溢出**
         * （pos=LONG_MAX、len=1 ⇒ 负数）⇒ `note_utf8_off` 拿到负数 ⇒ o2 < o1
         * ⇒ `o2 - o1` 作为 size_t 变成巨数 ⇒ `memmove` 写坏 NoteState 后面的字段
         * （draft/outbox 同一个结构体）。三处钳制：pos ≥ 0、len ≥ 0、两者都不超过容量。 */
        if (pos < 0) pos = 0;
        if (len < 0) len = 0;
        {
            long capc = (long)sizeof st->draft;
            if (pos > capc) pos = capc;
            if (len > capc - pos) len = capc - pos;
            size_t o1 = note_utf8_off(st->draft, pos);
            size_t o2 = note_utf8_off(st->draft, pos + len);
            if (o2 < o1) o2 = o1;
            note_delete(st->draft, o1, o2 - o1);
        }
        if (out && cap) snprintf(out, cap, "草稿 %zu 字节", strlen(st->draft));
        return 0;
    }
    if (!strcmp(op, "改")) {
        if (n < 2) return -1;
        note_replace(st->draft, sizeof st->draft, args[0], args[1]);
        if (out && cap) snprintf(out, cap, "草稿 %zu 字节", strlen(st->draft));
        return 0;
    }
    if (!strcmp(op, "清空"))     { st->draft[0] = 0;  if (out && cap) snprintf(out, cap, "草稿已清空"); return 0; }
    if (!strcmp(op, "清空发言")) { st->outbox[0] = 0; if (out && cap) snprintf(out, cap, "待发送已清空"); return 0; }
    if (!strcmp(op, "提交")) {
        snprintf(st->outbox, sizeof st->outbox, "%s", st->draft);
        st->draft[0] = 0;
        if (out && cap) snprintf(out, cap, "草稿已提交为待发送（%zu 字节）", strlen(st->outbox));
        return 0;
    }
    if (!strcmp(op, "插入发言") || !strcmp(op, "删发言") || !strcmp(op, "改发言")) {
        if (!strcmp(op, "插入发言")) {
            long pos = (n >= 1) ? strtol(args[0], NULL, 10) : 0;
            char t2[PATCH_NOTE_CAP];
            t2[0] = 0;
            for (size_t i = 1; i < n; ++i) {
                if (i > 1) note_append(t2, sizeof t2, " ");
                note_append(t2, sizeof t2, args[i]);
            }
            note_insert(st->outbox, sizeof st->outbox, note_utf8_off(st->outbox, pos), t2);
        } else if (!strcmp(op, "删发言")) {
            long pos = (n >= 1) ? strtol(args[0], NULL, 10) : 0;
            long len = (n >= 2) ? strtol(args[1], NULL, 10) : 1;
            {
                size_t o1 = note_utf8_off(st->outbox, pos);
                size_t o2 = note_utf8_off(st->outbox, pos + (len < 0 ? 0 : len));
                note_delete(st->outbox, o1, o2 - o1);
            }
        } else {
            if (n < 2) return -1;
            note_replace(st->outbox, sizeof st->outbox, args[0], args[1]);
        }
        if (out && cap) snprintf(out, cap, "待发送 %zu 字节", strlen(st->outbox));
        return 0;
    }
    if (!strcmp(op, "发送")) {
        /* **AI 自己写的优先**：草稿非空就把它顶成待发送 ——
         * 否则流水线早先 stage 进来的那句会把它盖掉（实测踩过：写了的结论发不出去）。 */
        if (st->draft[0]) snprintf(st->outbox, sizeof st->outbox, "%s", st->draft);
        st->decided = 1;                          /* 明确表态：这一轮的话由我说了算 */
        st->send_flag = 1;
        st->n_sent++;
        /* 把**正文**也带出来（有界）：核心层（`MOD_PATCH_CALL` 的 applier）要靠它
         * 存 `[草稿:…]` 标记节点 —— §6.94 定位出的正确落点（补丁层交文本、核心层存节点）。 */
        if (out && cap) snprintf(out, cap, "这一轮发送（%zu 字节）：%.*s",
                                 strlen(st->outbox), 60, st->outbox);
        return 0;
    }
    if (!strcmp(op, "不发")) {
        st->decided = 1;                          /* 明确表态：这一轮不说 */
        st->send_flag = 0;
        if (out && cap) snprintf(out, cap, "这一轮不说话");
        return 0;
    }
    if (!strcmp(op, "模式")) {
        if (n >= 1 && !strcmp(args[0], "manual")) st->manual = 1;
        else if (n >= 1 && !strcmp(args[0], "auto")) st->manual = 0;
        else return -1;
        if (out && cap) snprintf(out, cap, "模式=%s", st->manual ? "manual" : "auto");
        return 0;
    }
    if (!strcmp(op, "存草稿")) {
        if (!st->draft[0]) { if (out && cap) snprintf(out, cap, "草稿是空的"); return 0; }
        snprintf(st->save_pending, sizeof st->save_pending, "%s", st->draft);
        st->save_wanted = 1;                 /* 交给 propose 走受审通道落成标记节点 */
        if (out && cap) snprintf(out, cap, "草稿已交给安全内核保存（%zu 字节）", strlen(st->draft));
        return 0;
    }
    if (!strcmp(op, "读")) {
        if (out && cap)
            snprintf(out, cap, "草稿[%zu字节]=%s | 待发送[%zu字节]=%s | 模式=%s | 本轮发=%s",
                     strlen(st->draft), st->draft[0] ? st->draft : "(空)",
                     strlen(st->outbox), st->outbox[0] ? st->outbox : "(空)",
                     st->manual ? "manual" : "auto",
                     st->send_flag ? "是" : "否");
        return 0;
    }
    return -1;                                    /* 不认这个操作 → 算补丁出错（会计数） */
}

/* 暂存：流水线这一轮**本来要说的原文**。
 * auto：原样进待发送并标记发送（所以输出逐字节不变）；
 * manual：先落进**草稿**，AI 不 `发送` 就不说话。 */
static inline void patch_note_stage(Patch *p, const char *text) {
    NoteState *st = (NoteState *)p->priv;
    if (!st || !text) return;
    /* 一条总规矩：**绝不覆盖 AI 自己写过/排好的内容**。
     * 流水线只是"替它起草"，一旦它自己动过手（草稿或待发送非空），这一轮就听它的。 */
    if (st->draft[0] || st->outbox[0]) return;
    if (st->decided) return;                  /* 它已经明确表态（发送/不发），别再改主意 */
    snprintf(st->outbox, sizeof st->outbox, "%s", text);
    /* auto：流水线的话直接排进待发送（没编辑时逐字节等于无补丁）；
     * manual：只扣在待发送区，等它自己 `发送`（不设 send_flag，也不碰它的表态）。 */
    if (!st->manual) st->send_flag = 1;
}

static inline int patch_note_emit(Patch *p, char *out, size_t cap) {
    NoteState *st = (NoteState *)p->priv;
    if (!st) return 0;
    if (st->send_flag && !st->outbox[0] && st->draft[0])    /* 说了 → 草稿就是这一轮的话 */
        snprintf(st->outbox, sizeof st->outbox, "%s", st->draft);
    if (st->send_flag && st->outbox[0]) {
        snprintf(out, cap, "%s", st->outbox);
        st->outbox[0] = 0;                 /* 说过就说过了（下一轮重新暂存） */
        st->draft[0] = 0;
        st->send_flag = 0;
        st->decided = 0;                   /* 表态用完就清（下一轮重新决定） */
        return 1;
    }
    if (st->decided) {                     /* 明确"不发"：这一轮不说话 */
        st->decided = 0;
        return -1;
    }
    if (st->manual) return -1;             /* 手动模式：没发送 = 明确不说话 */
    return 0;                              /* 不接管：内核用流水线文本 */
}

/* 把草稿写成一个**标记节点**（普通符号节点，随图存盘）—— 这就是"草稿持久化"，
 * 而且**没动存档格式**：标记是图里已有的东西，恢复时扫一遍就行。
 * 名字里不留空格（老规矩，`::edges [草稿:…]` 要打得出）。 */
static inline int patch_note_propose(Patch *p, void *mq) {
    NoteState *st = (NoteState *)p->priv;
    if (!st || !st->save_wanted || !mq) return 0;
    st->save_wanted = 0;
    AI *ai = (AI *)st->ai;
    if (!ai) return 0;
    char nm[PATCH_NOTE_CAP];
    size_t off = 0;
    const char *head = MICRO_DRAFT_TOKEN_PREFIX;
    for (const char *q = head; *q && off + 2 < sizeof nm; ++q) nm[off++] = *q;
    for (const char *q = st->save_pending; *q && off + 2 < sizeof nm; ++q)
        nm[off++] = (*q == ' ') ? '_' : *q;
    nm[off++] = ']';
    nm[off] = 0;
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_TOKEN;
    /* 接回"我"；**没有"我"**（比如单元测试里的空脑子）时退回到池里第一个普通符号节点 ——
     * 这个挂靠点只是让标记有个归属，不参与语义（确定性：按池子下标取第一个）。 */
    Token *host = ai->self;
    if (!host)
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t && !t->is_garbage && t->name && t->name[0] != '[') { host = t; break; }
        }
    m.target = host;
    snprintf(m.name, sizeof m.name, "%s", nm);
    snprintf(m.source, sizeof m.source, "记事本:存草稿");
    return mod_propose((ModQueue *)mq, &m);
}

/* 挂载时把最近一次存下来的草稿扫回来（若有）。 */
static inline void patch_note_restore(NoteState *st, TokenPool *pool) {
    if (!st || !pool) return;
    const Token *best = NULL;
    for (size_t i = 0; i < pool->count; ++i) {
        const Token *t = pool->tokens[i];
        if (!t || t->is_garbage || !t->name) continue;
        if (strncmp(t->name, MICRO_DRAFT_TOKEN_PREFIX,
                    strlen(MICRO_DRAFT_TOKEN_PREFIX)) != 0) continue;
        if (!best || t->id > best->id) best = t;      /* 取最新的那一个 */
    }
    if (!best) return;
    size_t n = strlen(best->name);
    char txt[PATCH_NOTE_CAP];
    size_t k = 0;
    for (size_t i = strlen(MICRO_DRAFT_TOKEN_PREFIX);
         i < n && best->name[i] != ']' && k + 1 < sizeof txt; ++i)
        txt[k++] = (best->name[i] == '_') ? ' ' : best->name[i];
    txt[k] = 0;
    if (k == 0) return;
    snprintf(st->draft, sizeof st->draft, "%s", txt);
    st->restored = 1;
}

static inline int patch_note_init(Patch *p, TokenPool *pool, void *cfg) {
    NoteState *st = (NoteState *)p->priv;
    if (!st) return -1;
    void *saved_ai = st->ai;                 /* 挂载时设过：init 不能把它清掉 */
    memset(st, 0, sizeof *st);
    st->ai = saved_ai;
    /* **§6.109 翻默认**：不带 cfg 挂载时默认 **manual**（流水线文本先落草稿，要**显式发送**
     * 才出口 —— 比 auto 稳）。想回 auto：`::patch op 记事本 模式 auto` 或 `AIP_NOTEAUTO=1`。 */
    if (cfg) st->manual = (*(const int *)cfg != 0);
    else {
        st->manual = 1;
        const char *na = getenv("AIP_NOTEAUTO");
        if (na && na[0] == '1') st->manual = 0;
        const char *nm = getenv("AIP_NOTEMANUAL");
        if (nm && nm[0] == '1') st->manual = 1;
    }
    patch_note_restore(st, pool);            /* 草稿持久化：扫 [草稿:…] 拿回来 */
    return 0;
}

/* ---- 内建补丁工厂 ----
 * 只有 `自省`：不依赖任何外部设备，证明边界可用。
 * 视觉/3D/听觉那些**故意不预置** —— 它们是外设，装不装与内核无关。
 *
 * 壳用**文件级静态**而不是 malloc：内建补丁同时只能挂一个（同名不许重挂），
 * 复用同一个壳就没有泄漏，卸载/重挂也不需要额外记账。 */
static Patch g_patch_introspect;

static inline int patch_mount_builtin(AI *ai, const char *name) {
    Micro *mk = micro_of(ai);
    if (!mk || !name) return -1;
    if (!strcmp(name, "自省")) {
        if (patch_find(&mk->patches, "自省")) return -2;
        Patch *p = &g_patch_introspect;
        memset(p, 0, sizeof *p);
        p->name = "自省";
        p->kind = PATCH_SENSE;
        p->priv = ai;
        p->sense = patch_introspect_sense;
        p->mounted = 0;
        return patch_register(&mk->patches, p, ai->pool, ai);
    }
    if (!strcmp(name, "记事本")) {
        if (patch_find(&mk->patches, "记事本")) return -2;
        Patch *p = &g_patch_note;
        memset(p, 0, sizeof *p);
        p->name = "记事本";
        p->kind = PATCH_BOTH;
        p->priv = &g_note_state;
        g_note_state.ai = ai;                /* propose 里要用它（模型里的"我"） */
        p->stage = patch_note_stage;
        p->emit  = patch_note_emit;
        p->op    = patch_note_op;
        p->propose = patch_note_propose;
        p->init  = patch_note_init;
        p->mounted = 0;
        return patch_register(&mk->patches, p, ai->pool, NULL);
    }
    return -3;   /* 没有这个补丁 */
}

static inline int patch_unmount_by_name(AI *ai, const char *name) {
    Micro *mk = micro_of(ai);
    if (!mk || !name) return -1;
    return patch_unmount(&mk->patches, name);
}

/* 启用/停用（不卸载）：停用的补丁在这一步既不感知也不动作，但统计还在，
 * 重新启用时 err_streak 归零（出错停用的补丁可以被人工放回来再试一次）。 */
static inline int patch_set_enabled(AI *ai, const char *name, int on) {
    Micro *mk = micro_of(ai);
    if (!mk || !name) return -1;
    Patch *p = patch_find(&mk->patches, name);
    if (!p) return -1;
    p->enabled = on ? 1 : 0;
    if (on) { p->err_streak = 0; p->err[0] = 0; }
    return 0;
}

/* ============================ 检测器（内建的自修改来源） ============================ */

/* D1 共现固化：同一轮输入里距离 ≤2 的词对，图里两个方向都没有边，
 * 但它们的邻居已经高度重叠 —— 说明「它们是一类东西」这件事图里**已有证据**，
 * 只差把这条边写下来。
 * 判据是图内量出来的（共同邻居 + Jaccard），不是随机接线；
 * 这也是它和「A 和 B 挨着出现过就加边」的区别。 */
static inline void micro_detect_coactivation(AI *ai, char *const *words, size_t n,
                                             Micro *mk) {
    if (!ai || !mk || !words || n < 2 || mk->coact_off) return;
    Token *seq[32];
    size_t m = 0;
    for (size_t i = 0; i < n && m < 32; ++i) {
        if (!words[i] || !words[i][0] || micro_reserved(words[i])) continue;
        Token *t = pool_find(ai->pool, words[i]);
        if (micro_token_reserved(t)) continue;
        if (t->count < MICRO_D1_MIN_EDGES) continue;
        seq[m++] = t;
    }
    size_t budget = MICRO_D1_PER_STEP;
    for (size_t i = 0; i + 1 < m && budget; ++i) {
        for (size_t j = i + 1; j < m && j <= i + 2 && budget; ++j) {
            Token *a = seq[i], *b = seq[j];
            if (a == b) continue;
            if (micro_has_edge(a, b) || micro_has_edge(b, a)) continue;
            if (!micro_jaccard_at_least(a, b, MICRO_D1_MIN_COMMON)) continue;
            ModIntent in;
            memset(&in, 0, sizeof in);
            in.type = MOD_ADD_ASSOC;
            in.target = a;
            in.main = b;
            in.bump = 1;
            snprintf(in.source, sizeof in.source, "共现固化");
            if (mod_propose(&mk->queue, &in)) budget--;
        }
    }
}

/* D2 缺口命名：空缺感高的时候，把一个**新的** 符号节点造出来并接回网络。
 * 「未知」要能变成「有名字的东西」，否则系统永远只有「不知道」。
 * 严格限速（每 MICRO_D2_EVERY 轮最多一个），名字从这轮的词里取。 */
/* ---- D4 条件依赖：前提的**生产端** ----
 *
 * 一句话判据：**A 与 B 的同轮共现里，有 ≥70% 的轮次 P 也在场**（且都够多次数）
 * ⟹ 提议 `A —[前提]→ P → B`（建/标前提链接 + 拆 A—B 直连，由安全内核裁决）。
 *
 * 两张**有界**表（满了就淘汰计数最低的一行，不动态分配）：
 *   ① (A,B) → 同轮次数       —— 分母。**必须**独立于 P 记，否则分母只统计
 *      "P 在场的那些轮"，比值恒等于 100% —— 这是这类检测器最容易犯的错；
 *   ② (A,B,P) → 三元同轮次数 —— 分子。
 *
 * 只读图与流；提议走安全点。整张表是**会话内**的（不进存档），装载后重新计数。 */
/* 找/建一行 (A,B,P)；总要 bump 分母，P 在场时才 bump 分子。
 * 满了淘汰**分子最小**的一行（它最不可能成为前提）。 */
static inline CondTriple *cond_row(Micro *mk, Token *a, Token *b, Token *p, int present) {
    MicroMeta *mm = &mk->meta;
    size_t i;
    /* ⚠ **分母只在这里新建时记 1**，之后的每一轮都由 cond_bump_pair 统一加 ——
     *   两处都加的话一轮会加两次，比值恒等于 ~50%（实测踩过）。 */
    for (i = 0; i < mm->cond_tri_n; ++i) {
        CondTriple *c = &mm->cond_tri[i];
        if (c->a == a && c->b == b && c->p == p) {
            if (present && c->n_triple < 0xFFFFFFFFu) c->n_triple++;
            return c;
        }
    }
    if (mm->cond_tri_n < MICRO_D4_MAX_TRIPLES) i = mm->cond_tri_n++;
    else {
        size_t w = 0;
        for (size_t k = 1; k < MICRO_D4_MAX_TRIPLES; ++k)
            if (mm->cond_tri[k].n_triple < mm->cond_tri[w].n_triple) w = k;
        i = w;
    }
    CondTriple *c = &mm->cond_tri[i];
    c->a = a; c->b = b; c->p = p;
    c->n_pair = 1;
    c->n_triple = present ? 1u : 0u;
    c->fired = 0;
    return c;
}

/* 这一轮 (A,B) 同轮：**所有**以 (A,B) 为键的行都要 bump 分母
 * （不管这一轮 P 在不在场）—— 分母必须独立于 P，这是这类检测器的命门。 */
static inline void cond_bump_pair(Micro *mk, Token *a, Token *b) {
    MicroMeta *mm = &mk->meta;
    for (size_t i = 0; i < mm->cond_tri_n; ++i) {
        CondTriple *c = &mm->cond_tri[i];
        if (c->a == a && c->b == b && c->n_pair < 0xFFFFFFFFu) c->n_pair++;
    }
}

static inline void micro_detect_conditional(AI *ai, char *const *words, size_t n, Micro *mk) {
    if (!ai || !mk || !words || n < 2) return;   /* 两个词就能记"同轮"（分母） */
    if (mk->meta.cond_off) return;
    MicroMeta *mm = &mk->meta;
    /* ⓪ **先去掉同一轮里的重复词**：一轮里「你 我 你」这种重复会让分母只加一次、
     *   分子加两次 → 比值算成 179%、270% 这种鬼数（实测在 2 万行语料上就是这么发现的）。
     *   "同轮共现"按**轮**算，不按出现次数算。 */
    Token *u[32];
    size_t nu = 0;
    for (size_t i = 0; i < n && nu < 32; ++i) {
        Token *t = pool_find(ai->pool, words[i]);
        if (!t || micro_token_reserved(t)) continue;
        int dup = 0;
        for (size_t k = 0; k < nu; ++k) if (u[k] == t) { dup = 1; break; }
        if (!dup) u[nu++] = t;
    }
    if (nu < 2) return;
    /* ① 记「同轮」：这一轮里**已经有连结**的有序词对（连结先于前提存在）。
     *   ⚠ 短句（只有两个词）也要记分母 —— 只在 ≥3 个词时记的话，"没有 P 的那些轮"
     *     会被整轮跳过，比值就又变成"只统计 P 在场的轮"了（实测踩过）。 */
    for (size_t i = 0; i < nu; ++i) {
        Token *A = u[i];
        for (size_t j = 0; j < nu; ++j) {
            if (i == j) continue;
            Token *B = u[j];
            long ei = micro_edge_index(A, B);
            if (ei < 0) continue;                              /* 没连结就不谈前提 */
            if (token_assocs(A)[ei].hits < MICRO_D4_MIN_PAIR_HITS) continue;  /* 刚造的边不算证据 */
            cond_bump_pair(mk, A, B);                          /* 分母：所有 (A,B,·) 行 +1 */
            for (size_t k = 0; k < nu; ++k) {                  /* 分子：P 在场的那一行 +1 */
                if (k == i || k == j) continue;
                Token *P = u[k];
                if (P == A || P == B) continue;
                (void)cond_row(mk, A, B, P, 1);
            }
        }
    }
    /* ② 该提的提一条：分子/分母都够，比例够，而且还没建过 */
    size_t budget = MICRO_D4_PER_TURN;
    for (size_t t = 0; t < mm->cond_tri_n && budget; ++t) {
        CondTriple *c = &mm->cond_tri[t];
        if (c->fired || !c->a || !c->b || !c->p) continue;
        if (c->n_triple < MICRO_D4_MIN_TRIPLE) continue;
        if (c->n_pair < MICRO_D4_MIN_PAIRS) continue;
        if ((size_t)c->n_triple * MICRO_D4_PCT_DEN < (size_t)c->n_pair * MICRO_D4_PCT_NUM) continue;
        if (token_premise_link(c->a, c->p)) { c->fired = 1; continue; }  /* 已经标过了 */
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_INSERT_BETWEEN;
        m.target = c->a;
        m.main = c->b;
        m.flags = MOD_FLAG_SPLIT_DIRECT | MOD_FLAG_MARK_PREMISE;
        m.bump = 1;
        snprintf(m.name, sizeof m.name, "%s", c->p->name ? c->p->name : "");
        snprintf(m.source, sizeof m.source, "条件依赖");
        if (m.name[0] && mod_propose(&mk->queue, &m)) {
            c->fired = 1;
            mm->n_cond_proposed++;
            budget--;
        }
    }
}

static inline void micro_detect_gap(AI *ai, char *const *words, size_t n,
                                    const char *out, Micro *mk) {
    if (!ai || !mk || !words || !out || n == 0 || mk->gap_off) return;
    if (mk->meta.n_steps == 0 || (mk->meta.n_steps % MICRO_D2_EVERY) != 0) return;
    if (ai->pool->count >= (size_t)mk->safe.max_tokens_total) return;
    /* 只在这轮真的「没话说」时才命名：空缺感高 */
    Token *seed = pool_find(ai->pool, words[n - 1]);
    if (micro_token_reserved(seed) || seed->count < MICRO_D1_MIN_EDGES) return;
    if (!(token_gap(seed) >= (float)MICRO_D2_GAP_NUM / (float)MICRO_D2_GAP_DEN)) return;
    /* 名字取这轮最有信息量的两个词（跳过保留标记），没有就放弃 */
    char nm[48];
    const char *w1 = NULL, *w2 = NULL;
    for (size_t i = 0; i < n; ++i) {
        if (!words[i] || !words[i][0] || micro_reserved(words[i])) continue;
        Token *t = pool_find(ai->pool, words[i]);
        if (micro_token_reserved(t) || t == seed) continue;
        if (!w1) w1 = words[i];
        else if (!w2) { w2 = words[i]; break; }
    }
    if (!w1) return;
    if (w2) snprintf(nm, sizeof nm, "[新概念:%s+%s]", w1, w2);
    else    snprintf(nm, sizeof nm, "[新概念:%s]", w1);
    if (pool_find(ai->pool, nm)) return;   /* 已经造过就不重复造 */
    ModIntent in;
    memset(&in, 0, sizeof in);
    in.type = MOD_ADD_TOKEN;
    in.target = seed;                       /* 造出来就接回触发它的上下文 */
    snprintf(in.name, sizeof in.name, "%s", nm);
    snprintf(in.source, sizeof in.source, "缺口命名");
    mod_propose(&mk->queue, &in);
}

/* D3 休眠剪枝：长期没被碰过、权重为 0 的**共现**边可以考虑去掉。
 * 判据苛刻（边老、hits 为 0、两端都还有别的边、不是关系边），
 * 因为删错东西比不删更糟 —— 安全内核还会再拦一道（关系边一律不许删）。 */
static inline void micro_detect_prune(AI *ai, Micro *mk) {
    if (!ai || !mk || mk->prune_off) return;
    if (ai->pool->current_tick < MICRO_D3_MIN_AGE) return;
    size_t budget = MICRO_D3_PER_STEP;
    for (size_t i = 0; i < ai->pool->count && budget; ++i) {
        Token *t = ai->pool->tokens[i];
        if (micro_token_reserved(t) || t->count < MICRO_D1_MIN_EDGES) continue;
        for (size_t j = 0; j < t->count && budget; ++j) {
            AssocEntry *e = &token_assocs(t)[j];
            if (e->hits || e->rel || e->nrel || e->dyn) continue;
            if (edge_is_causal_guess(e)) continue;   /* 猜想边不剪（它不是知识，另有雪藏路径） */
            if (!e->main || micro_token_reserved(e->main)) continue;
            if (e->last_touch_tick + MICRO_D3_MIN_AGE > ai->pool->current_tick) continue;
            if (e->main->count < MICRO_D1_MIN_EDGES) continue;
            ModIntent in;
            memset(&in, 0, sizeof in);
            in.type = MOD_REMOVE_ASSOC;
            in.target = t;
            in.main = e->main;
            snprintf(in.source, sizeof in.source, "休眠剪枝");
            if (mod_propose(&mk->queue, &in)) budget--;
        }
    }
}

/* ============================ 应用 ============================ */

/* 把一条通过了审查的意图真的做出来。返回 1 = 产生了新结构，0 = 幂等（本来就有），
 * -1 = 失败。**只在这里改结构**，所以遍历里永远不会 realloc。 */
static inline int micro_apply_one(AI *ai, const ModIntent *m, char *note, size_t note_cap) {
    if (!ai || !m) return -1;
    TokenPool *p = ai->pool;
    if (note && note_cap) note[0] = 0;
    switch (m->type) {
    case MOD_ADD_TOKEN: {
        /* intern 而不是 pool_token：同名已存在时是**幂等**（返回老节点），
         * 不会造出两个同名符号节点 —— 那种图连 ::edges 都读不明白。 */
        int existed = pool_find(p, m->name) != NULL;
        Token *nt = pool_token_intern(p, m->name);
        if (!nt) return -1;
        if (m->target && m->target != nt) {
            token_add_assoc_unique(nt, m->target);
            token_touch(nt);
        }
        if (note) {
            /* 名字现在可以到 96 字节：有界拼（截断是故意的，别让 -Wformat-truncation 拦） */
            size_t k = 0;
            const char *q = m->name;
            while (*q && k + 3 < note_cap) note[k++] = *q++;
            note[k++] = (char)0xE2; note[k++] = (char)0x86; note[k++] = (char)0x92;  /* → */
            q = m->target && m->target->name ? m->target->name : "-";
            while (*q && k + 1 < note_cap) note[k++] = *q++;
            note[k] = 0;
        }
        return existed ? 0 : 1;
    }
    case MOD_ADD_ASSOC: {
        /* 另一个端点可以**按名字**给（`::op 加步骤 …` 的动作词可能是新词）：
         * 到这一步才 intern —— **在受审通道之内**（检查器已经按名字记过新符号节点预算）。
         * 意图本身是只读的，所以用局部 `Mn`，不改 `m`。 */
        Token *Mn = m->main;
        if (!Mn && m->name[0]) Mn = pool_token_intern(p, m->name);
        if (!Mn) return -1;
        int created = !micro_has_edge(m->target, Mn);
        if (token_add_assoc_unique(m->target, Mn) != 0) return -1;
        long idx = micro_edge_index(m->target, Mn);
        if (idx >= 0 && m->bump) token_assocs(m->target)[idx].hits += m->bump;
        if (m->action) token_set_action_by_target(m->target, Mn, m->action);
        if (m->flags & MOD_FLAG_PROCESS) {
            /* 过程记号（第 9 条 ②/⑤）：和 `[前提]` 同一个规矩 —— 记号本身在**这一步内** intern，
             * 随边的建立一起走账；role 标成「过程」把过程边与共现边分开。
             * 记号名有两种给法：**固定四个**（步骤/调用/回边/否则）用 flag 位拼；
             * **带名字的两种**（`[条件:X]`/`[异常:类型]`）由 expand 放进 `m->action` 直接带过来
             * —— 名字是参数，拼不进 flag。 */
            if (m->action) {
                (void)token_set_action_by_target(m->target, Mn, m->action);
            } else {
                char mk[32];
                if (m->flags & MOD_FLAG_PROC_BACK)      snprintf(mk, sizeof mk, "%s", PROC_BACK_MARK);
                else if (m->flags & MOD_FLAG_PROC_ELSE) snprintf(mk, sizeof mk, "%s", PROC_ELSE_MARK);
                else if (m->flags & MOD_FLAG_PROC_CALL) snprintf(mk, sizeof mk, "%s", PROC_CALL_MARK);
                else snprintf(mk, sizeof mk, "%s%u]", PROC_STEP_PREFIX, (unsigned)m->rel_id);
                Token *mkt = pool_token_intern(p, mk);
                if (mkt) (void)token_set_action_by_target(m->target, Mn, mkt);
            }
        }
        if (idx >= 0 && (m->flags & MOD_FLAG_PROCESS))
            token_assocs(m->target)[idx].role = ASSOC_ROLE_PROCESS;   /* 过程边（第 9 条 ①） */
        if (note) snprintf(note, note_cap, "%s→%s%s",
                           m->target->name ? m->target->name : "?",
                           Mn->name ? Mn->name : "?",
                           created ? "（新）" : "（已有，只加权）");
        return created ? 1 : 0;
    }
    case MOD_TAG_DERIVED: {
        /* 沉淀：先保证边在，再按极性置位，最后把 role 标成「推出来的」。
         * 顺序要紧：token_tag_rel/nrel 会把缓存标记清掉（那是「观测压过缓存」
         * 的不变式），所以**标记必须最后打**。 */
        int created = !micro_has_edge(m->target, m->main);
        if (token_add_assoc_unique(m->target, m->main) != 0) return -1;
        long idx = micro_edge_index(m->target, m->main);
        if (idx < 0) return -1;
        AssocEntry *e = &token_assocs(m->target)[idx];
        uint16_t bit = REL_BIT(m->rel_id);
        if (!m->neg) {
            if (e->rel & bit) {
                /* 已经是同向的观测事实了 —— 不用沉淀（也拒绝降级它） */
                if (e->role != ASSOC_ROLE_DERIVED) return 0;
            }
            e->rel |= bit;
            if (p) pool_rev_add(p, m->main, m->target->id, m->rel_id, 0);   /* §6.214 */
        } else {
            if (e->nrel & bit) {
                if (e->role != ASSOC_ROLE_DERIVED) return 0;
            }
            e->nrel |= bit;
            if (p) pool_rev_add(p, m->main, m->target->id, m->rel_id, 1);   /* §6.214 */
        }
        e->role = ASSOC_ROLE_DERIVED;
        token_cache_enqueue(m->target);   /* §6.127：新落的推论边进复核队列（不再每轮扫全图） */
        e->hits += m->bump ? m->bump : 1;
        e->mem_state = MEM_ACTIVE;
        e->last_touch_tick = p->current_tick;
        /* 名字里只放关系 id：micro.h 在 rel.h 之前包含，看不到关系词表。
         * 给人看的名字由思维层（它能看到 rel.h）自己写进痕里。 */
        if (note) snprintf(note, note_cap, "%s %srel%u %s（沉淀）",
                           m->target->name ? m->target->name : "?",
                           m->neg ? "不 " : "", (unsigned)m->rel_id,
                           m->main->name ? m->main->name : "?");
        return created ? 1 : 0;
    }
    case MOD_TAG_REL: {
        /* ⚠ **否定也要走同一条路**：以前这里恒调 token_tag_rel（正向），
         *   于是"路由过去的否定观测"会被写成肯定 → 改口语义当场失效
         *   （实测被 `AIP_ROUTE=1` 的 revision/consolidate 两套抓住）。 */
        int rc = (m->rel_id >= REL_DYN_BASE)
                     ? token_tag_dyn_neg(m->target, m->main,
                                         (uint8_t)(m->rel_id - REL_DYN_BASE + 1),
                                         m->neg ? 1 : 0, m->bump)
                     : (m->neg ? token_tag_nrel(m->target, m->main, m->rel_id,
                                         m->bump ? m->bump : REL_BOOST)
                        : token_tag_rel(m->target, m->main, m->rel_id,
                                        m->bump ? m->bump : REL_BOOST));
        if (rc != 0) return -1;
        if (note) snprintf(note, note_cap, "%s-[rel%u]->%s",
                           m->target->name ? m->target->name : "?",
                           (unsigned)m->rel_id,
                           m->main->name ? m->main->name : "?");
        return 1;
    }
    case MOD_BUMP_HITS: {
        long idx = micro_edge_index(m->target, m->main);
        if (idx < 0) return -1;
        token_assocs(m->target)[idx].hits += m->bump ? m->bump : 1;
        token_assocs(m->target)[idx].last_touch_tick = p->current_tick;
        if (note) snprintf(note, note_cap, "hits+%u", m->bump ? m->bump : 1u);
        return 1;
    }
    case MOD_DECAY:
        token_decay_hits(m->target, 0.9f);
        if (note) snprintf(note, note_cap, "衰减 %s", m->target->name ? m->target->name : "?");
        return 1;
    case MOD_SET_ACTION:
        if (token_set_action_by_target(m->target, m->main, m->action) != 0) return -1;
        if (note) snprintf(note, note_cap, "设动作 %s", m->action->name ? m->action->name : "?");
        return 1;
    case MOD_SET_PARENT:
        token_set_parent(m->target, m->main);
        if (note) snprintf(note, note_cap, "父类 %s→%s",
                           m->target->name ? m->target->name : "?",
                           m->main->name ? m->main->name : "?");
        return 1;
    case MOD_SET_CATEGORY:
        token_set_category(m->target, 1);
        if (note) snprintf(note, note_cap, "设类别 %s", m->target->name ? m->target->name : "?");
        return 1;
    case MOD_REMOVE_ASSOC: {
        if (token_remove_assoc(m->target, m->main) != 0) return -1;
        if (note) snprintf(note, note_cap, "删 %s→%s",
                           m->target->name ? m->target->name : "?",
                           m->main->name ? m->main->name : "?");
        return 1;
    }
    /* **在 A 与 B 之间插一个符号节点**：A→N→B；带 SPLIT_DIRECT 时让 A、B 不再直连。
     * 一体化做三件事，所以是**一条**意图：分成三条的话「拆直连」会在 N 还没建好时执行。 */
    case MOD_INSERT_BETWEEN: {
        Token *A = m->target, *B = m->main;
        int existed = pool_find(p, m->name) != NULL;
        Token *N = pool_token_intern(p, m->name);
        if (!N || N == A || N == B) return -1;
        if (token_add_assoc_unique(A, N) != 0) return -1;   /* A → N */
        if (token_add_assoc_unique(N, B) != 0) return -1;   /* N → B */
        {   /* 给这两条中转边一点初始权重：hits==0 的边会被当成「没语义依据的残留」 */
            uint32_t bump = m->bump ? m->bump : 1;
            for (size_t k = 0; k < A->count; ++k)
                if (token_assocs(A)[k].main == N) token_assocs(A)[k].hits += bump;
            for (size_t k = 0; k < N->count; ++k)
                if (token_assocs(N)[k].main == B) token_assocs(N)[k].hits += bump;
        }
        /* **前提**：把 A→N 标成「前提链接」——N 就是这两个词之间的**前提符号节点**。
         * 它是行为符号节点（标记）挂在动作槽上，所以：联想照走（A→N→B 都是普通边）、
         * 前提符号节点照常能被联想到（它就是普通符号节点）、而逻辑问得出它是谁
         * （token_premise_between）。 */
        if (m->flags & (MOD_FLAG_MARK_PREMISE | MOD_FLAG_MARK_PREMISE_NOT)) {
            const char *mk = (m->flags & MOD_FLAG_MARK_PREMISE_NOT) ? AI_PREMISE_NOT_MARK
                                                                    : AI_PREMISE_MARK;
            Token *mark = pool_token_intern(p, mk);
            if (mark) (void)token_set_action_by_target(A, N, mark);
        }
        int cut = 0, kept = 0;
        if (m->flags & MOD_FLAG_SPLIT_DIRECT) {
            long i = micro_edge_index(A, B);
            if (i >= 0) {
                if (micro_edge_removable(&token_assocs(A)[i])) {
                    if (token_remove_assoc(A, B) == 0) cut = 1;
                } else {
                    kept = 1;      /* 直连是**知识**（观测关系边）→ 铁律 2：不许拆 */
                }
            }
        }
        if (note && note_cap) {
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%.16s→%.16s→%.16s%s%s",
                     A->name ? A->name : "?", m->name, B->name ? B->name : "?",
                     cut ? "（拆直连）" : "",
                     kept ? "（直连是知识，保留）" : "");
            /* 手动按边界拷（note_cap 是参数，直接用 snprintf 会触发
             * -Wformat-truncation：编译器按最大可能长度算，而我们只关心装得下） */
            size_t l = strlen(tmp);
            if (l >= note_cap) l = note_cap - 1;
            memcpy(note, tmp, l);
            note[l] = 0;
        }
        return existed ? 1 : 2;        /* 2 = 连符号节点也是新建的 */
    }
    /* 回收临时草稿（安全内核已经保证它没人引用、且是 [临: 命名空间） */
    case MOD_REMOVE_TOKEN: {
        /* 软删（和 ai_gc 一样：标垃圾、清空边表），不把符号节点从池里搬走 ——
         * 池里到处是指针，物理搬走会让它们全悬空。 */
        Token *t = m->target;
        t->is_garbage = 1;
        t->count = 0;
        if (note) snprintf(note, note_cap, "回收临时 %s",
                           t->name ? t->name : "?");
        return 1;
    }
    case MOD_PATCH_CALL: {
        /* 调用补丁：**内核不解释这个操作是什么**，只做有界派发 + 故障隔离。
         * 没挂/停用/不认这个操作 → 失败（会计进补丁自己的错误计数）。 */
        char rep[160];
        /* ⚠ `char[4][40]` **不是** `char**`：直接强转会让补丁把参数行当指针读
         *   （实测当场 SEGV）。必须先落成真正的指针数组。 */
        char *argv[4];
        for (uint8_t i = 0; i < m->n_args && i < 4; ++i) argv[i] = (char *)m->args[i];
        int rc = patch_op_call_by_name(ai, m->patch, m->name,
                                       argv, m->n_args, rep, sizeof rep);
        if (rc < 0) return -1;
        /* **草稿留痕**（§6.103，默认关）：`::notepad on` 发送会把草稿区清掉 —— 发送成功之后
         * 由**核心层**（这里有 `ai` 和队列）把刚发出去的那句话另存成一条 `[草稿:…]` 标记节点。
         * 判据只用"补丁=记事本、操作=发送、回复里带正文"这三条（正文由补丁层交出来）。 */
        if (ai->draft_keep && strcmp(m->patch, "记事本") == 0 && strcmp(m->name, "发送") == 0) {
            const char *colon = strstr(rep, "）：");
            if (colon) {
                ModIntent dk;
                memset(&dk, 0, sizeof dk);
                dk.type = MOD_ADD_TOKEN;
                size_t off = 0;
                const char *hp = MICRO_DRAFT_TOKEN_PREFIX;
                for (const char *q = hp; *q && off + 2 < sizeof dk.name; ++q) dk.name[off++] = *q;
                for (const char *q = colon + strlen("）："); *q && off + 2 < sizeof dk.name && off < 70; ++q)
                    dk.name[off++] = *q;
                if (off + 1 < sizeof dk.name) dk.name[off++] = ']';
                dk.name[off] = 0;
                dk.target = ai->self;
                snprintf(dk.source, sizeof dk.source, "输出:草稿留痕");
                (void)mod_propose(&micro_of(ai)->queue, &dk);
            }
        }
        if (note) {
            /* 手动拼而不是 snprintf：patch(24)+name(48)+rep(160) 一起格式化会被
             * -Werror=format-truncation 拦下（note 只有 192）。截断是**故意**的。 */
            size_t off = 0;
            #define NOTE_ADD(str) do { \
                const char *q_ = (str); \
                while (*q_ && off + 1 < note_cap) note[off++] = *q_++; \
            } while (0)
            NOTE_ADD(m->patch); NOTE_ADD("."); NOTE_ADD(m->name); NOTE_ADD("：");
            NOTE_ADD(rep[0] ? rep : "ok");
            note[off] = 0;
            #undef NOTE_ADD
        }
        return 0;               /* 0 = 没有产生新图结构（它动的是补丁自己的缓冲） */
    }
    default:
        return -1;
    }
}

/* ============================ 手动试一条意图（::mod try） ============================ */

/* 每「步」重开预算。自动路径里由 micro_step 开头做；
 * 手动路径（::mod try）里一条命令算一步，所以也要重开一次。 */
static inline void safety_step_begin(AI *ai) {
    Micro *mk = micro_of(ai);
    if (!mk) return;
    mk->safe.step_new_tokens = 0;
    mk->safe.step_new_edges = 0;
    mk->safe.step_patch_calls = 0;
}

/* 前向声明：micro_try 要用它，而它的定义在下面「元认知」那一节 */
static inline void micro_log_apply(Micro *mk, const ModIntent *m, const char *note);

/* 手册上的一条意图：走的还是**同一条**「审查 → 应用 → 记账 → 审计」路径。
 * 所以它同时是个好用的调试入口 —— 想验证某条边到底会被放行、
 * 还是被哪一条底线拦住，提一条就知道（::safe 里能看到理由）。
 *
 * 但要说明白：**这条不是自修改**。按定义，自修改必须是系统运行中
 * 自己产生的修改；手动试是从外面施加的，和 ::asm / ::learn 同级，
 * 只是用来驱动同一套机制。 */
static inline SafeReason micro_try(AI *ai, const ModIntent *in, char *note, size_t note_cap) {
    Micro *mk = micro_of(ai);
    if (!mk || !in) return SAFE_DENY_BADREF;
    ModIntent m = *in;
    if (m.source[0] == 0) snprintf(m.source, sizeof m.source, "手动");
    SafeReason r = safety_review(&mk->safe, ai->pool, &m);
    if (r != SAFE_OK) {
        mk->safe.n_denied++;
        mk->safe.deny_by_reason[r]++;
        mk->meta.n_denied++;
        safety_audit(&mk->safe, ai->pool->current_tick, &m, 0, r);
        return r;
    }
    safety_charge(&mk->safe, ai->pool, &m);            /* 先记账（预算是「这一步允许多少」） */
    int rc = micro_apply_one(ai, &m, note, note_cap);
    if (rc < 0) {
        safety_refund(&mk->safe, ai->pool, &m);        /* 没改成 → 账退回去 */
        mk->safe.n_denied++;
        mk->safe.deny_by_reason[SAFE_DENY_BADREF]++;
        mk->meta.n_denied++;
        safety_audit(&mk->safe, ai->pool->current_tick, &m, 0, SAFE_DENY_BADREF);
        return SAFE_DENY_BADREF;
    }
    safety_audit(&mk->safe, ai->pool->current_tick, &m, 1, SAFE_OK);
    mk->meta.n_applied++;
    ai->writes[AI_WSRC_MICRO]++;      /* 统一写入通道：手动/路由过来的受审落地也记账 */
    switch (m.type) {
    case MOD_ADD_TOKEN:    mk->meta.n_new_tokens++; break;
    case MOD_ADD_ASSOC:    if (rc == 1) mk->meta.n_new_edges++; break;
    case MOD_TAG_DERIVED:  if (rc == 1) mk->meta.n_new_edges++;
                           mk->meta.n_derived++; break;
    case MOD_REMOVE_ASSOC: mk->meta.n_removed++; break;
    case MOD_BUMP_HITS:    mk->meta.n_bumped++; break;
    case MOD_TAG_REL:      mk->meta.n_rel++; break;
    case MOD_INSERT_BETWEEN:
        if (rc >= 1) mk->meta.n_new_edges += 2;
        if (rc == 2) mk->meta.n_new_tokens++;
        break;
    case MOD_REMOVE_TOKEN: mk->meta.n_removed++; break;
    default: break;
    }
    micro_log_apply(mk, &m, note);
    return SAFE_OK;
}

/* ============================ 符号节点行为（图操作） ============================
 *
 * 「给符号节点加上行为」：一个符号节点可以**命令图改变自己** —— 接线、按语境接线、
 * 在两者之间插一个中转符号节点、回收草稿……这些是**操作**（机制），不是知识。
 *
 * 所以它们写成一张表：{名字, 参数形状, 说明, 参数个数, 展开函数}。
 * 展开函数**只产出 ModIntent、不碰图** —— 结构改动照旧只有 `micro_step` 一个入口，
 * 每条意图都要过安全内核（预算、底线、审计），所以「符号节点有行为」不等于
 * 「符号节点能绕过审查改脑」。
 *
 * 新增一种行为 = 表里加一行 + 写它那个展开函数（不碰循环、不碰安全内核）。
 * 现在的行为（用户点名的三条 + 淘汰「语境」后的前提表示）：
 *   ① 加前提    ：A 与 B 之间加一个**前提符号节点** P（缺省临时符号节点）→ A —[前提]→ P → B
 *   ② 前提接线  ：让 A 经前提 P 也能到 Y（选择判据是**图里的前提链接**，不是边上的字段）
 *   ③ 中间插词  ：在 A 与 B 之间插一个符号节点 N，使二者**不直连**、经 N 中转
 *   ④ 回收临时  ：回收没人引用的草稿（前提缺省值就是草稿）
 */
typedef struct {
    const char *name;      /* 行为名（::op 后面写它） */
    const char *shape;     /* 人看的参数形状 */
    const char *help;      /* 一句话说明 */
    size_t      min_args;
    size_t      max_args;
    /* 展开成意图：返回条数，-1 = 参数/前提不合法（note 里写给人看的原因）。
     * 注意它**只提议**：能不能落地由安全内核说了算。 */
    int (*expand)(AI *ai, char *const *args, size_t n, ModQueue *q,
                  char *note, size_t cap);
} TokenOp;

/* 〇 调用补丁 <补丁> <操作> [参数…]
 *
 * 给 AI 一条**受审**的路去用外设（输出侧的记事本、草稿区、发送……）：
 * 它不改图，但照样走安全内核（预算/审计/日志/故障隔离同一规格）。
 * 补丁自己解释 op 的语义 —— 内核只负责有界派发。 */
static inline int token_op_call_patch(AI *ai, char *const *args, size_t n, ModQueue *q,
                                      char *note, size_t cap) {
    (void)ai;
    if (n < 2) { snprintf(note, cap, "要 <补丁> <操作> [参数…]"); return -1; }
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_PATCH_CALL;
    snprintf(m.patch, sizeof m.patch, "%s", args[0]);
    snprintf(m.name, sizeof m.name, "%s", args[1]);
    for (size_t i = 2; i < n && m.n_args < 4; ++i)
        snprintf(m.args[m.n_args++], sizeof m.args[0], "%s", args[i]);
    snprintf(m.source, sizeof m.source, "行为:调用补丁");
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "调用 %s.%s（%s）", args[0], args[1],
             ok ? "已提出" : "与队列里的重复，丢了");
    return ok ? 1 : 0;
}

/* ① 加前提 <A> <B> [前提名]
 *
 * **淘汰「语境」后的主角**：一条「有前提的连结」= `A —[前提]→ P → B`
 *   · P 是**普通符号节点**（前提符号节点）：联想照走、能被正常联想到、逻辑查得到；
 *   · `[前提]` 是**行为符号节点**（标记），挂在 A→P 的动作槽上；
 *   · 不给名字时 P 用**临时符号节点** `[临:前提]`（草稿：不存盘、可回收）。
 * 建好后：直接连结 A—B 会被拆掉（连结改「经过前提」），这条由安全内核裁决。 */
/* ---- 过程记号（第 9 条 ①+②）：建过程 / 加步骤 / 加调用 / 加回边 ----
 *
 * 一个"过程"= 根符号节点 `[过程:名字]` + 从它出去的过程边；控制流记号挂在**动作槽**上。
 * 全部走 `token_op_run`（提议 → 安全内核审查 → 应用 → 记账）—— 和自修改同一条路，
 * "过程记号"不是绕过内核的后门。**记号与 role 都是数据**，不改存档布局。 */
static inline Token *proc_find(AI *ai, const char *bare) {
    if (!ai || !bare || !bare[0]) return NULL;
    char full[128];
    int n = snprintf(full, sizeof full, "%s%s]", PROC_TOKEN_PREFIX, bare);
    if (n <= 0 || (size_t)n >= sizeof full) return NULL;
    return pool_find(ai->pool, full);
}

static inline int token_op_make_proc(AI *ai, char *const *args, size_t n, ModQueue *q,
                                     char *note, size_t cap) {
    if (!ai || n < 1 || !args[0] || !args[0][0]) { snprintf(note, cap, "缺过程名"); return -1; }
    char full[64];                       /* note 只有 96 字节：名字本身短一点，免得截断告警 */
    int ln = snprintf(full, sizeof full, "%s%s]", PROC_TOKEN_PREFIX, args[0]);
    if (ln <= 0 || (size_t)ln >= sizeof full) { snprintf(note, cap, "过程名太长"); return -1; }
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_TOKEN;
    snprintf(m.name, sizeof m.name, "%s", full);
    /* 挂靠点（和草稿补丁同一个规矩）：标记得有个归属，它不参与语义 ——
     * 没有"我"（单元测试里的空脑子）就退回到池里第一个普通符号节点。 */
    Token *host = ai->self;
    if (!host)
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t && !t->is_garbage && t->name && t->name[0] != '[') { host = t; break; }
        }
    m.target = host;
    snprintf(m.source, sizeof m.source, "行为:建过程");
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s", full, ok ? " 备好了" : " 与队列重复");
    return ok ? 1 : 0;
}

/* 加一步：`<过程> <动作> [序号]` → 边 `[过程:X] —[步骤:N]→ 动作`，role=过程。
 * 序号不给就按已有步骤数 +1（**顺序写在记号上**，查询时才排得出来）。 */
static inline int token_op_add_step(AI *ai, char *const *args, size_t n, ModQueue *q,
                                    char *note, size_t cap, const char *mark) {
    if (n < 2) { snprintf(note, cap, "要 <过程> <动作>"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s（先 `::op 建过程 %s`）", args[0], args[0]); return -1; }
    /* `加回边 <过程> <步号>`：指回**第 N 步**（端点用 `[步骤:N]` 那个记号符号节点）。
     * 为什么给这个形状：图的边是 (起点,终点) 唯一的 —— 指回**动作词**会和那一步共用同一条边，
     * 把它的动作槽覆盖掉（实测：`加步骤 泡茶 烧水` 之后再 `加回边 泡茶 烧水`，那一步就没了）。
     * 指回步号不会撞车。 */
    char backnum[32];
    backnum[0] = 0;
    if (mark && strcmp(mark, PROC_BACK_MARK) == 0 && args[1] && args[1][0]) {
        int alldig = 1;
        for (const char *q = args[1]; *q; ++q) if (*q < '0' || *q > '9') { alldig = 0; break; }
        if (alldig) snprintf(backnum, sizeof backnum, "%s%s]", PROC_STEP_PREFIX, args[1]);
    }
    /* `加调用 <过程> <子过程名>`：名字没带 `[过程:` 前缀、但**确实有个同名过程**时，
     * 自动补上前缀（省得让人手打方括号）—— 认不出就当普通动作词。 */
    char callref[96];
    callref[0] = 0;
    if (mark && strcmp(mark, PROC_CALL_MARK) == 0 && args[1] && args[1][0] &&
        strncmp(args[1], PROC_TOKEN_PREFIX, strlen(PROC_TOKEN_PREFIX)) != 0) {
        char probe[96];
        int pn = snprintf(probe, sizeof probe, "%s%s]", PROC_TOKEN_PREFIX, args[1]);
        if (pn > 0 && (size_t)pn < sizeof probe && pool_find(ai->pool, probe))
            snprintf(callref, sizeof callref, "%s", probe);
    }
    const char *refname = callref[0] ? callref : (backnum[0] ? backnum : args[1]);
    Token *act = pool_find(ai->pool, refname);   /* 可能还没有：走名字端点（内核会建） */
    (void)act;                                 /* 名字端点由内核在**这一步里**解析/intern */
    int idx = 0;
    uint8_t pflag = MOD_FLAG_PROC_CALL;
    if (!mark) {
        pflag = MOD_FLAG_PROC_STEP;
        if (n >= 3 && args[2] && args[2][0]) idx = atoi(args[2]);
        if (idx <= 0) {                        /* 自动排号：现有步骤里最大的 +1 */
            for (size_t i = 0; i < root->count; ++i) {
                int s = edge_step_index(&token_assocs(root)[i]);
                if (s > idx) idx = s;
            }
            idx++;
        }
        if (idx > 255) { snprintf(note, cap, "步号太大（最多 255）"); return -1; }
    } else if (strcmp(mark, PROC_BACK_MARK) == 0) {
        pflag = MOD_FLAG_PROC_BACK;
    } else if (strcmp(mark, PROC_ELSE_MARK) == 0) {
        pflag = MOD_FLAG_PROC_ELSE;
    }
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = act;                              /* 池里有就直接指；没有就让 name 说话 */
    snprintf(m.name, sizeof m.name, "%s", refname);  /* 名字端点（内核按它解析/记账） */
    m.rel_id = (uint8_t)idx;
    m.flags = (uint8_t)(MOD_FLAG_PROCESS | pflag);
    m.bump = 1;
    snprintf(m.source, sizeof m.source, "行为:加步骤");
    int ok = mod_propose(q, &m);
    {
        char mk[32];
        if (pflag == MOD_FLAG_PROC_BACK)      snprintf(mk, sizeof mk, "%s", PROC_BACK_MARK);
        else if (pflag == MOD_FLAG_PROC_ELSE) snprintf(mk, sizeof mk, "%s", PROC_ELSE_MARK);
        else if (pflag == MOD_FLAG_PROC_CALL) snprintf(mk, sizeof mk, "%s", PROC_CALL_MARK);
        else                                  snprintf(mk, sizeof mk, "%s%d]", PROC_STEP_PREFIX, idx);
        snprintf(note, cap, "%s%s —%s→ %s（%s）", PROC_TOKEN_PREFIX, args[0], mk, refname,
                 ok ? "已提出" : "与队列重复");
    }
    return ok ? 1 : 0;
}

static inline int token_op_add_step_n(AI *ai, char *const *args, size_t n, ModQueue *q,
                                      char *note, size_t cap) {
    return token_op_add_step(ai, args, n, q, note, cap, NULL);
}
static inline int token_op_add_call(AI *ai, char *const *args, size_t n, ModQueue *q,
                                    char *note, size_t cap) {
    return token_op_add_step(ai, args, n, q, note, cap, PROC_CALL_MARK);
}
/* `加回边 <过程> <步号或动作> [条件词]`：给了条件词就记成 `[回边:条件词]`（每轮先看它成不成立）。
 * 为什么条件写在记号里：图里一条 (根, 目标) 只有一条边 —— 条件另挂一条会和回边自己撞车。 */
static inline int token_op_add_back(AI *ai, char *const *args, size_t n, ModQueue *q,
                                    char *note, size_t cap) {
    if (n < 2) { snprintf(note, cap, "要 <过程> <步号或动作> [条件词]"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s", args[0]); return -1; }
    char backnum[32];
    backnum[0] = 0;
    if (args[1] && args[1][0]) {
        int alldig = 1;
        for (const char *q = args[1]; *q; ++q) if (*q < '0' || *q > '9') { alldig = 0; break; }
        if (alldig) snprintf(backnum, sizeof backnum, "%s%s]", PROC_STEP_PREFIX, args[1]);
    }
    const char *refname = backnum[0] ? backnum : args[1];
    char mk[96];
    if (n >= 3 && args[2] && args[2][0]) snprintf(mk, sizeof mk, "%s%s]", PROC_BACK_PREFIX, args[2]);
    else                                 snprintf(mk, sizeof mk, "%s", PROC_BACK_MARK);
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = pool_find(ai->pool, refname);
    snprintf(m.name, sizeof m.name, "%s", refname);
    snprintf(m.source, sizeof m.source, "行为:加回边");
    m.flags = MOD_FLAG_PROCESS;
    m.action = pool_token_intern(ai->pool, mk);
    if (!m.action) { snprintf(note, cap, "回边记号建不出来"); return -1; }
    m.bump = 1;
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s —%s→ %s（%s）", PROC_TOKEN_PREFIX, args[0], mk, refname,
             ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}
/* ⑤ 分支/循环/异常的三个写入口（都复用"名字端点"那条路，走同一份受审通道）。
 * `加条件 <过程> <条件词> <动作>`：`[过程:X] —[条件:条件词]→ 动作`（条件成立才走这一步）。
 * `加否则 <过程> <动作>`：条件不成立时走的那一支（和上面那条互斥）。
 * `加异常 <过程> <类型> <动作>`：哪一步失败时跳去做的处理步（类型只作标签，v1 不按类型分派）。 */
/* `加断言 <过程> <主语> <关系> <宾语> [不] [序号]`：给过程加一个**要核的步骤**
 * （记号 = `[步骤:N@要:S/R/B]`）—— 跑的时候用 `rel_solve` + `der_verify` 独立复核，
 * 这就是"步骤带对象"那条（第 9 条 ① 的收口）。 */
/* `加执行 <过程> <动作词> [参数…]`：给过程加一个**真的会调动作**的步骤。
 * 记号端点是 `[执行:动作词 参数…]`（动作符号节点）；动作词必须是白名单里的（`::act list` 看）。 */
static inline int token_op_add_exec(AI *ai, char *const *args, size_t n, ModQueue *q,
                                    char *note, size_t cap) {
    if (n < 2) { snprintf(note, cap, "要 <过程> <动作词> [参数…]"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s", args[0]); return -1; }
    /* 白名单检查在 action.h 那层（这里只做"名字像不像"的粗查：不许带空格/斜杠） */
    for (const char *q2 = args[1]; *q2; ++q2)
        if (*q2 == ' ' || *q2 == '/' || *q2 == ';' || *q2 == '|') {
            snprintf(note, cap, "动作词只能是白名单里的命令名（不带参数/路径）");
            return -1;
        }
    char mk[128];
    size_t off = 0;
    for (const char *q2 = MICRO_EXEC_TOKEN_PREFIX; *q2 && off + 2 < sizeof mk; ++q2) mk[off++] = *q2;
    for (const char *q2 = args[1]; *q2 && off + 2 < sizeof mk; ++q2) mk[off++] = *q2;
    for (size_t i = 2; i < n && off + 2 < sizeof mk; ++i) {
        if (!args[i] || !args[i][0]) continue;
        mk[off++] = ' ';
        for (const char *q2 = args[i]; *q2 && off + 2 < sizeof mk; ++q2) mk[off++] = *q2;
    }
    if (off + 1 < sizeof mk) mk[off++] = ']';
    mk[off] = 0;
    /* 步号：接着现有的排 */
    int idx = 0;
    for (size_t i = 0; i < root->count; ++i) {
        int sN = edge_step_index(&token_assocs(root)[i]);
        if (sN > idx) idx = sN;
    }
    idx++;
    char smk[64];
    snprintf(smk, sizeof smk, "%s%d]", PROC_STEP_PREFIX, idx);
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = pool_token_intern(ai->pool, mk);
    if (!m.main) { snprintf(note, cap, "动作符号节点建不出来"); return -1; }
    snprintf(m.name, sizeof m.name, "%s", mk);
    snprintf(m.source, sizeof m.source, "行为:加执行");
    m.flags = MOD_FLAG_PROCESS;
    m.rel_id = (uint8_t)idx;
    m.action = pool_token_intern(ai->pool, smk);
    m.bump = 1;
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s 第 %d 步 = %s（%s）", PROC_TOKEN_PREFIX, args[0], idx, mk,
             ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}

static inline int token_op_add_claim(AI *ai, char *const *args, size_t n, ModQueue *q,
                                     char *note, size_t cap) {
    if (n < 4) { snprintf(note, cap, "要 <过程> <主语> <关系> <宾语> [不] [序号]"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s", args[0]); return -1; }
    Token *sub = pool_find(ai->pool, args[1]);
    Token *obj = pool_find(ai->pool, args[3]);
    /* 关系**只记名字、到跑的时候再解析**：micro.h 在 rel.h 之前（这一层看不见 rel_id_of）。
     * 关系词表启动时都会进池子，所以这里先按"词在不在"做个粗检查。 */
    Token *relw = pool_find(ai->pool, args[2]);
    /* §6.509 B3：`$1`..`$4` 是**跑的时候才绑**的实参占位，建的时候当然不在池子里 ——
     * 占位符跳过"认得全"检查（真给不出实参时，跑那一步会因认不全断点，照实说）。 */
    int sub_ph = (args[1] && args[1][0] == '$' && args[1][1] >= '1' && args[1][1] <= '9' && !args[1][2]);
    int obj_ph = (args[3] && args[3][0] == '$' && args[3][1] >= '1' && args[3][1] <= '9' && !args[3][2]);
    int rel_ph = (args[2] && args[2][0] == '$' && args[2][1] >= '1' && args[2][1] <= '9' && !args[2][2]);
    if ((!sub && !sub_ph) || (!obj && !obj_ph) || (!relw && !rel_ph)) {
        snprintf(note, cap, "主语/关系/宾语认不全（关系词得是表里的；占位写 `$1`..`$4`）");
        return -1;
    }
    int neg = 0, idx = 0;
    for (size_t i = 4; i < n; ++i) {
        if (!args[i] || !args[i][0]) continue;
        if (strcmp(args[i], "不") == 0) { neg = 1; continue; }
        int d = atoi(args[i]);
        if (d > 0) idx = d;
    }
    if (idx <= 0) {
        for (size_t i = 0; i < root->count; ++i) {
            int sN = edge_step_index(&token_assocs(root)[i]);
            if (sN > idx) idx = sN;
        }
        idx++;
    }
    if (idx > 255) { snprintf(note, cap, "步号太大"); return -1; }
    char mk[160];
    snprintf(mk, sizeof mk, "%s%d%s%s/%s/%s%s]", PROC_STEP_PREFIX, idx, PROC_STEP_CLAIM,
             args[1], args[2], args[3], neg ? "/不" : "");
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = sub;                                  /* 端点 = 主语词 */
    snprintf(m.name, sizeof m.name, "%s", args[1]);
    snprintf(m.source, sizeof m.source, "行为:加断言");
    m.flags = MOD_FLAG_PROCESS;
    m.rel_id = (uint8_t)idx;
    m.action = pool_token_intern(ai->pool, mk);
    if (!m.action) { snprintf(note, cap, "断言记号建不出来"); return -1; }
    m.bump = 1;
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s —%s→ %s（%s）", PROC_TOKEN_PREFIX, args[0], mk,
             args[1], ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}

/* §6.513 B5：把一个词标成引导语外壳（图里留一条 `词 —[外壳]→ [外壳:标记]` 的边）。 */
static inline int token_op_mark_shell(AI *ai, char *const *args, size_t n, ModQueue *q,
                                      char *note, size_t cap) {
    if (n < 1) { snprintf(note, cap, "要 <词>"); return -1; }
    Token *w = pool_token_intern(ai->pool, args[0]);
    if (!w) { snprintf(note, cap, "这个词建不出来"); return -1; }
    Token *mark = pool_token_intern(ai->pool, "外壳标记");
    if (!mark) { snprintf(note, cap, "标记节点建不出来"); return -1; }
    int is_q = (n >= 2 && args[1] && !strcmp(args[1], "问"));
    Token *act = is_q ? pool_token_intern(ai->pool, "[外壳:问]") : NULL;
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = w;
    m.main = mark;
    m.action = act;                                 /* NULL = 普通外壳；`[外壳:问]` = 问句框架 */
    snprintf(m.name, sizeof m.name, "%s", "外壳标记");
    snprintf(m.source, sizeof m.source, "行为:标外壳");
    m.bump = 1;
    int ok = mod_propose(q, &m);
    ++g_shell_epoch;                      /* §6.521 通知剥离侧：外壳集合变了，缓存该重建 */
    snprintf(note, cap, "%s —[外壳]→ 外壳标记%s（%s）", args[0], is_q ? "（问句框架）" : "",
             ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}

/* §6.530 目标②：**撤外壳** —— 把 `词 —[外壳]→ 外壳标记` 这条边撤掉（受审通道，和标走同一条路）。
 * 这是"外壳的唯一写法在图上"的另一半：能建就能撤，否则图只是单向的配置。
 * 三种"撤不掉"都要**明说**，不许假装成功：
 *   ① 词不在池子里；② 没有外壳标记边；③ 这个词是从数据 `[lead]` 镜像来的（`[外壳:词]` 记号还在）——
 *     那种要改数据行，不是撤销能解决的。 */
static inline int token_op_unmark_shell(AI *ai, char *const *args, size_t n, ModQueue *q,
                                        char *note, size_t cap) {
    if (n < 1) { snprintf(note, cap, "要 <词>"); return -1; }
    /* ⚠ 先查镜像记号：镜像**故意不 intern 原词**（§6.518：intern 原词会改分词），
     * 所以 `请问` 这种词在池子里根本不存在 —— 若先查"词在不在池里"，
     * 镜像来的外壳会被误报成"不是外壳"（§6.530 实测踩到）。 */
    char mk[72];
    snprintf(mk, sizeof mk, "[外壳:%s]", args[0]);
    Token *mir = pool_find(ai->pool, mk);
    if (!mir) {
        snprintf(mk, sizeof mk, "[外壳问:%s]", args[0]);
        mir = pool_find(ai->pool, mk);
    }
    if (mir) {
        snprintf(note, cap, "「%s」是数据 [lead] 镜像来的外壳（%s 还在）—— 要改 patterns.txt 的那行，撤不了",
                 args[0], mir->name);
        return 0;
    }
    Token *w = pool_find(ai->pool, args[0]);
    if (!w) { snprintf(note, cap, "「%s」不在词池里 —— 不是外壳", args[0]); return 0; }
    Token *mark = pool_find(ai->pool, "外壳标记");
    if (!mark) { snprintf(note, cap, "图里还没有任何外壳标记 —— 「%s」不是外壳", args[0]); return 0; }
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_REMOVE_ASSOC;
    m.target = w;
    m.main = mark;
    snprintf(m.name, sizeof m.name, "%s", "外壳标记");
    snprintf(m.source, sizeof m.source, "行为:撤外壳");
    int ok = mod_propose(q, &m);
    ++g_shell_epoch;                      /* §6.521 同理：外壳集合变了，剥离缓存要重建 */
    snprintf(note, cap, "%s —[外壳]→ 外壳标记 已撤（%s）", args[0], ok ? "已提出" : "没这条边");
    return ok ? 1 : 0;
}

/* §6.511 B4：加一个"说"的步骤。模板 = args[1..] 用空格连起来（`::op 加说 查 猫 属于 $1`）。 */
static inline int token_op_add_say(AI *ai, char *const *args, size_t n, ModQueue *q,
                                   char *note, size_t cap) {
    if (n < 2) { snprintf(note, cap, "要 <过程> <模板…>"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s", args[0]); return -1; }
    int idx = 0;
    for (size_t i = 0; i < root->count; ++i) {
        int sN = edge_step_index(&token_assocs(root)[i]);
        if (sN > idx) idx = sN;
    }
    idx++;
    if (idx > 255) { snprintf(note, cap, "步号太大"); return -1; }
    char tpl[128] = {0};
    size_t use = 0;
    for (size_t i = 1; i < n && i < 6; ++i) {
        if (!args[i] || !args[i][0]) continue;
        use += (size_t)snprintf(tpl + use, sizeof tpl - use, "%s%s", use ? " " : "", args[i]);
        if (use + 8 >= sizeof tpl) break;
    }
    if (!tpl[0]) { snprintf(note, cap, "模板是空的"); return -1; }
    char mk[192];
    snprintf(mk, sizeof mk, "%s%d%s%s]", PROC_STEP_PREFIX, idx, PROC_STEP_SAY, tpl);
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = pool_find(ai->pool, args[1]);           /* 端点：模板第一个词（可能没有 ⇒ 走名字端点） */
    snprintf(m.name, sizeof m.name, "%s", args[1]);
    snprintf(m.source, sizeof m.source, "行为:加说");
    m.flags = MOD_FLAG_PROCESS;
    m.rel_id = (uint8_t)idx;
    m.action = pool_token_intern(ai->pool, mk);
    if (!m.action) { snprintf(note, cap, "说记号建不出来"); return -1; }
    m.bump = 1;
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s —%s→ %s（%s）", PROC_TOKEN_PREFIX, args[0], mk, tpl,
             ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}

static inline int token_op_add_cond(AI *ai, char *const *args, size_t n, ModQueue *q,
                                    char *note, size_t cap) {
    if (n < 3) { snprintf(note, cap, "要 <过程> <条件词> <动作>"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s", args[0]); return -1; }
    char mk[96];
    snprintf(mk, sizeof mk, "%s%s]", PROC_COND_PREFIX, args[1]);
    if (micro_reserved_new_name(mk)) { snprintf(note, cap, "条件名不能用保留名"); return -1; }
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = pool_find(ai->pool, args[2]);            /* 可能还没有：走名字端点 */
    snprintf(m.name, sizeof m.name, "%s", args[2]);
    snprintf(m.source, sizeof m.source, "行为:加条件");
    m.flags = MOD_FLAG_PROCESS;      /* 记号名走 m.action（带名字的记号不占 flag 位） */
    /* 条件词自己也要在（它得能当端点、能被 `ai_premise_holds` 判） */
    m.action = pool_token_intern(ai->pool, mk);
    if (!m.action) { snprintf(note, cap, "条件名建不出来"); return -1; }
    m.bump = 1;
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s —%s→ %s（%s）", PROC_TOKEN_PREFIX, args[0], mk, args[2],
             ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}
static inline int token_op_add_else(AI *ai, char *const *args, size_t n, ModQueue *q,
                                    char *note, size_t cap) {
    return token_op_add_step(ai, args, n, q, note, cap, PROC_ELSE_MARK);
}
static inline int token_op_add_exc(AI *ai, char *const *args, size_t n, ModQueue *q,
                                   char *note, size_t cap) {
    if (n < 3) { snprintf(note, cap, "要 <过程> <类型> <动作>"); return -1; }
    Token *root = proc_find(ai, args[0]);
    if (!root) { snprintf(note, cap, "没有这个过程：%s", args[0]); return -1; }
    char mk[96];
    snprintf(mk, sizeof mk, "%s%s]", PROC_EXC_PREFIX, args[1]);
    if (micro_reserved_new_name(mk)) { snprintf(note, cap, "类型名不能用保留名"); return -1; }
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_ASSOC;
    m.target = root;
    m.main = pool_find(ai->pool, args[2]);
    snprintf(m.name, sizeof m.name, "%s", args[2]);
    snprintf(m.source, sizeof m.source, "行为:加异常");
    m.flags = MOD_FLAG_PROCESS;
    m.action = pool_token_intern(ai->pool, mk);
    if (!m.action) { snprintf(note, cap, "类型名建不出来"); return -1; }
    m.bump = 1;
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s%s —%s→ %s（%s）", PROC_TOKEN_PREFIX, args[0], mk, args[2],
             ok ? "已提出" : "与队列重复");
    return ok ? 1 : 0;
}

static inline int token_op_add_premise(AI *ai, char *const *args, size_t n, ModQueue *q,
                                       char *note, size_t cap) {
    Token *A = pool_find(ai->pool, args[0]);
    Token *B = pool_find(ai->pool, args[1]);
    if (!A || !B || A == B) {
        snprintf(note, cap, "找不到两个不同的词：%s / %s", args[0], args[1]);
        return -1;
    }
    const char *pw = (n >= 3 && args[2] && args[2][0]) ? args[2]
                                                       : MICRO_TEMP_TOKEN_PREFIX "前提]";
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_INSERT_BETWEEN;
    m.target = A; m.main = B;
    m.flags = MOD_FLAG_SPLIT_DIRECT | MOD_FLAG_MARK_PREMISE;
    m.bump = 1;
    snprintf(m.name, sizeof m.name, "%s", pw);
    snprintf(m.source, sizeof m.source, "行为:加前提");
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s —[前提]→ %s → %s（%s）",
             A->name ? A->name : "?", pw, B->name ? B->name : "?",
             ok ? "已提出" : "与队列里的重复，丢了");
    return ok ? 1 : 0;
}

/* ①b 加反前提 <A> <B> [前提名]：与「加前提」同形，但条件是**前提不在注意窗口里**
 * （旧 `subs_not` 的替代：`A —[前提不]→ P → B` 表示"只要没人提到 P，A 就能到 B"）。 */
static inline int token_op_add_premise_not(AI *ai, char *const *args, size_t n, ModQueue *q,
                                           char *note, size_t cap) {
    Token *A = pool_find(ai->pool, args[0]);
    Token *B = pool_find(ai->pool, args[1]);
    if (!A || !B || A == B) {
        snprintf(note, cap, "找不到两个不同的词：%s / %s", args[0], args[1]);
        return -1;
    }
    const char *pw = (n >= 3 && args[2] && args[2][0]) ? args[2]
                                                       : MICRO_TEMP_TOKEN_PREFIX "前提]";
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_INSERT_BETWEEN;
    m.target = A; m.main = B;
    m.flags = MOD_FLAG_SPLIT_DIRECT | MOD_FLAG_MARK_PREMISE_NOT;
    m.bump = 1;
    snprintf(m.name, sizeof m.name, "%s", pw);
    snprintf(m.source, sizeof m.source, "行为:加反前提");
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s —[前提不]→ %s → %s（%s）",
             A->name ? A->name : "?", pw, B->name ? B->name : "?",
             ok ? "已提出" : "与队列里的重复，丢了");
    return ok ? 1 : 0;
}

/* ② 前提接线 <A> <前提P> <目标Y>
 * 「让 A 经前提 P 也能到 Y」：确保 A —[前提]→ P 在（不在就先建），并加一条 P→Y。
 * 这就是原来「按语境接线」那条能力，只是**选择判据从边上的字段换成了图里的前提链接**。 */
static inline int token_op_premise_tie(AI *ai, char *const *args, size_t n, ModQueue *q,
                                       char *note, size_t cap) {
    (void)n;
    Token *A = pool_find(ai->pool, args[0]);
    Token *P = pool_find(ai->pool, args[1]);
    Token *Y = pool_find(ai->pool, args[2]);
    if (!A || !P || !Y || A == P) {
        snprintf(note, cap, "找不到词：%s / %s / %s", args[0], args[1], args[2]);
        return -1;
    }
    int pushed = 0;
    {   /* 前提链接：A —[前提]→ P（没有就先建一个标记） */
        AssocEntry *link = NULL;
        for (size_t i = 0; i < A->count; ++i)
            if (token_assocs(A)[i].main == P) { link = &token_assocs(A)[i]; break; }
        if (!link) {
            /* A —[前提]→ P：先加边，再挂标记（两步都过安全内核） */
            ModIntent e2;
            memset(&e2, 0, sizeof e2);
            e2.type = MOD_ADD_ASSOC; e2.target = A; e2.main = P; e2.bump = 1;
            snprintf(e2.source, sizeof e2.source, "行为:前提接线");
            if (mod_propose(q, &e2)) pushed++;
            ModIntent mk;
            memset(&mk, 0, sizeof mk);
            mk.type = MOD_SET_ACTION; mk.target = A; mk.main = P;
            mk.action = pool_token_intern(ai->pool, AI_PREMISE_MARK);   /* 正前提 */
            snprintf(mk.source, sizeof mk.source, "行为:前提接线");
            if (mk.action && mod_propose(q, &mk)) pushed++;
        }
    }
    {   /* 前提的出口：P → Y（普通边，联想照走） */
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_ADD_ASSOC;
        m.target = P; m.main = Y; m.bump = 1;
        snprintf(m.source, sizeof m.source, "行为:前提接线");
        if (mod_propose(q, &m)) pushed++;
    }
    snprintf(note, cap, "%s —[前提]→ %s → %s；提出 %d 条",
             A->name ? A->name : "?", P->name ? P->name : "?", Y->name ? Y->name : "?", pushed);
    return pushed;
}

/* ③ 中间插词 <A> <B> [<中转名>]
 * 语义（用户点名的那条）：**不要让 A 与 B 直接联想到** —— 在它们之间插一个符号节点，
 * 让连接经过它中转（A→N→B），并把 A—B 那条直连拆掉（拆不动就保留并说明：
 * 观测到的关系边是知识，铁律 2 不许剪）。不给名字时用临时符号节点。 */
static inline int token_op_insert_between(AI *ai, char *const *args, size_t n, ModQueue *q,
                                          char *note, size_t cap) {
    Token *A = pool_find(ai->pool, args[0]);
    Token *B = pool_find(ai->pool, args[1]);
    if (!A || !B || A == B) {
        snprintf(note, cap, "找不到两个不同的词：%s / %s", args[0], args[1]);
        return -1;
    }
    const char *nw = (n >= 3 && args[2] && args[2][0]) ? args[2] : MICRO_TEMP_TOKEN_PREFIX "中转]";
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_INSERT_BETWEEN;
    m.target = A; m.main = B;
    m.flags = MOD_FLAG_SPLIT_DIRECT;         /* 不要直连：拆掉 A—B */
    m.bump = 1;                              /* 两条中转边都算「用过一次」（别留 hits=0 的残留） */
    snprintf(m.name, sizeof m.name, "%s", nw);
    snprintf(m.source, sizeof m.source, "行为:插词");
    int ok = mod_propose(q, &m);
    snprintf(note, cap, "%s —%s→ %s（%s）", A->name ? A->name : "?", nw,
             B->name ? B->name : "?", ok ? "已提出" : "与队列里的重复，丢了");
    return ok ? 1 : 0;
}

/* ④ 回收临时：没人引用的草稿符号节点（自己没边、也没人指向它）提出来删掉。
 * 临时符号节点 **不存盘**，所以这一步只是省内存；把它做成一条行为是为了走同一条审查路径。 */
static inline int token_op_collect_temp(AI *ai, char *const *args, size_t n, ModQueue *q,
                                        char *note, size_t cap) {
    (void)args; (void)n;
    size_t found = 0, pushed = 0;
    for (size_t i = 0; i < ai->pool->count && pushed < 4; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!token_is_temp(t) || t->is_garbage) continue;
        if (t->count != 0) continue;                          /* 自己还连着谁 */
        if (micro_token_has_in_edge(ai->pool, t)) continue;    /* 还有人指向它 */
        found++;
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_REMOVE_TOKEN;
        m.target = t;
        snprintf(m.source, sizeof m.source, "行为:回收");
        if (mod_propose(q, &m)) pushed++;
    }
    snprintf(note, cap, "没人引用的临时符号节点 %zu 个；提出回收 %zu 条", found, pushed);
    return (int)pushed;
}

static const TokenOp TOKEN_OPS[] = {
    { "加前提",     "<A> <B> [前提名]", "给 A 与 B 之间加一个**前提符号节点**（默认临时 [临:前提]）：A —[前提]→ P → B",
      2, 3, token_op_add_premise },
    { "加反前提",   "<A> <B> [前提名]", "同「加前提」，但条件是**前提不在注意窗口里**（[前提不]）",
      2, 3, token_op_add_premise_not },
    { "前提接线",   "<A> <前提P> <目标Y>", "让 A 经前提 P 也能到 Y（确保 A—[前提]→P，并加 P→Y）",
      3, 3, token_op_premise_tie },
    { "中间插词",   "<A> <B> [中转名]", "在 A 与 B 之间插一个符号节点（默认临时 [临:中转]），"
      "让它们不直连、经它中转", 2, 3, token_op_insert_between },
    { "回收临时",   "", "回收没人引用的临时符号节点（草稿纸）", 0, 0, token_op_collect_temp },
    { "调用补丁",   "<补丁> <操作> [参数…]", "走受审通道调用外设补丁的操作（如 记事本 写/提交/发送）",
      2, 6, token_op_call_patch },
    /* ⚠ 下面四条是**第 9 条（过程记号）**新加的 —— 必须留在**表尾**：
     * `token_lang.h` 的 `IPOP_*` 是**按下标**引用这张表的（层与层的编号契约，
     * tools/optest.c 钉着它）。插在中间会把 `[原语:回收临时]` 指到「建过程」上（实测踩到）。 */
    { "建过程",     "<名字>", "建一个**过程根** `[过程:名字]`（内部命名空间；过程边从它出去）",
      1, 1, token_op_make_proc },
    { "加步骤",     "<过程> <动作> [序号]", "给过程加一步：`[过程:X] —[步骤:N]→ 动作`，"
      "role 标成**过程**（序号不给就自动排）", 2, 3, token_op_add_step_n },
    { "加调用",     "<过程> <动作>", "给过程加一条**调用**边（`[调用]`）：这一步是叫别人/别处",
      2, 2, token_op_add_call },
    { "加回边",     "<过程> <步号或动作> [条件词]", "给过程加一条**回边**（`[回边]`）：给步号就指回"
      "`[步骤:N]`；给了条件词就记成 `[回边:条件词]`（**每轮先看它成不成立** —— 这就是 while 循环）",
      2, 3, token_op_add_back },
    { "加条件",     "<过程> <条件词> <动作>", "⑤ 给过程加一条**条件分支**："
      "`[过程:X] —[条件:条件词]→ 动作`（条件成立才走这一步；判据就是前提那三档）。"
      "§6.507 条件词可以是**复合式**：`A&B` 与 / `A|B` 或 / `!A` 非（如 `!鱼&!猫`）；"
      "§6.508 还可用 `#N` 引用本过程第 N 步`加断言`的**复核结论**（`#1`、`#1&!鱼`）",
      3, 3, token_op_add_cond },
    { "加否则",     "<过程> <动作>", "⑤ 给过程加**另一支**（`[否则]`）：上面那条条件不成立时走它",
      2, 2, token_op_add_else },
    { "加执行",     "<过程> <动作词> [参数…]", "① 给过程加一个**真的会调动作**的步骤"
      "（动作符号节点 = `[执行:动作词 参数…]`）；跑的时候走 `action_run`（白名单+超时+账），"
      "干跑时只核过、不动外部世界", 2, 6, token_op_add_exec },
    { "加断言",     "<过程> <主语> <关系> <宾语> [不] [序号]", "① 给过程加一个**要核的步骤**"
      "（`[步骤:N@要:S/R/B]`）：跑的时候 `rel_solve` + `der_verify` **独立复核**，核不过就是断点",
      4, 6, token_op_add_claim },
    { "加异常",     "<过程> <类型> <动作>", "⑤ 给过程加**出错时跳的处理步**（`[异常:类型]`）："
      "哪一步失败就跑它（类型只作标签，v1 不按类型分派）", 3, 3, token_op_add_exc },
    /* §6.513 B5 **外壳也是图里的记号**：把某个词标成"引导语"，剥离环节会从**图**里读它。
     * §6.515 加一个可选参数 `问`：表示这是**问句框架**的引导语（剥完要补回问号）。
     * 这个信息以前只存在 `[lead]` 桶的 `?` 后缀里 —— 图化 A/B 掉 81 条就是因为丢了它。 */
    { "标外壳",     "<词> [问]", "§6.513/§6.515 把 <词> 标成**引导语外壳**（`词 —[外壳]→ 外壳标记`）；"
      "带 `问` 时再挂 `[外壳:问]` 动作（= 问句框架，剥完补回问号）",
      1, 2, token_op_mark_shell },
    /* §6.511 B4 **答复模板也是行为**：给过程加一个"说出来"的步骤（模板=后面所有参数用空格连起来） */
    { "加说",       "<过程> <模板…>", "§6.511 给过程加一步**答复模板**（`[步骤:N@说:模板]`）；"
      "模板里可用 `$1`..`$4`（`::proc run <名> <实参…>` 绑）；跑完把说过的话打在「答复：」上",
      2, 6, token_op_add_say },
    /* §6.530 目标② **撤销外壳**：外壳的唯一写法在图上 —— 能建边就能删边（同一条受审通道） */
    { "撤外壳",     "<词>", "§6.530 把 <词> 的**引导语外壳**标记撤掉（删 `词 —[外壳]→ 外壳标记`）；"
      "数据 `[lead]` 镜像来的外壳撤不了（要改数据行，会明说）",
      1, 1, token_op_unmark_shell },
    { NULL, NULL, NULL, 0, 0, NULL }
};

static inline size_t token_op_count(void) {
    size_t n = 0;
    while (TOKEN_OPS[n].name) n++;
    return n;
}

/* 跑一条行为：展开 → 逐条走「审查 → 记账 → 应用 → 审计」（和自修改同一条路）。
 * 返回应用成功的条数；-1 = 没有这个行为 / 参数不对 / 前提不成立。 */
/* 按名字找一条符号节点行为（`::op` 与过程执行那一层共用；判据只写这一处）。 */
static inline const TokenOp *token_op_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; TOKEN_OPS[i].name; ++i)
        if (strcmp(TOKEN_OPS[i].name, name) == 0) return &TOKEN_OPS[i];
    return NULL;
}

static inline int token_op_run(AI *ai, const char *name, char *const *args, size_t n,
                               char *note, size_t cap) {
    if (ai && note && cap) note[0] = 0;
    if (!ai || !name) return -1;
    const TokenOp *op = token_op_by_name(name);
    if (!op) { snprintf(note, cap, "没有这个行为：%s（::op 看清单）", name); return -1; }
    if (n < op->min_args || n > op->max_args) {
        snprintf(note, cap, "参数不对：%s %s", op->name, op->shape);
        return -1;
    }
    ModQueue q;
    memset(&q, 0, sizeof q);
    char enote[160];
    enote[0] = 0;
    int cnt = op->expand(ai, args, n, &q, enote, sizeof enote);
    if (cnt < 0) { snprintf(note, cap, "%s", enote); return -1; }
    safety_step_begin(ai);                 /* 一条命令 = 一步 */
    int applied = 0, denied = 0;
    SafeReason first_deny = SAFE_OK;
    for (size_t i = 0; i < q.n; ++i) {
        char an[96];
        SafeReason r = micro_try(ai, &q.items[i], an, sizeof an);
        if (r == SAFE_OK) applied++;
        else { denied++; if (first_deny == SAFE_OK) first_deny = r; }
    }
    if (denied)
        snprintf(note, cap, "%s；应用 %d 条，被拒 %d 条（第一条理由：%s）",
                 enote, applied, denied, SAFE_REASON_NAME[first_deny]);
    else
        snprintf(note, cap, "%s；应用 %d 条", enote, applied);
    return applied;
}

/* ============================ 元认知 ============================ */

static inline void micro_log_apply(Micro *mk, const ModIntent *m, const char *note) {
    if (!mk) return;
    size_t i = mk->meta.apply_n % MICRO_APPLY_LOG_MAX;
    snprintf(mk->meta.apply_log[i], sizeof mk->meta.apply_log[i], "%s %s [%s]",
             micro_mod_type_name(m->type), note ? note : "",
             m->source[0] ? m->source : "?");
    mk->meta.apply_n++;
}

static inline void micro_metacog(AI *ai) {
    Micro *mk = micro_of(ai);
    if (!mk) return;
    MicroMeta *mm = &mk->meta;

    /* ---- 速率窗口 ---- */
    size_t applied_now = mm->n_applied - mm->win_applied;
    mm->win_applied = mm->n_applied;
    mm->win[mm->win_i] = applied_now;
    mm->win_i = (mm->win_i + 1) % MICRO_WINDOW;
    if (mm->win_filled < MICRO_WINDOW) mm->win_filled++;
    size_t sum = 0;
    for (size_t i = 0; i < mm->win_filled; ++i) sum += mm->win[i];
    mm->last_rate = mm->win_filled ? (float)sum / (float)mm->win_filled : 0.0f;

    /* ---- 停止条件①：池子总量触顶 ---- */
    if (safety_check_stop(&mk->safe, ai->pool)) {
        micro_remind(ai, MICRO_REMIND_TOTAL, NULL);
        micro_remind(ai, MICRO_REMIND_FROZEN, NULL);
    }

    /* ---- 停止条件②：窗口内**每步都把建边预算用满** → 冻结 ----
     * 阈值刻意高于检测器上限（D1 每步最多 3 条，预算 8 条），
     * 所以正常负载永远不会碰到它；它拦的是补丁狂推意图这类失控。 */
    if (!mk->safe.freeze && mm->win_filled == MICRO_WINDOW &&
        mm->last_rate >= (float)mk->safe.max_new_edges_step) {
        mk->safe.freeze = 1;
        mk->safe.n_freeze++;
        micro_remind(ai, MICRO_REMIND_OVERHEAT, NULL);
        micro_remind(ai, MICRO_REMIND_FROZEN, NULL);
    }

    /* ---- 自省：提案几乎全被否 → 自己把提案关小 ----
     * 安全内核在干活，但「一直在提垃圾」本身也是问题。
     * 关小是有期限的：propose_off 倒数到 0 就恢复（元认知不是一次踩死）。 */
    if (mm->win_filled == MICRO_WINDOW && mm->denied_window >= 32 &&
        mm->denied_window > mm->win_filled * 4) {
        if (mk->propose_off == 0) {
            mk->propose_off = 10;
            mm->n_throttles++;
            if (!mk->coact_off) { mk->coact_off = 1; mk->coact_off_by_meta = 1; }
            micro_remind(ai, MICRO_REMIND_DENIED_MOST, NULL);
            micro_remind(ai, MICRO_REMIND_THROTTLE, NULL);
        }
    }
    if (mk->propose_off == 0 && mk->coact_off_by_meta) {
        mk->coact_off = 0;
        mk->coact_off_by_meta = 0;
    }
    if (mm->n_steps % MICRO_WINDOW == 0) mm->denied_window = 0;

    /* ---- 补丁健康：挂着却一直不输出 = 沉默 ---- */
    for (size_t i = 0; i < mk->patches.n; ++i) {
        Patch *p = mk->patches.items[i];
        if (!p || !p->mounted || !p->enabled) continue;
        if (!(p->kind & PATCH_SENSE)) continue;
        if (ai->pool->current_tick > p->last_out_tick + MICRO_PATCH_SILENT)
            micro_remind(ai, MICRO_REMIND_PATCH_SILENT, p->name);
    }
}

/* ============================ 安全点（一轮的收尾） ============================ */

/* 安全点：逐条「审查 → 记账 → 应用 → 审计」。**唯一一份** ——
 * micro_step（每轮）与 action.h 的"一次动作落一个观测标记"都走它：
 * 入口不同但口径必须一样（被拒的不占预算、应用失败要退款、末尾清队列）。
 * 返回真正应用了几条。 */
static inline size_t micro_drain_queue(AI *ai) {
    if (!ai) return 0;
    Micro *mk = micro_of(ai);
    if (!mk) return 0;
    ModQueue *q = &mk->queue;
    size_t applied = 0;
    for (size_t i = 0; i < q->n; ++i) {
        ModIntent *m = &q->items[i];
        if (applied >= MICRO_APPLY_MAX_STEP) { mk->meta.n_dropped++; continue; }
        SafeReason r = safety_review(&mk->safe, ai->pool, m);
        if (r != SAFE_OK) {
            mk->safe.n_denied++;
            mk->safe.deny_by_reason[r]++;
            mk->meta.n_denied++;
            mk->meta.denied_window++;
            safety_audit(&mk->safe, ai->pool->current_tick, m, 0, r);
            continue;
        }
        char note[96];
        safety_charge(&mk->safe, ai->pool, m);         /* 先记账，再应用（见 safety_charge） */
        int rc = micro_apply_one(ai, m, note, sizeof note);
        if (rc < 0) {
            safety_refund(&mk->safe, ai->pool, m);
            mk->safe.n_denied++;
            mk->safe.deny_by_reason[SAFE_DENY_BADREF]++;
            mk->meta.n_denied++;
            mk->meta.denied_window++;
            safety_audit(&mk->safe, ai->pool->current_tick, m, 0, SAFE_DENY_BADREF);
            continue;
        }
        safety_audit(&mk->safe, ai->pool->current_tick, m, 1, SAFE_OK);
        mk->meta.n_applied++;
        ai->writes[AI_WSRC_MICRO]++;          /* 统一写入通道：受审落地的那些也记账 */
        applied++;
        switch (m->type) {
        case MOD_ADD_TOKEN:    mk->meta.n_new_tokens++; break;
        case MOD_ADD_ASSOC:    if (rc == 1) mk->meta.n_new_edges++; break;
        case MOD_TAG_DERIVED:  if (rc == 1) mk->meta.n_new_edges++;
                               mk->meta.n_derived++; break;
        case MOD_REMOVE_ASSOC: mk->meta.n_removed++; break;
        case MOD_BUMP_HITS:    mk->meta.n_bumped++; break;
        case MOD_TAG_REL:      mk->meta.n_rel++; break;
        case MOD_INSERT_BETWEEN:
            if (rc >= 1) mk->meta.n_new_edges += 2;
            if (rc == 2) mk->meta.n_new_tokens++;
            if (strcmp(m->source, "条件依赖") == 0) mk->meta.n_cond_applied++;
            break;
        case MOD_REMOVE_TOKEN: mk->meta.n_removed++; break;
        default: break;
        }
        micro_log_apply(mk, m, note);
    }
    q->n = 0;   /* 意图不跨轮：要么在这一步应用，要么被拒 */
    return applied;
}

/* 一轮的**唯一**结构修改入口。放在输出成句之后 ——
 * 此时遍历早就结束了（不在任何 for (i < t->count) 里），
 * 加边/加符号节点不会破坏任何正在进行的迭代。 */
static inline void micro_step(AI *ai, char *const *words, size_t n, const char *out) {
    if (!ai) return;
    Micro *mk = micro_of(ai);
    if (!mk) return;
    ModQueue *q = &mk->queue;
    /* 刻意**不**在这里清队列：思维层（think.h）和补丁都在这一轮里更早的时候
     * 提议，安全点只负责消费。末尾 q->n = 0 保证意图不跨轮。 */
    mk->safe.step_new_tokens = 0;
    mk->safe.step_new_edges = 0;
    mk->safe.step_patch_calls = 0;
    /* 测量钩子：`AIP_PATCH=记事本` 自动挂一个内建补丁（只给 A/B 用，不进业务逻辑）。
     * 用它跑整包回归，回答的是"挂上这个外设到底改不改既有行为"。 */
    if (!mk->meta.patch_auto_tried) {
        mk->meta.patch_auto_tried = 1;
        const char *pn = getenv("AIP_PATCH");
        if (pn && pn[0]) (void)patch_mount_builtin(ai, pn);
    }
    mk->meta.n_steps++;
    if (mk->propose_off > 0) mk->propose_off--;

    /* ① 补丁提议（内核不知道补丁内部怎么想） */
    patch_propose_all(ai, q);

    /* ② 内建检测器：从图里量出「该改什么」 */
    if (!mk->safe.mod_off && mk->propose_off == 0) {
        micro_detect_coactivation(ai, words, n, mk);
        micro_detect_conditional(ai, words, n, mk);   /* D4：从反复出现的谈话里长前提 */
        micro_detect_gap(ai, words, n, out, mk);
        micro_detect_prune(ai, mk);
    }

    /* ③ 安全点：逐条「审查 → 记账 → 应用 → 审计」。**唯一一份**实现 ——
     * micro_step（每轮）与 action.h 的"一次动作落一个观测标记"都调它，
     * 这样"受审的写入"口径不会因为入口不同而分叉。 */
    size_t proposed = q->n;
    (void)micro_drain_queue(ai);
    mk->meta.n_proposed += proposed;

    /* ④ 元认知：看速率 / 拒绝率 / 补丁健康，必要时自己踩刹车 */
    micro_metacog(ai);

    /* ⑤ 补丁的 update（补丁自己的内部状态，内核不管） */
    patch_update_all(ai, 0.016f);
}

/* ---- 补丁注入 / 取出：内核与补丁的**唯一**两个接触点 ---- */

/* 激活前：把补丁给的符号节点放进这一轮的上下文（焦点 + 时刻）。
 * 没有补丁时这里是空转 —— 「核心不依赖补丁」不是口号，是这段代码。 */
static inline size_t micro_patch_sense(AI *ai) {
    if (!ai) return 0;
    Micro *mk = micro_of(ai);
    if (!mk || mk->patches.n == 0) return 0;
    PatchOut out[64];
    size_t n = patch_sense_all(ai, out, 64);
    if (n == 0) return 0;
    char tname[64];
    snprintf(tname, sizeof tname, "[时刻:%llu]",
             (unsigned long long)ai->pool->current_tick);
    Token *now = pool_token_intern(ai->pool, tname);
    for (size_t i = 0; i < n; ++i) {
        Token *t = out[i].tok;
        if (!t) continue;
        ai_focus_push(ai, t, out[i].w > 0 ? out[i].w : 1.0f);
        if (now) token_add_assoc_unique(now, t);
    }
    return n;
}

/* 激活后：把这一轮说出去的话交给动作补丁。
 * 内核不知道「日志」「音箱」「机械臂」是什么 —— 它只给符号节点。 */
static inline size_t micro_patch_act(AI *ai, const char *out) {
    if (!ai || !out || !out[0]) return 0;
    Micro *mk = micro_of(ai);
    if (!mk || mk->patches.n == 0) return 0;
    size_t n = 0;
    char **words = ai_tokenize(ai, out, &n);
    if (!words || n == 0) { ai_free_words(words, n); return 0; }
    Token **toks = (Token **)malloc(n * sizeof *toks);
    if (!toks) { ai_free_words(words, n); return 0; }
    for (size_t i = 0; i < n; ++i) toks[i] = pool_token_intern(ai->pool, words[i]);
    patch_act_all(ai, toks, n);
    free(toks);
    ai_free_words(words, n);
    return n;
}

/* ============================ 对话触发：自然语言 → 符号节点行为 ============================
 *
 * 用户要的「对话触发」：`猫 和 鱼 之间 加 个 词` 这种话要真的能干活，
 * 而不是只能手打 `::op 中间插词 猫 鱼`。
 *
 * **窄集 + 表驱动**（不是写死的 if-else 森林）：四张关键词表各只有一处，
 * 新增一种说法 = 往对应的表里加一个词；新增一类行为 = 在 `DIALOG_TRIGGERS[]` 里加一行。
 * **宁可漏，不可误触发**：必须同时命中「之间 + 动词 + 种类词」才动手，
 * 光说「猫 和 鱼 之间 有 什么 关系」不会触发（那是提问，不是命令）。
 *
 * 触发的动作**照样走受审通道**（token_op_run → 审查/预算/审计）——
 * 自然语言只是"入口"，不是"特权"。 */
typedef struct {
    const char *name;        /* 触发的符号节点行为（TOKEN_OPS[] 里的名字） */
    const char *kind;        /* 种类词（"词" / "前提" / "反前提" / "草稿"） */
    const char *help;        /* 一句话说明（::triggers 里列出来） */
} DialogTriggerRow;

/* 四张关键词表：**各只有一处**（改说法只改这里） */
static const char *const DIALOG_CONNECTIVES[] = { "和", "跟", "与", "同", "、", NULL };
/* 动词多收几个说法：加/插/添/塞/放 都是"往中间放一个"；连/接/搭 是"接上" */
static const char *const DIALOG_VERBS[]       = { "加", "插", "添", "塞", "放",
                                                  "连", "接", "搭", NULL };
static const char *const DIALOG_KIND_WORD[]   = { "词", "词儿", "词儿", "token", "符号节点", NULL };
static const char *const DIALOG_KIND_PREM[]   = { "前提", "条件", "前题", NULL };
static const char *const DIALOG_NEG[]          = { "反", "不", "别", NULL };
static const char *const DIALOG_RECYCLE_ACT[] = { "回收", "清理", "收", "清", NULL };
static const char *const DIALOG_RECYCLE_OBJ[] = { "草稿", "草稿纸", "临时", "临时词", "临时符号节点", NULL };

/* 词的"连写匹配"：**分词器会把词切开**（实测「前提」被切成「前」+「提」），
 * 所以候选词要么一个符号节点就够，要么两个符号节点拼起来正好是它。
 * 返回用掉几个符号节点（0 = 不匹配）。 */
static inline size_t dialog_match_word(char *const *words, size_t n, size_t t,
                                       const char *want) {
    if (!words || !want || t >= n) return 0;
    if (!strcmp(words[t], want)) return 1;
    if (t + 1 < n) {
        char j[64];
        snprintf(j, sizeof j, "%s%s", words[t], words[t + 1]);
        if (!strcmp(j, want)) return 2;
    }
    return 0;
}

static inline int dialog_str_in(const char *const *tab, const char *w) {
    if (!tab || !w) return 0;
    for (size_t i = 0; tab[i]; ++i) if (strcmp(tab[i], w) == 0) return 1;
    return 0;
}

static inline int dialog_trigger_try_kw(AI *ai, char *const *words, size_t n,
                                        char *out, size_t out_cap) {
    if (!ai || !words || n == 0 || out_cap == 0) return 0;
    out[0] = 0;
    /* ① 「回收草稿」这类：动作词 + 对象词都在就行（顺序无关） */
    {
        int act = -1, obj = -1;
        for (size_t i = 0; i < n; ++i) {
            if (act < 0 && dialog_str_in(DIALOG_RECYCLE_ACT, words[i])) act = (int)i;
            if (obj < 0 && dialog_str_in(DIALOG_RECYCLE_OBJ, words[i])) obj = (int)i;
        }
        if (act >= 0 && obj >= 0) {
            char note[200];
            int got = token_op_run(ai, "回收临时", NULL, 0, note, sizeof note);
            snprintf(out, out_cap, "好 ， 照做 了 ： %s", note);
            return got >= 0 ? 1 : 1;
        }
    }
    /* ② 「A 和 B 之间 <动词> …」：锚点在「之间/中间」，动词必须在它后面 */
    size_t i = n;
    for (size_t k = 0; k < n; ++k)
        if (!strcmp(words[k], "之间") || !strcmp(words[k], "中间")) { i = k; break; }
    if (i == n || i < 2) return 0;
    /* 取出 A、B。两种说法：
     *   `A 和 B 之间`（连接词在 之间 前面第二位）→ A=words[i-3], B=words[i-1]
     *   `A B 之间`    （没有连接词）             → A=words[i-2], B=words[i-1]
     * 前面的"把/给"之类自然被忽略。 */
    const char *A = NULL, *B = NULL;
    if (i >= 3 && dialog_str_in(DIALOG_CONNECTIVES, words[i - 2])) {
        A = words[i - 3]; B = words[i - 1];
    } else {
        A = words[i - 2]; B = words[i - 1];
    }
    if (!A || !B || !A[0] || !B[0] || A[0] == '[' || B[0] == '[') return 0;
    /* 动词：紧跟「之间」，允许中间夹一个"再/又/就/给" */
    size_t v = i + 1;
    if (v < n && !dialog_str_in(DIALOG_VERBS, words[v])) {
        if (v + 1 < n && dialog_str_in(DIALOG_VERBS, words[v + 1])) v = v + 1;
        else return 0;                       /* 没有动词 → 不触发（提问不是命令） */
    }
    if (v >= n) return 0;
    /* 种类词：跳过 个/一个/一 等量词 */
    size_t t = v + 1;
    while (t < n && (!strcmp(words[t], "个") || !strcmp(words[t], "一个") ||
                     !strcmp(words[t], "一"))) t++;
    int kind = 0;                             /* 1 = 插词，2 = 前提，3 = 反前提 */
    size_t after = t;
    size_t used = 0;
    /* 种类词：**整张表**都要试（以前只试表里第一个，于是「条件」这种同义说法漏掉），
     * 而且要用连写匹配（分词器会把「前提/词儿」切开）。 */
    size_t kinds = 0;
    for (size_t ki = 0; DIALOG_KIND_PREM[ki]; ++ki) kinds++;
    if (t < n && dialog_str_in(DIALOG_NEG, words[t])) {
        for (size_t ki = 0; ki < kinds && kind == 0; ++ki) {
            used = dialog_match_word(words, n, t + 1, DIALOG_KIND_PREM[ki]);
            if (used > 0) { kind = 3; after = t + 1 + used; }     /* 「反 前提/条件」 */
        }
    }
    if (kind == 0) {
        for (size_t ki = 0; ki < kinds && kind == 0; ++ki) {
            used = dialog_match_word(words, n, t, DIALOG_KIND_PREM[ki]);
            if (used > 0) { kind = 2; after = t + used; }         /* 「前提/条件」 */
        }
    }
    if (kind == 0) {
        for (size_t ki = 0; DIALOG_KIND_WORD[ki] && kind == 0; ++ki) {
            used = dialog_match_word(words, n, t, DIALOG_KIND_WORD[ki]);
            if (used > 0) {
                kind = 1; after = t + used;
                /* 被切开的「词儿」：尾巴「儿」是种类词的一部分，不是名字 */
                if (after < n && !strcmp(words[after], "儿")) after++;
            }
        }
    }
    if (kind == 0) {
        for (size_t ki = 0; DIALOG_KIND_WORD[ki] && kind == 0; ++ki) {
            used = dialog_match_word(words, n, t + 1, DIALOG_KIND_WORD[ki]);
            if (used > 0) { kind = 1; after = t + 1 + used; }
        }
    }
    if (kind == 0) kind = 1;                  /* 只说了「之间加」→ 默认插词 */
    const char *P = (after < n && words[after][0] && words[after][0] != '[' &&
                     strcmp(words[after], "吧") && strcmp(words[after], "。"))
                        ? words[after] : NULL;
    char note[220];
    int got;
    if (kind == 1) {
        char *args[3] = { (char *)A, (char *)B, (char *)P };
        got = token_op_run(ai, "中间插词", args, P ? 3 : 2, note, sizeof note);
    } else if (kind == 2) {
        char *args[3] = { (char *)A, (char *)B, (char *)P };
        got = token_op_run(ai, "加前提", args, P ? 3 : 2, note, sizeof note);
    } else {
        char *args[3] = { (char *)A, (char *)B, (char *)P };
        got = token_op_run(ai, "加反前提", args, P ? 3 : 2, note, sizeof note);
    }
    /* 有界拼接（note 可能比 out 长；截断是故意的，别让 -Wformat-truncation 拦下） */
    {
        const char *head = (got < 0) ? "这句 我 听 懂 了 ， 可 没 做 成 ： " : "好 ， 照做 了 ： ";
        size_t off = 0;
        for (const char *q = head; *q && off + 1 < out_cap; ++q) out[off++] = *q;
        for (const char *q = note; *q && off + 1 < out_cap; ++q) out[off++] = *q;
        out[off] = 0;
    }
    return 1;
}

/* ============================ 对话触发：**说法归一表 + 槽位**（新匹配器） ============================
 *
 * 为什么换（老匹配器靠**位置算术**，实测 25/33，量法见 tests/meas_trigger.sh）：
 *   · 漏说法：「当中」「堆」「搁」这些同义说法直接漏；「给 我 加 个 词」漏（两位填充）；
 *     「加 个 词 在 猫 和 鱼 之间」这种**动词在前**的语序漏；
 *   · 填错槽：「加 上 个 词」把"上"当成词名插进去；
 *   · **误触发**：「别 加 词」「别 塞 个 条件」这种**否定命令**反而照做（最不能接受的一类）。
 *
 * 新匹配器分两步，还是"表驱动"，但表换成**一张说法归一表**（词 → 槽位类别）：
 *   ① 归一：每个词查 `DIALOG_WORDS[]` → 类别（连接词/锚点/动词/种类词/否定/问句/填充…）；
 *   ② 填槽：锚点（之间/中间/当中）把话切成"操作数"和"指令"两段，各自扫槽：
 *        操作数 = 类别为空的实词（恰好两个）；动词/种类词/否定**不看位置**，只看有没有；
 *        名字   = 种类词之后的第一个实词（`吧/了/上/个` 这类填充与量词不算）。
 * 动手的硬条件（**宁可漏，不可误触发**）：有锚点 + 恰好两个操作数 + 一个动词 +
 *        没有否定命令（别/不要/不…）+ 没有问句标志（什么/吗/呢…）。
 * 加说法只加 `DIALOG_WORDS[]` 一行；匹配逻辑只有这一份。 */
typedef enum {
    DWC_NONE = 0,     /* 实词（可能是操作数，也可能是名字） */
    DWC_CONN,         /* 和/跟/与/以及… */
    DWC_ANCHOR,       /* 之间/中间/当中 */
    DWC_V_INSERT,     /* 加/插/添/塞/放/搁/堆 */
    DWC_V_CONNECT,    /* 连/接/搭（动作同插词：往中间接一个） */
    DWC_KIND_WORD,    /* 词/词儿/词元/符号节点 */
    DWC_KIND_PREM,    /* 前提/条件/前题/前提词/条件词 */
    DWC_NEG_KIND,     /* 反/非（修饰种类词 → 反前提） */
    DWC_NEG_CMD,      /* 别/甭/莫/不要/不用/不（**否定命令 → 不动手**） */
    DWC_QUESTION,     /* 什么/吗/呢/怎么/为什么/哪（**问句 → 不动手**） */
    DWC_SKIP,         /* 把/在/给/我/请/一下/个/上/吧/了…（填充、量词、动词词尾） */
    DWC_DRAFT,        /* 草稿/草稿纸/临时…（回收的对象） */
    DWC_RECYCLE,      /* 回收/清理/收/清（回收的动作） */
} DialogWordClass;

typedef struct { const char *w; uint8_t cls; } DialogWordRow;

static const DialogWordRow DIALOG_WORDS[] = {
    { "和", DWC_CONN }, { "跟", DWC_CONN }, { "与", DWC_CONN }, { "同", DWC_CONN },
    { "、", DWC_CONN }, { "以及", DWC_CONN }, { "还有", DWC_CONN },
    { "之间", DWC_ANCHOR }, { "中间", DWC_ANCHOR }, { "当中", DWC_ANCHOR },
    { "加", DWC_V_INSERT }, { "插", DWC_V_INSERT }, { "添", DWC_V_INSERT },
    { "塞", DWC_V_INSERT }, { "放", DWC_V_INSERT }, { "搁", DWC_V_INSERT },
    { "堆", DWC_V_INSERT },
    { "连", DWC_V_CONNECT }, { "接", DWC_V_CONNECT }, { "搭", DWC_V_CONNECT },
    { "词", DWC_KIND_WORD }, { "词儿", DWC_KIND_WORD }, { "词元", DWC_KIND_WORD },
    { "符号节点", DWC_KIND_WORD }, { "token", DWC_KIND_WORD },   /* 老说法照收，不破坏已有脚本 */
    { "前提", DWC_KIND_PREM }, { "条件", DWC_KIND_PREM }, { "前题", DWC_KIND_PREM },
    { "前提词", DWC_KIND_PREM }, { "条件词", DWC_KIND_PREM },
    { "反", DWC_NEG_KIND }, { "非", DWC_NEG_KIND },
    { "别", DWC_NEG_CMD }, { "甭", DWC_NEG_CMD }, { "莫", DWC_NEG_CMD },
    { "不要", DWC_NEG_CMD }, { "不用", DWC_NEG_CMD }, { "不", DWC_NEG_CMD },
    { "什么", DWC_QUESTION }, { "吗", DWC_QUESTION }, { "呢", DWC_QUESTION },
    { "怎么", DWC_QUESTION }, { "为什么", DWC_QUESTION }, { "哪", DWC_QUESTION },
    { "把", DWC_SKIP }, { "在", DWC_SKIP }, { "给", DWC_SKIP }, { "我", DWC_SKIP },
    { "你", DWC_SKIP }, { "请", DWC_SKIP }, { "帮", DWC_SKIP }, { "一下", DWC_SKIP },
    { "个", DWC_SKIP }, { "一个", DWC_SKIP }, { "一", DWC_SKIP },
    { "再", DWC_SKIP }, { "又", DWC_SKIP }, { "就", DWC_SKIP }, { "先", DWC_SKIP },
    { "然后", DWC_SKIP },
    /* 动词词尾/补语：`加 上 个 词` 里的"上"是词尾，**不是名字**（老匹配器在这里插了个叫"上"的词） */
    { "上", DWC_SKIP }, { "下", DWC_SKIP }, { "进", DWC_SKIP }, { "出", DWC_SKIP },
    { "好", DWC_SKIP }, { "吧", DWC_SKIP }, { "了", DWC_SKIP },
    { "这", DWC_SKIP }, { "那", DWC_SKIP },
    { "回收", DWC_RECYCLE }, { "清理", DWC_RECYCLE }, { "收", DWC_RECYCLE },
    { "清", DWC_RECYCLE },
    { "草稿", DWC_DRAFT }, { "草稿纸", DWC_DRAFT }, { "临时", DWC_DRAFT },
    { "临时词", DWC_DRAFT }, { "临时符号节点", DWC_DRAFT },
    { NULL, DWC_NONE }
};

static inline uint8_t dialog_class_of(const char *w) {
    if (!w) return DWC_NONE;
    for (size_t i = 0; DIALOG_WORDS[i].w; ++i)
        if (!strcmp(DIALOG_WORDS[i].w, w)) return DIALOG_WORDS[i].cls;
    return DWC_NONE;
}

/* 在位置 i 上归一（连写匹配：分词器会把「前提/词儿」切开）→ 类别；*used = 吃掉几个符号节点 */
static inline uint8_t dialog_class_at(char *const *words, size_t n, size_t i, size_t *used) {
    if (used) *used = 0;
    if (!words || i >= n) return DWC_NONE;
    uint8_t c = dialog_class_of(words[i]);
    if (c != DWC_NONE) { if (used) *used = 1; return c; }
    if (i + 1 < n) {
        char j[64];
        snprintf(j, sizeof j, "%s%s", words[i], words[i + 1]);
        c = dialog_class_of(j);
        if (c != DWC_NONE) { if (used) *used = 2; return c; }
    }
    return DWC_NONE;
}

#define DIALOG_MAX_WORDS 64      /* 触发只看短句；超了按前 64 个词算（有界） */

static inline int dialog_trigger_try_norm(AI *ai, char *const *words, size_t n,
                                          char *out, size_t out_cap) {
    if (!ai || !words || n == 0 || out_cap == 0) return 0;
    out[0] = 0;
    size_t m = (n < DIALOG_MAX_WORDS) ? n : DIALOG_MAX_WORDS;
    uint8_t cls[DIALOG_MAX_WORDS];
    for (size_t i = 0; i < m; ++i) cls[i] = DWC_NONE;
    /* ① 归一：一次扫过去，连写的第二个符号节点标成"已被吃掉"（用 255 表示） */
    for (size_t i = 0; i < m;) {
        size_t u = 0;
        uint8_t c = dialog_class_at(words, m, i, &u);
        cls[i] = c;
        if (u == 2) cls[i + 1] = 255;
        i += (u == 0) ? 1 : u;
    }
    /* ② 回收草稿：动作 + 对象都出现就动手（与锚点无关） */
    {
        int act = 0, obj = 0;
        for (size_t i = 0; i < m; ++i) {
            if (cls[i] == DWC_RECYCLE) act = 1;
            if (cls[i] == DWC_DRAFT)   obj = 1;
        }
        if (act && obj) {
            char note[200];
            int got = token_op_run(ai, "回收临时", NULL, 0, note, sizeof note);
            snprintf(out, out_cap, "好 ， 照做 了 ： %s", note);
            return got >= 0 ? 1 : 1;
        }
    }
    /* ③ 硬条件：有锚点 / 没有否定命令 / 没有问句标志 / 有动词 */
    int has_negcmd = 0, has_question = 0, has_verb = 0, has_prem = 0, has_word = 0;
    size_t anchor_idx = m, kind_end = m;
    for (size_t i = 0; i < m; ++i) {
        if (cls[i] == DWC_ANCHOR && anchor_idx == m) anchor_idx = i;
        if (cls[i] == DWC_NEG_CMD)   has_negcmd = 1;
        if (cls[i] == DWC_QUESTION)  has_question = 1;
        if (cls[i] == DWC_V_INSERT || cls[i] == DWC_V_CONNECT) has_verb = 1;
        if (cls[i] == DWC_KIND_PREM) has_prem = 1;
        if (cls[i] == DWC_KIND_WORD) has_word = 1;
    }
    if (anchor_idx == m || has_negcmd || has_question || !has_verb) return 0;
    if (has_prem || has_word) {
        for (size_t i = 0; i < m; ++i)
            if (cls[i] == DWC_KIND_PREM || cls[i] == DWC_KIND_WORD) {
                kind_end = i + 1;
                /* 被切开的「词儿」：尾巴「儿」是**种类词的一部分**，不是名字。
                 * （老匹配器有这条；重写时漏了，被 tokenop 的既有断言当场抓住 ——
                 *   症状：`搭 个 词儿` 插进去一个叫「儿」的词。） */
                if (kind_end < m && words[kind_end] && !strcmp(words[kind_end], "儿")) kind_end++;
                break;
            }
    }
    /* ④ 填槽：**以锚点为界**（这是唯一稳的判据）——
     *   操作数 = 锚点**之前**的实词，恰好两个（`A 和 B 之间`／`A B 之间`／`把 A 和 B 之间`）；
     *   名字   = 锚点**之后**、且在种类词之后的第一个实词
     *            （`…之间 加 个 词 月亮` → 月亮；`加 个 词 在 A 和 B 之间` → 没有名字）。
     *   第一版把"种类词之后"当名字，结果动词在前的语序里把操作数 A 当成名字吃掉了（实测 32/33）。 */
    const char *A = NULL, *B = NULL;
    int n_operand = 0;
    for (size_t i = 0; i < anchor_idx; ++i) {
        if (cls[i] != DWC_NONE) continue;    /* 实词才考虑 */
        if (!words[i] || !words[i][0] || words[i][0] == '[') continue;
        if (n_operand == 0) A = words[i];
        else if (n_operand == 1) B = words[i];
        n_operand++;
    }
    if (n_operand != 2 || !A || !B) return 0;                  /* 操作数不是两个 → 不动手 */
    const char *P = NULL;
    {
        size_t from = kind_end > anchor_idx + 1 ? kind_end : anchor_idx + 1;
        for (size_t i = from; i < m; ++i) {
            if (cls[i] != DWC_NONE) continue;
            if (!words[i] || !words[i][0] || words[i][0] == '[') continue;
            P = words[i];
            break;
        }
    }
    /* 种类词：前提 / 反前提（否定词紧挨在种类词前）/ 插词 */
    int kind = 1;
    if (has_prem) {
        kind = 2;
        for (size_t i = 1; i < m; ++i)
            if (cls[i] == DWC_KIND_PREM && (cls[i - 1] == DWC_NEG_KIND || cls[i - 1] == DWC_NEG_CMD))
                kind = 3;
    }
    char note[220];
    int got;
    char *args[3] = { (char *)A, (char *)B, (char *)P };
    int argc_ = P ? 3 : 2;
    if (kind == 1)      got = token_op_run(ai, "中间插词", args, argc_, note, sizeof note);
    else if (kind == 2) got = token_op_run(ai, "加前提",   args, argc_, note, sizeof note);
    else                got = token_op_run(ai, "加反前提", args, argc_, note, sizeof note);
    {
        const char *head = (got < 0) ? "这句 我 听 懂 了 ， 可 没 做 成 ： " : "好 ， 照做 了 ： ";
        size_t off = 0;
        for (const char *q = head; *q && off + 1 < out_cap; ++q) out[off++] = *q;
        for (const char *q = note; *q && off + 1 < out_cap; ++q) out[off++] = *q;
        out[off] = 0;
    }
    return 1;
}

/* 分派：默认老匹配器；开关打开走"说法归一表 + 槽位"（先 A/B，绿了再翻默认） */
static inline int dialog_trigger_try(AI *ai, char *const *words, size_t n,
                                     char *out, size_t out_cap) {
    if (ai && ai->trigger_norm) return dialog_trigger_try_norm(ai, words, n, out, out_cap);
    return dialog_trigger_try_kw(ai, words, n, out, out_cap);
}

/* `::triggers` 是**无参**的（命令表里如此），但报告要说清当前用哪个匹配器 ——
 * 用一个进程内的"最近一次看到的开关状态"（只影响显示，不影响行为）。 */
static int g_dialog_norm_shown = 0;
static inline int ai_trigger_norm_on(void) { return g_dialog_norm_shown; }
static inline void dialog_trigger_norm_note(int on) { g_dialog_norm_shown = on; }

static inline void dialog_trigger_list(void) {
    printf("对话触发（自然语言 → 符号节点行为）：\n");
    printf("  <A> 和 <B> 之间 加|插 个 词 [<名字>]     → ::op 中间插词\n");
    printf("  <A> 和 <B> 之间 加|插 个 前提 [<前提>]   → ::op 加前提\n");
    printf("  <A> 和 <B> 之间 加|插 个 反 前提 [<前提>] → ::op 加反前提\n");
    printf("  回收|清理 草稿|临时                      → ::op 回收临时\n");
    printf("  连接词：和/跟/与/同/、/以及；动词：加/插/添/塞/放/搁/堆/连/接/搭\n");
    printf("  **没动词不触发**：`猫 和 鱼 之间 有 什么 关系` 是提问，不是命令\n");
    printf("  匹配器 = %s（`::triggers norm on|off` 切；AIP_TRIGGER=norm|kw）\n",
           ai_trigger_norm_on() ? "说法归一表 + 槽位（新）" : "关键词表 + 位置算术（老）");
    if (ai_trigger_norm_on()) {
        printf("  新匹配器额外认：当中/堆/搁；动词在前的语序（`加 个 词 在 X 和 Y 之间`）；\n");
        printf("  任意位置的填充词（`给 我 加 个 词`）；动词词尾不当名字（`加 上 个 词`）；\n");
        printf("  **否定命令不动手**（`别 加 词`）；问句不动手（什么/吗/呢…）\n");
    }
}

/* ============================ 报告（::mod / ::safe / ::patch） ============================ */

static inline void micro_report(const AI *ai) {
    if (!ai || !ai->micro) {
        printf("[微内核] 还没起来（跑过一轮对话才有状态）\n");
        return;
    }
    const Micro *mk = (const Micro *)ai->micro;
    const MicroMeta *mm = &mk->meta;
    printf("[微内核] 自修改通道=%s  检测器：共现固化=%s 条件依赖=%s(提%zu/成%zu) 缺口命名=%s 休眠剪枝=%s\n",
           mk->safe.mod_off ? "关" : "开",
           mk->coact_off ? "关" : "开",
           mk->meta.cond_off ? "关" : "开",
           mk->meta.n_cond_proposed, mk->meta.n_cond_applied,
           mk->gap_off ? "关" : "开",
           mk->prune_off ? "关" : "开");
    printf("         安全点走过 %zu 轮：提了 %zu 条（去重 %zu / 队列溢出 %zu）\n",
           mm->n_steps, mm->n_proposed, mm->n_dedup, mm->n_dropped);
    printf("         应用 %zu（新边 %zu 新符号节点 %zu 打关系 %zu 沉淀 %zu 强化 %zu 删边 %zu）  被拒 %zu\n",
           mm->n_applied, mm->n_new_edges, mm->n_new_tokens, mm->n_rel,
           mm->n_derived, mm->n_bumped, mm->n_removed, mm->n_denied);
    printf("         最近 %zu 轮速率 %.2f 条/轮  元认知关小过 %zu 次  当前%s\n",
           mm->win_filled, mm->last_rate, mm->n_throttles,
           mk->safe.freeze ? "**已冻结**（::safe reset 解冻）"
                           : (mk->propose_off ? "关小中" : "正常"));
    size_t shown = mm->apply_n < MICRO_APPLY_LOG_MAX ? mm->apply_n : MICRO_APPLY_LOG_MAX;
    if (shown) {
        printf("         最近应用：\n");
        for (size_t k = 0; k < shown; ++k) {
            size_t i = (mm->apply_n - 1 - k) % MICRO_APPLY_LOG_MAX;
            printf("           - %s\n", mm->apply_log[i]);
        }
    }
    printf("         元认知提醒：");
    int any = 0;
    for (int k = 0; k < MICRO_REMIND_N; ++k)
        if (mm->n_reminders[k]) { printf("%s=%zu ", MICRO_REMIND_NAME[k], mm->n_reminders[k]); any = 1; }
    printf("%s\n", any ? "" : "（无）");
}

static inline void safety_report(const AI *ai) {
    /* ---- 统一写入通道：谁在改图（写入账）---- */
    if (ai) {
        printf("[统一写入通道] 写入账：");
        for (int i = 0; i < AI_WSRC_N; ++i)
            printf("%s=%zu%s", ai_write_src_name((AiWriteSrc)i), ai->writes[i],
                   i + 1 < AI_WSRC_N ? " " : "");
        printf("\n");
        printf("               写入闸门拦下 %zu 条（::wgate on 时，AI 自己想出来的东西不够分量不沉淀）\n",
               ai->n_write_blocked);
        /* §6.180 措辞按**实际默认**写：`route_writes` 从 §6.92 起默认就是**开**
         * （以前这里只写"开/关（默认）"，读起来像"默认关"）。 */
        printf("               路由=%s %s\n", ai->route_writes ? "开" : "关",
               ai->route_writes ? "（默认；§6.92 翻的 ⇒ AIP_ROUTE=0 / ::safe route off 退回直写）"
                                : "（关着 = 都直写；AIP_ROUTE=1 / ::safe route on 打开）");
        printf("               开时走受审通道的是：**关系事实** + **非观测的共现写入**（内部对话/想象…）\n");
        printf("               都改走受审通道。**两类刻意直写**：① 感知共现（它带 hits/pattern/topic\n");
        printf("               这些观测细节，转成意图会丢信息，而且是知识的来源）；② 端点落在**协议骨架**\n");
        printf("               里的写入（[时刻:…] 这类名字 —— 内核按规矩不接「改保留符号节点」，而时间链是内核\n");
        printf("               自己的记账、不是自修改）。两类都照记写入账。)\n");
    }
    if (!ai || !ai->micro) { printf("[安全内核] 还没起来\n"); return; }
    const SafetyKernel *sk = &((const Micro *)ai->micro)->safe;
    printf("[安全内核] 总闸=%s  冻结=%s（冻结过 %zu 次）\n",
           sk->mod_off ? "关" : "开", sk->freeze ? "是" : "否", sk->n_freeze);
    printf("           预算：每步新增边 ≤%d、新增符号节点 ≤%d；单符号节点边 ≤%d；池子 ≤%d\n",
           sk->max_new_edges_step, sk->max_new_tokens_step,
           sk->max_edges_per_token, sk->max_tokens_total);
    printf("           开关：造新符号节点=%s  删边=%s\n",
           sk->new_token_off ? "关" : "开", sk->remove_off ? "关" : "开");
    printf("           判定：允许 %zu 拒绝 %zu\n", sk->n_allowed, sk->n_denied);
    printf("           拒绝理由分布：");
    int any = 0;
    for (int r = 1; r < SAFE_REASON_N; ++r)
        if (sk->deny_by_reason[r]) { printf("%s=%zu ", SAFE_REASON_NAME[r], sk->deny_by_reason[r]); any = 1; }
    printf("%s\n", any ? "" : "（还没有拒绝过任何一条）");
    size_t shown = sk->audit_n < MICRO_AUDIT_MAX ? sk->audit_n : MICRO_AUDIT_MAX;
    if (shown) {
        printf("           最近审计（最多 %zu 条）：\n", shown);
        for (size_t k = 0; k < shown; ++k) {
            size_t i = (sk->audit_n - 1 - k) % MICRO_AUDIT_MAX;
            const SafetyAudit *a = &sk->audit[i];
            printf("             #%-6llu %-6s %-24s %s  %s\n",
                   (unsigned long long)a->tick, a->op, a->target,
                   a->allowed ? "允许" : "拒绝", a->reason);
        }
    }
}

static inline void patch_report(const AI *ai) {
    if (!ai || !ai->micro) { printf("[补丁] 内核还没起来\n"); return; }
    const Micro *mk = (const Micro *)ai->micro;
    const PatchRegistry *r = &mk->patches;
    if (r->n == 0) {
        printf("[补丁] 没有挂载任何补丁 —— 这是**默认且正常**的状态：\n");
        printf("       内核不依赖补丁，没有视觉/3D/听觉它照跑（::patch mount 自省|记事本 可以挂内建的那两个）\n");
        return;
    }
    printf("[补丁] 已挂载 %zu 个：\n", r->n);
    for (size_t i = 0; i < r->n; ++i) {
        const Patch *p = r->items[i];
        if (!p) continue;
        printf("  %-10s 类型=%s 状态=%s 感知=%zu 输出=%zu(最近%zu) 动作=%zu 提议=%zu 出错=%zu\n",
               p->name ? p->name : "?",
               (p->kind == PATCH_SENSE) ? "感知" : (p->kind == PATCH_ACT) ? "动作" : "感知+动作",
               p->enabled ? "启用" : "**已停用**",
               p->n_sense, p->n_out_total, p->n_out_last, p->n_act, p->n_propose, p->n_err);
        if (p->err[0]) printf("             最后一次错误：%s（连续 %d 次）\n", p->err, p->err_streak);
    }
    printf("       可用内建：自省（把内核自己的状态变成符号节点）｜记事本（**输出侧**：可编辑的答复 + 草稿区 + 发送控制）\n");
}

/* ---- 开关：::mod off / on / coact off … ---- */
static inline void micro_set_mod(AI *ai, int on) {
    Micro *mk = micro_of(ai);
    if (!mk) return;
    mk->safe.mod_off = on ? 0 : 1;
    /* 刻意**不**顺手解冻：冻结是停止条件，要解冻就显式 ::safe reset。
     * 开关和安全内核是两件事，别让「重新打开」偷偷绕过停止条件。 */
}
/* 统一写入通道的路由开关（默认关：观测直写是正常路径，知识类可路由）。
 * `::safe route on|off`；测量钩子 AIP_ROUTE=1。 */
static inline int micro_set_route(AI *ai, int on) {
    if (!ai) return -1;
    ai->route_writes = on ? 1 : 0;
    return 0;
}

static inline int micro_set_detector(AI *ai, const char *name, int on) {
    Micro *mk = micro_of(ai);
    if (!mk || !name) return -1;
    if (!strcmp(name, "coact"))      mk->coact_off = on ? 0 : 1;
    else if (!strcmp(name, "gap"))   mk->gap_off = on ? 0 : 1;
    else if (!strcmp(name, "prune")) mk->prune_off = on ? 0 : 1;
    else if (!strcmp(name, "cond"))  mk->meta.cond_off = on ? 0 : 1;
    else return -1;
    if (!strcmp(name, "coact")) mk->coact_off_by_meta = 0;   /* 手动的，元认知别去改它 */
    return 0;
}

static inline void patch_list_builtin(void) {
    printf("可用内建补丁：\n");
    printf("  自省    感知侧：把内核自己的状态变成符号节点（不依赖任何外部设备）\n");
    printf("  记事本  输出侧：可编辑的答复文本 + 草稿区 + 什么时候发送\n");
    printf("          操作：写/插入/删/改/清空/提交/清空发言/插入发言/删发言/改发言/发送/不发/模式/读\n");
    printf("          ::patch mount 记事本 → ::patch op 记事本 写 <文本>\n");
}

/* 一行摘要，挂在 ::stats 后面（ai_stats 在 micro.h 之前，不能反过来调用）。
 * 让「自修改到底在不在动」这件事在每次 ::stats 里都看得见。 */
static inline void micro_stats(const AI *ai) {
    if (!ai || !ai->micro) {
        printf("       微内核：还没起来（未走过安全点）\n");
        return;
    }
    const Micro *mk = (const Micro *)ai->micro;
    const MicroMeta *mm = &mk->meta;
    size_t rem = 0;
    for (int k = 0; k < MICRO_REMIND_N; ++k) rem += mm->n_reminders[k];
    printf("       微内核：自修改=%s%s  安全点=%zu轮 意图=%zu(应用%zu/拒%zu/去重%zu) "
           "新边=%zu 新符号节点=%zu 沉淀=%zu 删边=%zu 补丁=%zu 提醒=%zu\n",
           mk->safe.mod_off ? "关" : "开",
           mk->safe.freeze ? "(已冻结)" : (mk->propose_off ? "(关小中)" : ""),
           mm->n_steps, mm->n_proposed,
           mm->n_applied, mm->n_denied, mm->n_dedup,
           mm->n_new_edges, mm->n_new_tokens, mm->n_derived, mm->n_removed,
           mk->patches.n, rem);
}

#endif /* MICRO_H */
