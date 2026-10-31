/* token.h — 单头文件关联机 v12
 *
 * **术语（§6.127）**：图里的那个东西**对外一律叫「符号节点」** —— 输出、帮助、
 * 文档、注释里都是这个词（用户要求改称）。C 标识符**保持历史名** `Token` / `token_*`
 * / `TOKEN_*`：它们是类型名、函数名和存档里早就有的字段名，全量改名只换来
 * 一次几万处的机械 diff，风险（改漏/改错、`codes/` 镜像与旧文档对不上）远大于收益。
 * 读代码时的对照：`Token` = 符号节点；`token.h` = 符号层；`ai_tokenize` = 分词器
 * （它切出来的是**词**，intern 之后才成为符号节点）。
 *
 * 符号层：符号节点 / 边 / hits / mem_state
 * 亚符号层：VSA 超向量 + 绑定/解绑/捆绑 + 视角树绑定
 */
#ifndef TOKEN_H
#define TOKEN_H

#include <stdlib.h>
#include "ptrtag.h"
#include "share.h"      /* §6.581 池级共享：emb 这类"不带指针的大块"引用计数共享 */
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
#ifndef EMB_DIM
#define EMB_DIM 1024
#endif

#define TOKEN_SEQ_NONE ((uint32_t)-1)
#define TOKEN_ID_NONE  ((uint32_t)-1)

/* 一个词名最多缓存多少个「字符」（§6.127 的字符切分缓存，见 Token.ch_p）。
 * 16 对"词"来说绰绰有余；超长的名字走 255 哨兵、现场切，行为不变。 */
#ifndef TOKEN_CH_MAX
#define TOKEN_CH_MAX 16
#endif

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
    /* **过程边**（第 9 条 ①；用户 2026-10-10 拍板的 role 方案）：从**过程根**
     * `[过程:名字]` 出去的那些边 —— 步骤（动作槽 `[步骤:N]`）、调用（`[调用]`）、
     * 回边（`[回边]`）。它要解决的就是四选一里那个问题：**过程边与共现边分不开**。
     * role 一直是序列化字段（0~5 已用，还剩 250 个值）⇒ **存档布局不动**。 */
    ASSOC_ROLE_PROCESS = 6,
    /* §6.299 **想象出来的边**：`ai_imagine` 把两个焦点词直接连起来（不造节点、不绑向量）。
     * 角色字节本来就在存档里 ⇒ 零格式改动；它可存盘、可衰减，并且能一眼看出
     * "这条不是听到的，是它自己想出来的"。 */
    ASSOC_ROLE_IMAGINED = 7,
} AssocRole;

/* 来源档的中文名（一份，谁要显示都调它 —— 别在几个地方各写一遍）。 */
static inline const char *assoc_role_name(uint8_t role) {
    switch (role) {
    case ASSOC_ROLE_INPUT:   return "输入";
    case ASSOC_ROLE_REPLY:   return "回复";
    case ASSOC_ROLE_COINED:  return "造词";
    case ASSOC_ROLE_DERIVED: return "推论";
    case ASSOC_ROLE_INNATE:  return "基准";
    case ASSOC_ROLE_PROCESS: return "过程";
    case ASSOC_ROLE_IMAGINED: return "想象";
    default:                 return "共现";
    }
}
/* ============================ 过程记号（第 9 条 ①+②） ============================
 *
 * 用户 2026-10-10 讨论后拍板：**"过程边 vs 共现边"用 role 标记**（四选一里的那一个）。
 * 这一版做的是 §6.56 五步走的 **①+②**：
 *   ① 一个"过程" = 一个**根符号节点**（`[过程:名字]`，在内部命名空间里）+ 从它出去的边（结构）；
 *   ② 控制流记号挂在**动作槽**上（和 `[前提]`/`[因果]` 同一套做法，判据是精确名字比较）：
 *      `[步骤:N]` 第几步 / `[调用]` 这一步是"叫别人/别处" / `[回边]` 回到前面某一步。
 * 边用 `ASSOC_ROLE_PROCESS` 标成**过程边** ⇒ 查询与推理能把"过程"与"共现"一眼分开
 * （这正是四选一要解决的那个问题）。**不动存档布局**：role 一直是序列化字节。 */
#define PROC_TOKEN_PREFIX  "[过程:"
#define PROC_STEP_PREFIX   "[步骤:"
#define PROC_CALL_MARK     "[调用]"
#define PROC_BACK_MARK     "[回边]"      /* 不带条件的回边 */
#define PROC_BACK_PREFIX   "[回边:"      /* 带条件的回边：`[回边:条件词]` = 回到那一步，只要条件成立
                                          * （图里一条 (根,目标) 只有一条边 ⇒ 条件不能另挂一条边，只能
                                          * 写在回边自己的记号里；`[条件:X]→[步骤:N]` 那种会撞车，实测过） */
/* ⑤ 分支/循环/异常：条件挂**这一步**上（`[条件:X]` = 走这一步要先 X 成立）、
 * 互斥的另一支挂 `[否则]`、出错时跳的那一支挂 `[异常:类型]`。
 * 条件判据**不另造**：就是前提那三档（`ai_premise_holds`：注意窗口里 / 图上够得着 / 够得着且有据）。 */
#define PROC_STEP_CLAIM    "@要:"      /* 步骤记号里带"要核的那件事"：`[步骤:N@要:主语/关系/宾语[/不]]`
                                          * —— 一条边一个记号，所以"顺序 + 要核什么"写在同一个记号里（不撞车）。
                                          * 带它的步骤跑的时候走 `rel_solve` + `der_verify` **独立复核**。 */
#define PROC_STEP_SAY      "@说:"      /* §6.511 步骤记号里带**答复模板**：`[步骤:N@说:猫 属于 $1]` */

/* §6.521 外壳缓存的**版本号**：`标外壳` 每跑一次就 +1，剥离那边的缓存看到变了就重建。
 * 为什么不用"池子词数变了"当判据：标记一个**已经在池子里**的词时词数不变 ⇒ 缓存永远不更新
 * （§6.521 实测：先标 请 再标 问，第二条不生效）。放 token.h 是因为 micro.h（标外壳）与
 * agent.h（剥离）都看得见它，而 micro.h 看不见 agent.h 的全局。 */
static size_t g_shell_epoch;
#define PROC_COND_PREFIX   "[条件:"
#define PROC_ELSE_MARK     "[否则]"
#define PROC_EXC_PREFIX    "[异常:"
#define PROC_SHOW_MAX      256     /* 一次最多列/跑多少条过程边（有界） */
#define PROC_BACK_MARK     "[回边]"      /* 不带条件的回边 */
#define PROC_BACK_PREFIX   "[回边:"      /* 带条件的回边：`[回边:条件词]` = 回到那一步，只要条件成立
                                          * （图里一条 (根,目标) 只有一条边 ⇒ 条件不能另挂一条边，只能
                                          * 写在回边自己的记号里；`[条件:X]→[步骤:N]` 那种会撞车，实测过） */

/* 话题 id：语料里显式标注（`[话题:N]` 那一行属于第 N 个话题）。
 * 0 = 无话题（seed / generated 那些通用共现语料）。 */
#define ASSOC_TOPIC_NONE 0u

/* 句尾模式：这个符号节点后面**还有几个词**这个句子就结束了。
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
/* §6.633 16 → 64：阶段二要用"动态关系"当**结构槽**（谓词参数 `参数1..8`、规则前提 `前提1..6`、
 * 结论 `结论`），老上限 16 一用就满、把用户自己教的新谓语挤掉。表是 `char*[64]`（+384 字节），
 * 存档里只存 `n` 条名字，旧档（≤16 条）照样读 —— 不动格式。 */
#define AI_MAX_DYNREL  64      /* 动态关系（清单外的谓语 + 内核结构槽）最多几个 */
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
     *   · 前提：`A —[前提]→ P → B`（P 是**普通符号节点**，`[前提]` 挂在动作槽上）→ edge_premise_polarity()
     *   · 因果猜想：动作槽上的 `[因果]` → edge_is_causal_guess()
     * 于是"联想"这条路上**没有任何条件**（用户原话：联想本来不依赖前提），
     * 只有逻辑/思维层按上面两个判据决定用不用。 */
    Token    *main;
    Token    *action;
    uint32_t  hits, seq;
    uint64_t  birth_tick, last_touch_tick;
    /* **时刻集合**（§6.111，v12 起存盘）：这条边**在哪些 tick 上被观察到**。
     * 以前只有"首次/最近"两个点（区间）⇒ 判不了"同时/先后不明"；
     * 现在留 4 个**互不相同**的 tick（满了之后只滚最后一个 = 最初 3 次 + 最近 1 次，
     * 这一点在总表里照实写明：不是无限集合）。 */
    uint32_t  tset[4];
    uint8_t   n_tset;
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
    /* §6.214 **反查倒排表的桶头**（0 = 空，否则是 `TokenPool.rev_next` 的下标+1）。
     * 只追加、不维护删除：读取时用 `rel_direct*` **校验**，陈旧项自然被过滤
     * （所以只有一个写入点，不存在"漏维护 ⇒ 静默漏知识"）。只进内存、不存盘。 */
    uint32_t    rev_head;
    /* §6.570 **有没有因果猜想边**：`ai_causal_edge` 只需知道"有没有"，
     * 而它的判据要读边。局部加载下"为了确认没有因果边去把整段读回来"是纯浪费 ⇒
     * 缓存这一位：0 = 一定没有（可以**不读边**直接判空），1 = 有（要读边细看）。
     * 只进内存 + 段文件索引里的 flags 字节（不进 model.bin：读盘后第一次建因果边时会补上，
     * 最坏多读几段，不会答错）。 */
    uint8_t     has_causal;
    /* §6.573 **这一份边被改过吗**（相对"从段文件读回来时的样子"）：
     * 存盘/段文件只把脏的写成 delta —— 这就是"每个智能体只维护自己的修改"的落点。
     * 只进内存：读回来时清 0，任何**边写入**路径置 1（见 token_add_assoc_ex 等）。 */
    uint8_t     edges_dirty;
    /* §6.208 **入度缓存**（GC 用）：`ai_gc` 开头 O(E) 重建一次，之后 O(1) 读。
     * 只进内存、不存盘（读盘/训练后第一次 GC 会重建）。放在这里是因为
     * `sizeof(Token)` 不写进存档（model_io 逐字段写），加字段不动格式。 */
    uint32_t    in_degree;
    /* §6.588 **被"知识边"指着的次数**（§6.588：GC 不许因为"很久没提"就把知识删掉）。
     * 和 `in_degree` 一样只进内存、`ai_gc` 开头一次 O(E) 重建。 */
    uint32_t    rel_in_degree;
    Token      *parent;
    uint8_t     is_category, is_garbage;
    uint64_t    birth_tick, last_touch_tick;
    uint8_t     mem_state;
    TokenPool  *owner;
    const char *name;
    /* emb = **学出来的**分布式表示（1024 维 int8）：从图的邻居按 PPMI 加权做
     * **随机索引**投影出来的，所以语义相关的词余弦真的会高。
     * §6.299 删掉了 hv/hv_prev（名字的随机投影 + VSA 绑定代数）：它们不进任何
     * 对话/推理路径，占存档 2/3，而且 hv_prev 与 hv 恒等（漂移指标结构上恒 0）。
     * emb 是 (边 + hits + 稳定 id) 的确定性函数 → **不进模型文件**，
     * 训练收尾和读盘之后各现算一遍（见 ai_embed_build）。 */
    int8_t     *emb;
    Token      *perspective_of;
    Token      *next_in_time;
    /* **缓存复核队列**（§6.127）：只有"身上挂着推论边（role=推论）"的词才进这条链。
     * 加它的原因见 think.h 的 think_revalidate_caches()：那一趟原来**每轮扫全图**
     * （出厂模型 1229 词 / 41338 边 ⇒ 每轮 4 万次边访问），而真正要复核的边只有几十条。
     * 两个字段都落在结构体原有的对齐空隙里 ⇒ sizeof(符号节点) 不变，存档布局也不动
     * （model_io 是逐字段写的，不是整块 memcpy）。 */
    Token      *cache_next;     /* 队列里的后继（只在 cache_q 置位时有效） */
    uint8_t     cache_q;        /* 已经在这条队列里了（去重用的） */
    /* **名字的字符切分缓存**（§6.127）：`name` 一旦 intern 就不再变，而
     * "把名字切成 UTF-8 字符"这一步是**每次检索的头号开销**（`gprof`：字符 bigram
     * 项占全轮 76% 的 CPU，出厂模型每轮 1.3 万次调用 ⇒ 每轮把库里 4000 多条句子的
     * 名字逐字节重切一遍）。切一次存下来，按名字指针读回即可。
     * `n_ch == 0` = 还没切过；`n_ch == 255` = 名字超过 TOKEN_CH_MAX 个字符，
     * 故意不再缓存（退回现场切，行为与以前逐字节相同）。
     * 这是 **name 的纯派生值** ⇒ 不进存档、不占格式（`name` 一改就重算）。 */
    uint8_t     n_ch;
    uint8_t     ch_len[TOKEN_CH_MAX];
    const char *ch_p[TOKEN_CH_MAX];
    uint32_t    ch_sig;         /* 名字里出现过哪些**字符**（32 位位图，纯派生）
                                 * —— 只用来给字符 bigram 打分做**精确预筛**：
                                 * 两边位图不相交 ⇒ 一个字符都不共享 ⇒ 分数必然是 0。 */
};

struct TokenPool {
    Token     **tokens;
    size_t      count, cap;
    /* **池子的序号**（§6.127）：每个 pool_new 领一个**全局单调递增**的号。
     * 用途：给"跨调用的备忘"当一个不受 malloc 地址复用影响的身份 ——
     * 工具里连着建两个脑子，`AI*`/`TokenPool*` 很可能落在**同一块地址**上，
     * 光比指针会串味（实测过：按指针比"永远不命中"，反过来又可能"命中错了"）。 */
    uint64_t    serial;
    Token      *unknowing;
    MeaningFree meaning_free;
    NameEntry **buckets;
    size_t      n_buckets, n_entries;
    uint64_t    current_tick;
    /* **装载期**（§6.111）：`ai_load_file` 读完之后会重建关系/帧/emb（那里会 touch 一堆边）。
     * 那些 touch **不是观测**（跟"只有观测进图"一个口径）⇒ 不记进边的**时刻集合**。 */
    int         loading;
    /* §6.566 **局部加载**（多智能体/部分图）：
     * `parts` 是"边段文件"的上下文（定义在 parts.h，用 void* 避免头文件倒序），
     * `part_load` 是"这条边的数组在磁盘上，去读回来并挂上"的钩子 —— 由 parts.h 装上。
     * 两者都**不进存档**：它们是"这次打开的方式"，不是图的内容。 */
    void       *parts;
    void      (*part_load)(void *ctx, Token *t);   /* ctx = parts；签名写 void* 是为了让 token.h 不依赖 parts.h */
    /* §6.575 这一份边**被用到**（换出按"最久没用"挑，所以"用"要能记下来）。
     * 只有 attach 过段文件时才非空 ⇒ 平时这条路径只多一次判空。 */
    void      (*part_touch)(void *ctx, Token *t);

    PerspectiveNode  *perspective_root;
    PerspectiveNode  *perspective_cur;
    PerspectiveNode **perspective_pool;
    size_t            perspective_n, perspective_cap;

    /* **缓存复核候选队列**（§6.127）：见 Token.cache_next 的注释。
     * `cache_seeded`：读盘/新建之后的**第一趟**要全图播一次种（存档里带的推论边不在队列里），
     * 播过之后就一直靠写入时入队，不再扫全图。 */
    Token            *cache_head;
    size_t            n_cache_queued;
    uint8_t           cache_seeded;

    /* §6.214 **反查倒排表**（`谁 R B` 用）：按"宾语"分桶的链。
     * 只追加（打关系标记时写一条），删除/GC 不管 —— 查的时候把候选拿去 `rel_direct*`
     * 校验，陈旧项自然过滤。载入后一次 O(E) 重建（见 model_io）。
     * 每条 12 字节（src 4 + meta 4 + next 4）；出厂模型 4.4 万边 ≈ 0.6 MB。 */
    uint32_t         *rev_src;
    uint32_t         *rev_meta;   /* rid | neg<<15 */
    uint32_t         *rev_next;
    size_t            rev_n, rev_cap;
};
/* ============================ §6.565 边数组的**唯一入口** ============================
 *
 * 多智能体 / 局部加载（用户 2026-10-24 的目标）的**前置**：
 * 现在 `Token.assocs` 是一段 malloc 出来的内存，全仓有 ~170 处 `t->assocs[i]` 直接访问。
 * 将来"边暂时在磁盘上"就是把这个指针打上 `AIP_PTR_DISK` 标签（见 `ptrtag.h`）——
 * 那时**所有读边的地方都必须经过这里**，否则解引用一个带标签的指针会崩。
 *
 * 所以从这一轮起：**新增代码不许直接摸 `t->assocs`**，一律走
 *   · `token_assocs(t)`     —— 要写 / 要可能触发**按需加载**（加载本身是写：挂回数组、清标签）
 *   · `token_assocs_ro(t)`  —— 只读视图（拿得到 const Token 的地方用这个；它**不**触发加载）
 * 收口进度由 `tests/assocaudit.sh` 盯着：直接访问的处数只许降、不许涨。
 */
static inline void token_assocs_resolve(Token *t) {
    if (!t) return;
    if (aip_ptr_tag(t->assocs) == AIP_PTR_DISK && t->owner && t->owner->part_load)
        t->owner->part_load(t->owner->parts, t);   /* 读段文件、挂回真数组、清标签 */
}
static inline AssocEntry *token_assocs(Token *t) {
    token_assocs_resolve(t);
    if (t && t->owner && t->owner->part_touch) t->owner->part_touch(t->owner->parts, t);
    return t ? (AssocEntry *)aip_ptr_strip(t->assocs) : NULL;
}
/* "只读视图"：**也解析标签**（这一点是有意的、也是踩过之后改的）——
 * 第一版让它不解析（"只读就不该有副作用"），结果 `rel_direct` 这类 const 路径
 * 在"边已卸到磁盘"时拿到 **NULL 数组 + count>0**，循环里直接解引用 ⇒ 段错误。
 * 教训：**正确性 > 纯度**。加载只是把磁盘上的东西挂回内存（缓存填充），语义上仍是只读。 */
static inline const AssocEntry *token_assocs_ro(const Token *t) {
    if (!t) return NULL;
    token_assocs_resolve((Token *)t);
    if (t->owner && t->owner->part_touch) t->owner->part_touch(t->owner->parts, (Token *)t);
    return (const AssocEntry *)aip_ptr_strip(t->assocs);
}
/* §6.567 这段边现在在内存里吗？（换出/统计用；应用层不许自己看指针标签） */
static inline int token_assocs_resident(const Token *t) {
    return t && aip_ptr_tag(t->assocs) == AIP_PTR_RAM;
}

/* §6.566 把边"挂回内存"（加载完那一段时用）：存储层的活，应用层不许自己赋指针。 */
static inline void token_assocs_set_ram(Token *t, AssocEntry *as, size_t cnt) {
    if (!t) return;
    if (aip_ptr_tag(t->assocs) == AIP_PTR_RAM && t->assocs != as) free(t->assocs);
    t->assocs = as;                 /* 真指针 ⇒ 标签自动为 0（RAM） */
    t->count = cnt;
    t->cap = cnt;                   /* 下次追加会 realloc 成 2 倍 */
}

/* §6.566 把边"卸到磁盘"：存储层的活（free + 打标签），所以放在 token.h 里，
 * 应用层（parts.h）只调这一个函数，不许自己摸指针。
 * ⚠ 调用方必须保证"磁盘上确实有这一段"（否则读回来是空的）—— parts.h 的 detach 负责这件事。 */
static inline void token_assocs_set_disk(Token *t) {
    if (!t) return;
    if (aip_ptr_tag(t->assocs) == AIP_PTR_RAM) free(t->assocs);
    t->assocs = (AssocEntry *)(uintptr_t)AIP_PTR_DISK;   /* 哨兵：低 3 位 = DISK，strip 后是 0 */
    t->cap = 0;                                          /* 想追加会先 resolve（见 add_assoc_ex） */
}

/* §6.584 **清掉"死的自环边"**：老版本留下的 a→a 边（出厂图 18 条）。
 * 判据（和 `aicheck.h` 的"空自环"同一套）：两端同一个词、没有角色/关系/动作/命中/时刻。
 * 为什么要清而不是留着：查询 `A R A`（用户真会这么问）会让某条答复/强化路径
 * 把这它当普通边 touch/置位，于是"死数据"开始有 hits 甚至关系位 —— 自检就是在随机会话里
 * 抓到"自环边（带语义）"的。清掉之后"图里没有自环"可以当**硬不变式**来验。
 * 返回清掉几条。只对**在内存里**的边表动（磁盘上的段不动）。 */
static inline int token_edge_is_dead_self(const Token *t, const AssocEntry *e) {
    return t && e && e->main == t && !e->action && e->role == ASSOC_ROLE_NONE &&
           e->rel == 0 && e->nrel == 0 && e->dyn == 0 && e->hits == 0 && e->n_tset == 0;
}

static inline size_t pool_drop_dead_self_loops(TokenPool *p) {
    if (!p) return 0;
    size_t dropped = 0;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (!t || t->count == 0 || !token_assocs_resident(t)) continue;
        size_t w = 0;
        for (size_t k = 0; k < t->count; ++k) {
            AssocEntry *e = &token_assocs(t)[k];
            if (token_edge_is_dead_self(t, e)) { dropped++; continue; }
            if (w != k) token_assocs(t)[w] = *e;
            w++;
        }
        if (w != t->count) { t->count = w; t->edges_dirty = 1; }
    }
    return dropped;
}

/* 边在磁盘上吗 / 边数组是"真内存"吗 —— 存储层之外**不许**直接看标签（§6.565 的收口）。 */
static inline int token_assocs_on_disk(const Token *t) {
    return t && aip_ptr_tag(t->assocs) == AIP_PTR_DISK;
}
static inline int token_assocs_in_ram(const Token *t) {
    return t && aip_ptr_tag(t->assocs) == AIP_PTR_RAM;
}
/* DISK 哨兵必须是"strip 之后为 0"（不能带着一个假地址）—— 存储层自检用 */
static inline int token_assocs_disk_sentinel_ok(const Token *t) {
    return t && aip_ptr_tag(t->assocs) == AIP_PTR_DISK && aip_ptr_strip(t->assocs) == NULL;
}

/* 与访问器成对：边数也从这里读，避免"数组换了、count 还在别处读" */
static inline size_t token_assoc_count(const Token *t) { return t ? t->count : 0; }


/* ============================ 前提符号节点（淘汰「语境」后的表示） ============================
 *
 * 用户的澄清（原话）：「联想本来不依赖前提，可以正常联想；前提符号节点可以被正常联想到，
 * 但是**逻辑会知道这两个之间的前提符号节点是什么**，并且可以被正常处理。」
 *
 * 所以一条「有前提的连结」在图上长这样：
 *
 *     A —[前提]→ P → B          （P 是**普通符号节点**；`[前提]` 是行为符号节点，挂在 A→P 的**动作槽**上）
 *
 * 三条性质：
 *   ① **联想照走**：A→P→B 都是普通边，别处不因为「有前提」而被挡住；
 *   ② **前提能被正常联想到**：P 就是普通符号节点（可以是被说出来的词），
 *      它自己有关系、有共现，谁也不特殊对待它；
 *   ③ **逻辑知道前提是什么**：`token_premise_between(A,B)` 直接答出 P（可多个）——
 *      条件不再藏在边的字段里，而是**图里的节点 + 边上的标记**。
 *
 * 旧的「语境」（AssocEntry.subs/subs_not）由此**退役**：先是没有创建者（2026-10-07 上午），
 * 然后连字段一起删掉（模型 v10；旧文件仍读得回来，见 docs/语境退役-前提token.md）。
 * 注意 `[前提]` 是**标记**（行为符号节点），不是「前提符号节点」本身 ——
 * 前提符号节点是它指向的那个普通词。 */
#define AI_PREMISE_MARK     "[前提]"      /* 条件：P **在**注意窗口里 */
#define AI_PREMISE_NOT_MARK "[前提不]"    /* 条件：P **不在**注意窗口里（旧 subs_not 的替代） */
/* 因果猜想的行为符号节点（也挂在**动作槽**上）—— 名字只有这一处：
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
/* **记忆里的猜测**（§6.125）标记：动作槽写成 `[类比:依据词]`（像 `[修正:…]`/`[临:…]` 那样自带出处）。
 * 与「因果猜想」的关键区别：因果猜想**连推论都不是**（草稿，`rel_direct_*` 一律跳过）；
 * 猜测记忆**是记下来的结论**（`role = 推论`），能被后续推理当输入 ——
 * 但 `rel_direct`/`rel_direct_review`（"图里有没有这条**知识**"）**跳过它**，
 * 于是它永远变不成"知道的"，验证器的自查也永远查不到它（不中毒）。 */
#define AI_GUESS_MARK_PREFIX "[类比"
static inline int edge_is_guess(const AssocEntry *e) {
    return e && e->action && e->action->name &&
           strncmp(e->action->name, AI_GUESS_MARK_PREFIX,
                   strlen(AI_GUESS_MARK_PREFIX)) == 0;
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
        const AssocEntry *e = &token_assocs_ro(from)[i];
        if (e->main == to && edge_is_premise_link(e)) return e;
    }
    return NULL;
}

/* A 出边里有没有**前提链接**（有的话，逻辑层就该按前提决定用不用） */
static inline int token_has_premise_link(const Token *from) {
    if (!from) return 0;
    for (size_t i = 0; i < from->count; ++i)
        if (edge_is_premise_link(&token_assocs_ro(from)[i])) return 1;
    return 0;
}

/* **逻辑问的那句话**：A 与 B 之间的前提符号节点是什么（带**极性**）？
 * 顺着 A —[前提]/[前提不]→ P → B 找；pol 里写 +1（要 P 在）/ -1（要 P 不在）。
 * 返回条数（一件事可以有多个前提）。 */
static inline size_t token_premises(const Token *a, const Token *b,
                                    const Token **out, int *pol, size_t cap) {
    size_t n = 0;
    if (!a || !b) return 0;
    for (size_t i = 0; i < a->count && n < cap; ++i) {
        const AssocEntry *e = &token_assocs_ro(a)[i];
        int p = edge_premise_polarity(e);
        if (!p || !e->main) continue;
        const Token *pt = e->main;
        for (size_t j = 0; j < pt->count; ++j)
            if (token_assocs_ro(pt)[j].main == b) {
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
    /* ①b **顺序图上的 BFS**（§6.78）：`next_in_time` 只能表达"一条线"，
     * 但图上还有 `[关系:之后]` **标记边**（`X —[关系:之后]→ Y` 的意思是 "X 在 Y 之后"）
     * —— 多条这样的边合起来是个**偏序**。这里从 b 出发沿"谁在谁之后"走一遍（上限 64 个词），
     * 走得到 a 就说明 a 在 b 之后（⇒ b 先）。零格式改动，只是把图里已有的东西读全。 */
    {
        const Token *queue[64];
        size_t qn = 0, qh = 0;
        const Token *seen[64];
        size_t sn = 0;
        queue[qn++] = b; seen[sn++] = b;
        while (qh < qn) {
            const Token *cur = queue[qh++];
            if (cur == a) {
                /* 走到 a ⇒ **a 在 b 之前** ⇒ a 先（返回 +1）。
                 * ⚠ 这里第一版写成 -1（方向反了），被 optest 的老断言当场拦下 —— 别凭直觉写符号。 */
                if (why && whycap)
                    token_order_text(why, whycap, "顺序图（「[关系:之后]」边）：", an, " 在前");
                return +1;
            }
            /* 从 cur 出发：cur 挂着"之后"标记时，它连到的那些词都在 cur **之前** */
            int cur_after = 0;
            for (size_t i = 0; i < cur->count; ++i) {
                const Token *m = token_assocs_ro(cur)[i].main;
                if (m && m->name && strcmp(m->name, TOKEN_ORDER_MARK_AFTER) == 0) { cur_after = 1; break; }
            }
            if (!cur_after) continue;
            for (size_t i = 0; i < cur->count && qn < 64; ++i) {
                const Token *nx = token_assocs_ro(cur)[i].main;
                if (!nx || nx == cur) continue;
                if (nx->name && (strcmp(nx->name, TOKEN_ORDER_MARK_AFTER) == 0 ||
                                 strcmp(nx->name, TOKEN_ORDER_MARK_BEFORE) == 0)) continue;
                int dup = 0;
                for (size_t k = 0; k < sn; ++k) if (seen[k] == nx) { dup = 1; break; }
                if (dup) continue;
                seen[sn++] = nx;
                queue[qn++] = nx;
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
                const Token *m = token_assocs_ro(x)[i].main;
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


/* 过程边的判据（`AssocEntry` 完整定义之后才放得下）。 */
static inline int edge_is_process(const AssocEntry *e) {
    return e && e->role == ASSOC_ROLE_PROCESS;
}
/* `[步骤:N]` → N（不是过程边 / 不是步骤边 → 0）。只认数字，别的写法一律不算。 */
static inline int edge_step_index(const AssocEntry *e) {
    if (!edge_is_process(e) || !e->action || !e->action->name) return 0;
    if (strncmp(e->action->name, PROC_STEP_PREFIX, strlen(PROC_STEP_PREFIX)) != 0) return 0;
    const char *q = e->action->name + strlen(PROC_STEP_PREFIX);
    if (*q < '0' || *q > '9') return 0;
    long v = strtol(q, NULL, 10);
    return (v > 0 && v < 1000) ? (int)v : 0;
}
static inline int edge_is_call(const AssocEntry *e) {
    return edge_is_process(e) && e->action && e->action->name &&
           strcmp(e->action->name, PROC_CALL_MARK) == 0;
}
static inline int edge_is_back(const AssocEntry *e) {
    if (!edge_is_process(e) || !e->action || !e->action->name) return 0;
    return strcmp(e->action->name, PROC_BACK_MARK) == 0 ||
           strncmp(e->action->name, PROC_BACK_PREFIX, strlen(PROC_BACK_PREFIX)) == 0;
}
/* `[回边:条件词]` → 条件词（不带条件的回边返回 NULL） */
static inline const char *edge_back_cond(const AssocEntry *e) {
    if (!edge_is_back(e)) return NULL;
    if (strncmp(e->action->name, PROC_BACK_PREFIX, strlen(PROC_BACK_PREFIX)) != 0) return NULL;
    return e->action->name + strlen(PROC_BACK_PREFIX);
}
static inline int edge_is_cond(const AssocEntry *e) {
    return edge_is_process(e) && e->action && e->action->name &&
           strncmp(e->action->name, PROC_COND_PREFIX, strlen(PROC_COND_PREFIX)) == 0;
}
/* `[条件:X]` → **去掉结尾 `]` 的 X**（拷进调用方的缓冲，返回长度）。
 * 直接返回名字里的指针会把结尾那个 `]` 也带出去（打印时看得见 —— 实测踩过）。 */
static inline size_t proc_mark_bare(const char *name, const char *prefix,
                                    char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!name || !prefix) return 0;
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl) != 0) return 0;
    const char *q = name + pl;
    size_t k = 0;
    while (*q && *q != ']' && k + 1 < cap) out[k++] = *q++;
    if (out && cap) out[k] = 0;
    return k;
}

/* 步骤记号里有没有"要核的事"：`[步骤:N@要:S/R/B]`（末尾 `/不` = 要它**不**成立）。
 * 有就交给 `rel_solve` + `der_verify` 独立复核（第 9 条 ① 的收口）。返回 1 = 有。 */
static inline int proc_step_claim(const AssocEntry *e, int *out_idx,
                                  char *sb, size_t scap, char *rb, size_t rcap,
                                  char *bb, size_t bcap, int *out_neg) {
    if (out_idx) *out_idx = 0;
    if (out_neg) *out_neg = 0;
    if (sb && scap) sb[0] = 0;
    if (rb && rcap) rb[0] = 0;
    if (bb && bcap) bb[0] = 0;
    if (!e || !e->action || !e->action->name) return 0;
    const char *nm = e->action->name;
    if (strncmp(nm, PROC_STEP_PREFIX, strlen(PROC_STEP_PREFIX)) != 0) return 0;
    if (out_idx) *out_idx = edge_step_index(e);
    const char *at = strstr(nm, PROC_STEP_CLAIM);
    if (!at) return 0;
    at += strlen(PROC_STEP_CLAIM);
    /* 三段：主语 / 关系 / 宾语（可带结尾 `/不`） */
    char *dst[3] = { sb, rb, bb };
    size_t cap[3] = { scap, rcap, bcap };
    int seg = 0; size_t k = 0;
    for (const char *q = at; ; ++q) {
        if (*q == '/' || *q == ']' || *q == 0) {
            if (seg < 3 && dst[seg] && cap[seg]) dst[seg][k < cap[seg] ? k : cap[seg] - 1] = 0;
            seg++; k = 0;
            if (*q != '/') break;
            if (seg >= 3) { if (out_neg && strcmp(q + 1, "不]") == 0) *out_neg = 1; break; }
            continue;
        }
        if (seg < 3 && dst[seg] && k + 1 < cap[seg]) dst[seg][k++] = *q;
    }
    return (seg >= 3 && sb && sb[0] && rb && rb[0] && bb && bb[0]);
}

/* §6.511 步骤记号里有没有**答复模板**：`[步骤:N@说:模板]`。有就取出模板（到 `]` 为止）。 */
static inline int proc_step_say(const AssocEntry *e, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!e || !e->action || !e->action->name) return 0;
    const char *nm = e->action->name;
    if (strncmp(nm, PROC_STEP_PREFIX, strlen(PROC_STEP_PREFIX)) != 0) return 0;
    const char *at = strstr(nm, PROC_STEP_SAY);
    if (!at) return 0;
    at += strlen(PROC_STEP_SAY);
    size_t k = 0;
    while (*at && *at != ']' && k + 1 < cap) out[k++] = *at++;
    if (out && cap) out[k] = 0;
    return out && out[0];
}

/* `[条件:X]` → X（名字里的那一段；不是条件边就返回 NULL）。 */
static inline const char *edge_cond_name(const AssocEntry *e) {
    if (!edge_is_cond(e)) return NULL;
    return e->action->name + strlen(PROC_COND_PREFIX);
}
static inline int edge_is_else(const AssocEntry *e) {
    return edge_is_process(e) && e->action && e->action->name &&
           strcmp(e->action->name, PROC_ELSE_MARK) == 0;
}
static inline int edge_is_exc(const AssocEntry *e) {
    return edge_is_process(e) && e->action && e->action->name &&
           strncmp(e->action->name, PROC_EXC_PREFIX, strlen(PROC_EXC_PREFIX)) == 0;
}
static inline const char *edge_exc_name(const AssocEntry *e) {
    if (!edge_is_exc(e)) return NULL;
    return e->action->name + strlen(PROC_EXC_PREFIX);
}
/* 这条边算不算"过程的一部分"：role 对，**而且**动作槽上确实有个过程记号 ——
 * 光有 role 说不出它是第几步/干什么。 */
static inline int edge_is_proc_step(const AssocEntry *e) {
    return edge_step_index(e) > 0 || edge_is_call(e) || edge_is_back(e) ||
           edge_is_cond(e) || edge_is_else(e) || edge_is_exc(e);
}

struct NameEntry { uint32_t hash, id; NameEntry *next; };

struct PerspectiveNode {
    Token           *who;
    PerspectiveNode *parent;
    uint32_t         depth;
    uint32_t         refcount;
};




/* ---- §6.214 反查倒排表：**只追加 + 读时校验** ---------------------------------
 * `dst->rev_head` 是桶头，条目在 pool 的三条平数组里。
 *
 * **写入点清单（§6.224 逐条核过；`::revcheck` 是它的安全网）**：
 *   ① `token_tag_rel`（命中已有边 / 新建边两处）
 *   ② `token_tag_nrel`（两处）
 *   ③ `token_tag_dyn`（两处）
 *   ④ `token_tag_dyn_neg`（把"否定"置上时追加 neg=1 —— 在 ③ 之后）
 *   ⑤ `micro.h` 的 `MOD_TAG_DERIVED`（两处：肯定/否定）
 *   ⑥ `agent.h` 的 `agent_guess_remember`（直写 `e->rel`，刻意不走 token_tag_rel）
 * 载入后由 `model_io.h` 一次 O(E) 重建兜底。
 * 删除、GC、改口、猜测升格都**不动它** —— 陈旧条目在查询时被 `rel_direct*` 校验掉。
 * ⚠ 新增第 11 个写入点时必须也追加，否则 `::revcheck` 会红（它就是这么被发现的两次）。 */
static inline void pool_rev_add(TokenPool *p, Token *dst, uint32_t src_id,
                                uint32_t rid, int neg) {
    if (!p || !dst || !rid) return;
    if (p->rev_n == p->rev_cap) {
        size_t ncap = p->rev_cap ? p->rev_cap * 2 : 1024;
        uint32_t *ns = (uint32_t *)realloc(p->rev_src, ncap * sizeof *ns);
        if (!ns) return;
        p->rev_src = ns;
        uint32_t *nm = (uint32_t *)realloc(p->rev_meta, ncap * sizeof *nm);
        if (!nm) return;
        p->rev_meta = nm;
        uint32_t *nn = (uint32_t *)realloc(p->rev_next, ncap * sizeof *nn);
        if (!nn) return;
        p->rev_next = nn;
        p->rev_cap = ncap;
    }
    uint32_t idx = (uint32_t)p->rev_n;
    p->rev_src[idx] = src_id;
    p->rev_meta[idx] = (rid & 0x7FFFu) | (neg ? 0x8000u : 0u);
    p->rev_next[idx] = dst->rev_head;
    dst->rev_head = idx + 1;
    p->rev_n = (size_t)idx + 1;
}

/* §6.214 桶里有没有**任何一条** (rid, neg)（判"这个词是不是认得的宾语"，不关心 src）。*/
static inline int pool_rev_any(const TokenPool *p, const Token *dst, uint32_t rid, int neg) {
    if (!p || !dst) return 0;
    for (uint32_t e = dst->rev_head; e; e = p->rev_next[e - 1]) {
        uint32_t meta = p->rev_meta[e - 1];
        if ((uint32_t)(meta & 0x7FFFu) != (rid & 0x7FFFu)) continue;
        if (((meta & 0x8000u) != 0) != (neg != 0)) continue;
        return 1;
    }
    return 0;
}

/* §6.214 桶里有没有 (src, rid, neg) 这一条（`::revcheck` 用）。*/
static inline int pool_rev_has(const TokenPool *p, const Token *dst, uint32_t src_id,
                               uint32_t rid, int neg) {
    if (!p || !dst) return 0;
    for (uint32_t e = dst->rev_head; e; e = p->rev_next[e - 1]) {
        uint32_t meta = p->rev_meta[e - 1];
        if ((uint32_t)(meta & 0x7FFFu) != (rid & 0x7FFFu)) continue;
        if (((meta & 0x8000u) != 0) != (neg != 0)) continue;
        if (p->rev_src[e - 1] == src_id) return 1;
    }
    return 0;
}

/* ---- 缓存复核队列（§6.127）--------------------------------------------------
 * 只有一个不变式要守：**图上每一条推论边，其起点词必须进过队列**。
 * 三条写入路径都调 token_cache_enqueue（见 token_tag_full / micro.h 的
 * MOD_TAG_DERIVED / agent.h 的两处），读盘那一路由 cache_seeded 的首趟全图播种兜住。 */
static inline void token_cache_enqueue(Token *t) {
    if (!t || !t->owner || t->cache_q) return;
    t->cache_q = 1;
    t->cache_next = t->owner->cache_head;
    t->owner->cache_head = t;
    t->owner->n_cache_queued++;
}
/* 把整条队列**取走**（头指针交出去并清空）：调用方逐条处理，处理完再决定要不要
 * 重新入队（活下来的缓存要接着复核 ⇒ 重新入队到**下一轮**那一条链上）。
 * 这样"取走—处理—重新入队"不会在本轮内自激（新链不是本轮正在走的那条）。 */
static inline Token *token_cache_take(TokenPool *p) {
    Token *h = p->cache_head;
    p->cache_head = NULL;
    p->n_cache_queued = 0;
    return h;
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
    /* §6.582 池序号是**跨线程共享**的自增计数（`::brain fork` 可能在别的线程建脑）
     * ⇒ 用原子自增；单线程下和以前完全一样（1,2,3…）。 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    static _Atomic uint64_t g_pool_serial = 0;
#else
    static uint64_t g_pool_serial = 0;
#endif
    TokenPool *p = (TokenPool *)calloc(1, sizeof(TokenPool));
    if (!p) return NULL;
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    p->serial = atomic_fetch_add_explicit(&g_pool_serial, 1u, memory_order_relaxed) + 1u;
#else
    p->serial = ++g_pool_serial;
#endif
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
        /* §6.566 边数组可能在磁盘上（指针带 AIP_PTR_DISK 标签）：带标签的指针**不能 free** */
        if (aip_ptr_tag(t->assocs) == AIP_PTR_RAM) free(t->assocs);
        /* §6.581 `emb` 可能和别的池**共享**（引用计数）⇒ 不能直接 free，要还回去 */
        aip_buf_release(t->emb);
        free((void *)t->name); free(t);
    }
    free(p->tokens);
    free(p->rev_src); free(p->rev_meta); free(p->rev_next);   /* §6.214 反查表 */
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
/* §6.581 **池级共享**：把 `dst` 的 emb 换成 `src` 的那一份（引用计数 +1），
 * 前提是"两个池是**同一个图**"——顺序、名字、个数都一致（`::brain fork` 就是这种情况）。
 *
 * 为什么先验名字：emb 是按词算的向量，**错位共享比不共享危险得多**（内存好看、答案悄悄变错）。
 * 所以这里宁可拒绝（返回 0，调用方照旧各持一份），也不做"部分共享"。
 * 共享之后两边仍然是**各自可写**的：写 emb 的地方会走 `aip_buf_own` 先复制（见 ai.h）。 */
static inline size_t pool_share_emb(TokenPool *dst, const TokenPool *src) {
    if (!dst || !src || dst == src) return 0;
    if (dst->count != src->count || dst->count == 0) return 0;
    for (size_t i = 0; i < dst->count; ++i) {
        const Token *a = dst->tokens[i], *b = src->tokens[i];
        if (!a || !b) return 0;
        if (a->id != b->id) return 0;
        /* 名字两边都有就必须相等；**两边都没有**（无名临时节点，存读时按位置复原）
         * 也算对得上 —— 实测出厂图里有这种节点（第 1202 个），一开始要求"必须有名"
         * 于是共享整体被拒（报了"共享 0 个词"）。 */
        if (a->name || b->name) {
            if (!a->name || !b->name) return 0;
            if (strcmp(a->name, b->name) != 0) return 0;   /* 错位就整体放弃 */
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < dst->count; ++i) {
        Token *a = dst->tokens[i];
        Token *b = src->tokens[i];
        if (!a->emb || !b->emb) continue;
        if (a->emb == b->emb) { n++; continue; }       /* 已经共享过 */
        aip_buf_release(a->emb);                       /* 丢掉自己那份（可能是私有的）*/
        a->emb = (int8_t *)aip_buf_share(b->emb);
        n++;
    }
    return n;
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
    t->emb = (int8_t *)calloc(EMB_DIM, 1);       /* 全 0 = 还没算过 */
    if (!t->emb) {
        free((void *)t->name); free(t);
        return NULL;
    }

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
        if (token_assocs_ro(t)[i].main == main && edge_is_plain(&token_assocs_ro(t)[i]))
            return token_assocs_ro(t)[i].role == ASSOC_ROLE_DERIVED;
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
    /* **时刻集合**：同一个 tick 只记一次；满了（4）只滚最后一个槽（见结构里的说明）。
     * ⚠ `n_tset` 是**新字段**：不是每条边都被零初始化过（老路径/手工造的边可能是垃圾值）
     * ⇒ 用之前必须钳一次，否则下面的循环会**越界写**（实测：训练时直接 SIGSEGV）。 */
    if (e->n_tset > 4) e->n_tset = 0;
    if (now > 0 && !(t->owner && t->owner->loading)) {
        uint32_t tn = (uint32_t)now;
        int seen = 0;
        for (uint8_t k = 0; k < e->n_tset; ++k) if (e->tset[k] == tn) { seen = 1; break; }
        if (!seen) {
            if (e->n_tset < 4) e->tset[e->n_tset++] = tn;
            else               e->tset[3] = tn;      /* 满了：滚最新（最初 3 次 + 最近 1 次） */
        }
    }
    e->last_touch_tick = now;
    e->mem_state = MEM_ACTIVE;
}

/* ============================ 关联编辑 ============================ */

static inline int token_add_assoc_ex(Token *t, Token *main, Token *action) {
    if (!t) return -1;
    /* §6.584 **自环边在这里就挡住**：这是所有"建边"的唯一原语（共现/关系/猜测/
     * 受审沉淀都走它）。自环没有任何语义，而它会污染老数据里那条"空自环"
     * （实测：`水 有 水 吗` 走"猜"的路径，把空自环变成带 rel/role/action 的自环）。
     * 挡在这里 = 调用方不用各记一遍；返回 -1 与"没连上"的约定一致。 */
    if (main == t) return -1;
    token_assocs_resolve(t);   /* §6.566 边在磁盘上时先读回来，再谈扩容 */
    t->edges_dirty = 1;        /* §6.573 改过 ⇒ 进 delta */
    if (t->count == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 4;
        AssocEntry *na = (AssocEntry *)realloc(t->assocs, ncap * sizeof *na);
        if (!na) return -1;
        t->assocs = na; t->cap = ncap;
    }
    AssocEntry *e = &token_assocs(t)[t->count];
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
    /* **时刻集合必须显式清零**（§6.127 抓到的老 bug）：这两个字段原来没初始化，
     * 而边表是 `realloc` 出来的 ⇒ 新建边的 `n_tset`/`tset[]` 是**堆里的垃圾**。
     * 后果不是"崩"而是更阴的两条：
     *   ① 垃圾 `n_tset` 恰好 ≤4 时，这条边凭空"在某个 tick 被观察到过"——
     *      「同时/先后」的判断被污染（而 `>4` 时才在 token_touch_entry 里被钳掉）；
     *   ② **存档不确定**：换一个结构体大小/分配顺序，垃圾就变，`model.bin` 跟着变
     *      （实测：只给符号节点加了字段，同一段 100 轮会话存出来 1009 字节不同 ——
     *       逐条比图却**完全一样**，就是这个字段在作怪）。 */
    e->n_tset = 0;
    memset(e->tset, 0, sizeof e->tset);
    t->count++;
    return 0;
}
/* §6.588 这条边是不是**知识**（关系/否定/动态谓语位）—— 它和"共现"的维护规则不同：
 * 共现可以淡、可以忘；知识是用户教的（或语料里断言的），GC 不许删。 */
static inline int edge_is_knowledge(const AssocEntry *e) {
    return e && (e->rel || e->nrel || e->dyn);
}
/* 这个词有没有"知识"出边（GC 用它判断能不能把词标成垃圾）*/
static inline int token_has_knowledge_out(const Token *t) {
    if (!t) return 0;
    for (size_t i = 0; i < t->count; ++i)
        if (edge_is_knowledge(&token_assocs_ro(t)[i])) return 1;
    return 0;
}

/* 「t —main→ 这条边在不在」—— v10 起边上没有语境，所以只看 main。
 * （同 main 的两条边只可能是**动作槽不同**，那种用 token_find_assoc_action。） */
static inline int token_add_assoc_unique(Token *t, Token *main) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i)
        if (token_assocs(t)[i].main == main && edge_is_plain(&token_assocs(t)[i])) return 0;
    return token_add_assoc_ex(t, main, NULL);
}
/* 找 (main, action) 都对的那条边（没有就 NULL）。
 * 为什么需要：因果猜想边（动作槽 = `[因果]`）要和**普通观测边并存** ——
 * 「猫 吃 鱼」是观测到的知识，「我做 X 之后 Y 出现」是猜想，两者不是同一条边。 */
static inline AssocEntry *token_find_assoc_action(Token *t, Token *main, Token *action) {
    if (!t) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (token_assocs(t)[i].main == main && token_assocs(t)[i].action == action) return &token_assocs(t)[i];
    return NULL;
}
/* 按 (main, action) 建边：已经有了就把那条返回（不新建、不动 hits）。 */
static inline AssocEntry *token_add_assoc_action_unique(Token *t, Token *main, Token *action) {
    AssocEntry *e = token_find_assoc_action(t, main, action);
    if (e) return e;
    if (token_add_assoc_ex(t, main, action) != 0) return NULL;
    return &token_assocs(t)[t->count - 1];
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
        if (token_assocs(a)[i].main == b) { token_assocs(a)[i].role = ASSOC_ROLE_INNATE; return; }
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
    if (role == ASSOC_ROLE_DERIVED) token_cache_enqueue(a);   /* §6.127：推论边的起点要进复核队列 */
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &token_assocs(a)[i];
        if (e->main != b || !edge_is_plain(e)) continue;
        e->role = role;
        e->topic = topic;
        if (pattern) e->pattern = pattern;
        e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &token_assocs(a)[a->count - 1];
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
/* §6.583 **配置开关也要原子**：`::revise off` 是主线程/控制线程写的，而问答线程在读。
 * 以前它是普通 int ⇒ 运行中改就是一处数据竞争（TSan 曾报在 `token_tag_rel`）。 */
AIP_ATOMIC_INT(g_obs_revise, 1);
/* §6.582 每轮问答的**一次性标志**（tag 里置、学习那条路立刻读）⇒ 线程局部。
 * 单线程语义不变；多线程不再互相把"改口了吗"读成别人的。 */
static AIP_TLS int g_obs_revised = 0;

/* 给 a→b 打上语义关系（边不存在就先建）。
 * 和 token_tag_full 分开，是因为「出处角色」和「语义关系」是两件事：
 * 一条关系边也可能同时带着回复角色。 */
static inline int token_tag_rel(Token *a, Token *b, uint8_t rel_id, uint32_t bump) {
    if (a) a->edges_dirty = 1;   /* §6.573 关系边改过 ⇒ 进 delta */
    /* §6.584 **自环不写**：`A 属于 A` 没有任何语义，而它会把老数据里那条"空自环"
     * 变成"带语义的自环"（实测：随机教事实时 `鸟 属于 鸟` 就踩到了）。
     * 这条护栏放在**写入原语**里，而不是只靠上层调用方记得判。 */
    if (!a || !b || a == b || rel_id == 0 || rel_id >= REL_DYN_BASE) return -1;
    g_obs_revised = 0;
    uint16_t bit = (uint16_t)(1u << (rel_id - 1));
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &token_assocs(a)[i];
        if (e->main != b) continue;
        /* §6.170 **观测/教学压过"记忆里的猜测"**：猜出来的结论只是笔记（`[类比:依据]`），
         * 用户教了 / 观测到了同一条事实，就该把它**升格成知识** ——
         * 以前这里只是 `continue`（跳过非普通边），于是教完再问还一直显示「·猜」，
         * 实测：`狗 会 唱歌 吗`（先猜一次）→ `::teach 狗 会 唱歌` → 再问仍是「会·猜」。 */
        if (edge_is_guess(e)) {
            e->action = NULL;
            e->rel = 0; e->nrel = 0;
            e->role = ASSOC_ROLE_NONE;
        }
        if (!edge_is_plain(e)) continue;      /* 因果猜想/前提链接照旧跳过 */
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
        if (AIP_LD(g_obs_revise) && (e->nrel & bit)) { e->nrel &= (uint16_t)~bit; g_obs_revised = 1; }
        /* **幂等**：这一位已经置过就不再加权（关系可以叠加，同一位不能重复加）。
         * 否则存盘再读会重复装一遍 relations.txt，hits 翻倍 → emb 跟着变。 */
        if (e->rel & bit) return 0;
        e->rel |= bit;
        if (a->owner) pool_rev_add(a->owner, b, a->id, rel_id, 0);   /* §6.214 反查表 */
        if (bump) e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &token_assocs(a)[a->count - 1];
    e->rel = bit;
    if (a->owner) pool_rev_add(a->owner, b, a->id, rel_id, 0);       /* §6.214 */
    e->hits = bump;
    return 0;
}

/* 给 a→b 打上**否定**的核心关系（`猫 不 会 飞`） */
static inline int token_tag_nrel(Token *a, Token *b, uint8_t rel_id, uint32_t bump) {
    if (a) a->edges_dirty = 1;   /* §6.573 关系边改过 ⇒ 进 delta */
    if (!a || !b || a == b || rel_id == 0 || rel_id >= REL_DYN_BASE) return -1;   /* §6.584 自环不写 */
    g_obs_revised = 0;
    uint16_t bit = (uint16_t)(1u << (rel_id - 1));
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &token_assocs(a)[i];
        if (e->main != b) continue;
        if (edge_is_guess(e)) {              /* §6.170 同 token_tag_rel：猜测升格成观测 */
            e->action = NULL;
            e->rel = 0; e->nrel = 0;
            e->role = ASSOC_ROLE_NONE;
        }
        if (!edge_is_plain(e)) continue;
        /* 同 token_tag_rel：观测进来时先把这条边上的缓存整个撤掉 */
        if (e->role == ASSOC_ROLE_DERIVED) {
            e->rel = 0; e->nrel = 0;
            e->role = ASSOC_ROLE_NONE;
        }
        /* 改口：把同一条边上对立极性的旧观测改掉（理由见 token_tag_rel） */
        if (AIP_LD(g_obs_revise) && (e->rel & bit)) { e->rel &= (uint16_t)~bit; g_obs_revised = 1; }
        if (e->nrel & bit) return 0;
        e->nrel |= bit;
        if (a->owner) pool_rev_add(a->owner, b, a->id, rel_id, 1);   /* §6.214 否定 */
        if (bump) e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &token_assocs(a)[a->count - 1];
    e->nrel = bit;
    if (a->owner) pool_rev_add(a->owner, b, a->id, rel_id, 1);       /* §6.214 */
    e->hits = bump;
    return 0;
}

/* **动态关系的否定**（§6.80）：核心关系用 `nrel` 的 0~8 位，9~14 位空着、**15 位**拿来当
 * "这条动态关系是否定"的旗子 —— 这样零格式改动（`nrel` 本来就序列化），也不用动边布局。
 * 为什么不再开一个字段：存档格式是铁律（v10），空位就藏在现成的字节里。 */
#define REL_NEG_DYN_BIT 0x8000u

/* 给 a→b 打上**动态关系**（清单外的谓语，id 从 8 起） */
static inline int token_tag_dyn(Token *a, Token *b, uint8_t dyn_id, uint32_t bump) {
    if (a) a->edges_dirty = 1;   /* §6.573 关系边改过 ⇒ 进 delta */
    if (!a || !b || a == b || dyn_id == 0) return -1;
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &token_assocs(a)[i];
        if (e->main != b || !edge_is_plain(e)) continue;
        if (e->dyn == dyn_id) return 0;
        e->dyn = dyn_id;
        /* §6.584 幂等：同一条动态事实重复写不再追加反查条目（去重）*/
        if (a->owner && !pool_rev_has(a->owner, b, a->id,
                                      (uint32_t)(REL_DYN_BASE + dyn_id - 1), 0))
            pool_rev_add(a->owner, b, a->id, (uint32_t)(REL_DYN_BASE + dyn_id - 1), 0);

        if (bump) e->hits += bump;
        e->mem_state = MEM_ACTIVE;
        return 0;
    }
    if (token_add_assoc_ex(a, b, NULL) != 0) return -1;
    AssocEntry *e = &token_assocs(a)[a->count - 1];
    e->dyn = dyn_id;
    if (a->owner && !pool_rev_has(a->owner, b, a->id, (uint32_t)(REL_DYN_BASE + dyn_id - 1), 0))
        pool_rev_add(a->owner, b, a->id, (uint32_t)(REL_DYN_BASE + dyn_id - 1), 0);
    e->hits = bump;
    return 0;
}

/* 动态关系 + 否定：`dyn` 记关系 id，`nrel` 的 15 位记"是否定"（见 REL_NEG_DYN_BIT） */
static inline int token_tag_dyn_neg(Token *a, Token *b, uint8_t dyn_id, int neg, uint32_t bump) {
    if (a) a->edges_dirty = 1;   /* §6.573 关系边改过 ⇒ 进 delta */
    int rc = token_tag_dyn(a, b, dyn_id, bump);
    if (rc != 0) return rc;
    for (size_t i = 0; i < a->count; ++i) {
        AssocEntry *e = &token_assocs(a)[i];
        /* §6.584 过滤条件必须和 `token_tag_dyn` **一模一样**（多查 `edge_is_plain`）：
         * 以前这里不查 plain，可能抓到"另一条同 main 的因果/猜测边"把否定旗子打上去，
         * 而 `token_tag_dyn` 追加的是另一条边 ⇒ 反查表出现"有 neg=0、没 neg=1"的窟窿
         * （随机问答里自检报「反查表缺（动态 喵）：鸟 -> 鱼」）。 */
        if (e->main != b || !edge_is_plain(e) || e->dyn != dyn_id) continue;
        if (neg) e->nrel |= (uint16_t)REL_NEG_DYN_BIT;
        else     e->nrel &= (uint16_t)~REL_NEG_DYN_BIT;
        /* §6.224 **否定的动态关系也要进反查表**；§6.584 改成"缺了才补"（幂等）：
         * 靠 `fresh_neg` 的话，旗子已经在（比如从段文件读回来）就永远补不上。 */
        if (a->owner && !pool_rev_has(a->owner, b, a->id,
                                      (uint32_t)(REL_DYN_BASE + dyn_id - 1), neg ? 1 : 0))
            pool_rev_add(a->owner, b, a->id, (uint32_t)(REL_DYN_BASE + dyn_id - 1), neg ? 1 : 0);
        return 0;
    }
    return -1;
}
static inline int edge_dyn_neg(const AssocEntry *e) {
    return e && (e->nrel & (uint16_t)REL_NEG_DYN_BIT) != 0;
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
    if (t) t->edges_dirty = 1;   /* §6.573 */
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        AssocEntry *e = &token_assocs(t)[i];
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
        if (token_assocs(t)[i].seq == seq) { token_assocs(t)[i].action = action; return 0; }
    return -1;
}
static inline int token_set_action_by_target(Token *t, Token *main, Token *action) {
    if (!t) return -1;
    for (size_t i = 0; i < t->count; ++i) {
        AssocEntry *e = &token_assocs(t)[i];
        if (e->main != main || !edge_is_plain(e)) continue;
        e->action = action;
        return 0;
    }
    return -1;
}
static inline void token_clear_actions(Token *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; ++i) token_assocs(t)[i].action = NULL;
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
        if (token_assocs(t)[i].seq == seq) return &token_assocs(t)[i];
    return NULL;
}
static inline const AssocEntry *token_seq_to_entry_c(const Token *t, uint32_t seq) {
    if (!t || seq == TOKEN_SEQ_NONE) return NULL;
    for (size_t i = 0; i < t->count; ++i)
        if (token_assocs_ro(t)[i].seq == seq) return &token_assocs_ro(t)[i];
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
        if (entry_matches(&token_assocs_ro(t)[i], ctx, n_ctx)) return i;
    return (size_t)-1;
}
static inline Token *token_associate(const Token *t, Token *const *ctx,
                                     size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    return i == (size_t)-1 ? NULL : token_assocs_ro(t)[i].main;
}
static inline Token *token_associate1(const Token *t, Token *ctx, float energy) {
    if (ctx) { Token *c[1] = { ctx }; return token_associate(t, c, 1, energy); }
    return token_associate(t, NULL, 0, energy);
}
static inline uint32_t token_associate_seq(const Token *t, Token *const *ctx,
                                           size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    return i == (size_t)-1 ? TOKEN_SEQ_NONE : token_assocs_ro(t)[i].seq;
}
static inline void token_hit(Token *t, size_t i) {
    if (t && i < t->count) token_touch_entry(t, &token_assocs(t)[i]);
}
static inline Token *token_associate_tap(Token *t, Token *const *ctx,
                                         size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    if (i == (size_t)-1) return NULL;
    token_touch_entry(t, &token_assocs(t)[i]);
    return token_assocs(t)[i].main;
}
static inline uint32_t token_associate_tap_seq(Token *t, Token *const *ctx,
                                               size_t n_ctx, float energy) {
    size_t i = token_associate_idx(t, ctx, n_ctx, energy);
    if (i == (size_t)-1) return TOKEN_SEQ_NONE;
    token_touch_entry(t, &token_assocs(t)[i]);
    return token_assocs(t)[i].seq;
}
static inline Token *token_associate_weighted(const Token *t, Token *const *ctx,
                                              size_t n_ctx, float energy, uint32_t *rng) {
    if (!t || !rng) return NULL;
    if (*rng == 0) *rng = 0x9E3779B9u;
    size_t reach = token_reach(t, energy);
    uint64_t total = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &token_assocs_ro(t)[i];
        if (entry_matches(e, ctx, n_ctx)) total += (uint64_t)e->hits + 1;
    }
    if (total == 0) return NULL;
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x;
    uint64_t pick = (uint64_t)x % total, acc = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &token_assocs_ro(t)[i];
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
    for (size_t i = 0; i < t->count; ++i) s += token_assocs_ro(t)[i].hits;
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
        token_assocs(t)[i].hits = (uint32_t)((float)token_assocs(t)[i].hits * factor);
    t->recent_misses = (uint32_t)((float)t->recent_misses * factor);
}

/* ============================ 观察学习 ============================ */

static inline void token_observe_pair(Token *a, Token *b) {
    if (!a || !b || a == b) return;
    for (size_t k = 0; k < a->count; ++k) {
        AssocEntry *e = &token_assocs(a)[k];
        if (e->main == b && edge_is_plain(e)) {
            token_touch_entry(a, e);
            return;
        }
    }
    if (token_add_assoc_ex(a, b, NULL) == 0)
        token_touch_entry(a, &token_assocs(a)[a->count - 1]);
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
    /* §6.235 逐位写 1024 个字节太慢（实测 1.9 µs/次；embed_build 里每条边要两次，
     * 4.4 万边 ≈ 170 ms）。改成**查表按字节写**：256 种字节 → 8 个 ±1，
     * 每个 64 位字 memcpy 8 次。取值与逐位写法**完全一样**（同一个 z、同一个位序）。 */
    static int8_t tab[256][8];
    /* §6.582 这张表是**一次初始化、之后只读**的：多线程同时首次调用会竞争
     * （TSan 在冷启动那条路上抓到 `tab_ready`）。用 原子标志 + 锁 做 once：
     * 初始化好之后快路径只有一次 acquire 读，不加锁。 */
#if AIP_HAS_ATOMICS
    static _Atomic int tab_ready = 0;
    if (!atomic_load_explicit(&tab_ready, memory_order_acquire)) {
        static AipLock tab_lk = AIP_LOCK_INIT(f);
        aip_lock(&tab_lk);
        if (!atomic_load_explicit(&tab_ready, memory_order_relaxed)) {
            for (int v = 0; v < 256; ++v)
                for (int b = 0; b < 8; ++b)
                    tab[v][b] = (int8_t)(((v >> b) & 1) ? 1 : -1);
            atomic_store_explicit(&tab_ready, 1, memory_order_release);
        }
        aip_unlock(&tab_lk);
    }
#else
    static int tab_ready = 0;
    if (!tab_ready) {
        for (int v = 0; v < 256; ++v)
            for (int b = 0; b < 8; ++b)
                tab[v][b] = (int8_t)(((v >> b) & 1) ? 1 : -1);
        tab_ready = 1;
    }
#endif
    uint64_t x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)id * 0xBF58476D1CE4E5B9ULL);
    for (size_t i = 0; i < EMB_DIM; i += 64) {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        for (size_t k = 0; k < 64 && i + k < EMB_DIM; k += 8)
            memcpy(out + i + k, tab[(z >> k) & 0xFFu], 8);
    }
}

/* emb 的余弦：emb 不是 ±1 而是一般的 int8，必须按模长归一化。
 * （vsa_cosine 只除以维度，那是给 ±1 向量用的，用在这里会算错。） */
static inline float emb_cosine(const int8_t *a, const int8_t *b) {
    if (!a || !b) return 0.0f;
    int64_t dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < EMB_DIM; ++i) {
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

/* ============================ 递归激活 ============================ */

typedef void (*TokenVisitFn)(Token *t, size_t depth, float energy, void *user);

struct ActivateCtx {
    Token       **stack;
    size_t        n, cap;
    float         decay, cutoff;
    size_t        max_depth;
    /* §6.135 **访问上限**（0 = 不限，默认 —— 所有既有调用方一个字都不变）。
     * 为什么必须要有：`act_seen()` 只看**当前这条 DFS 路径**（返回时 `a->n--`），
     * 所以同一个符号节点可以被**不同路径反复激活** ⇒ 代价随深度**指数增长**。
     * 实测（出厂模型，深度/每个词）：深度 2 = 14.9 ms、深度 3 = **1648 ms** ——
     * 再深一档就是小时级。睡眠那条路用的是**深度 6**，于是它实际上**永远跑不完**。
     * 上限到了就停止扩散（确定性：同样的输入走同样的前缀，可复现）。 */
    size_t        max_visits;
    size_t        n_visits;             /* 这次激活已经访问过几个（含重复路径）*/
    size_t        n_cut;                /* 因为上限被截断的次数（诊断用）*/
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
    if (a->max_visits && a->n_visits >= a->max_visits) { a->n_cut++; return; }   /* §6.135 上限 */
    a->n_visits++;
    if (act_push(a, t) != 0) return;
    token_touch(t);
    if (a->on_visit) a->on_visit(t, depth, energy, a->user);
    size_t reach = token_reach(t, energy);
    float next_e = energy * a->decay;
    for (size_t i = 0; i < reach; ++i) {
        AssocEntry *e = &token_assocs(t)[i];
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
    if (a->max_visits && a->n_visits >= a->max_visits) { a->n_cut++; return; }   /* §6.135 上限 */
    a->n_visits++;
    if (a->on_visit) a->on_visit(t, depth, energy, a->user);
    size_t reach = token_reach(t, energy);
    float next_e = energy * a->decay;
    for (size_t i = 0; i < reach; ++i) {
        AssocEntry *e = &token_assocs(t)[i];
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
