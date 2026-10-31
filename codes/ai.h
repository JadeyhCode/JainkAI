/* ai.h — 认知层
 *
 * 机制：预测编码 / 内生状态 / 稳态设定点 / 多焦点 / 自我模型
 *       因果边 + 反事实 / 多信号元认知 / 表达自举 / 信用分配
 *       想象 / 类比 / 内部对话 / 概念漂移 / 自我叙事
 *       失败诊断 / 置信度校准 / 抽象形成 / 睡眠（增量）/ 遗忘（GC）
 * * 依赖：token.h
 */
/* 特性测试宏：`action.h` 要用 fork/exec/poll/kill/clock_gettime，而这些声明只在
 * POSIX 模式下可见；**特性宏必须在第一个 libc 头之前**定义（本项目用 `-std=c11`，
 * 那是严格 ISO 模式）。放这里是因为 ai.h 是所有 TU 的第一个头
 * （main.c 与 tools/ 下的每个工具都从 agent.h → ai.h 进来）：一处生效，处处生效。 */
#define _DEFAULT_SOURCE 1

#ifndef AI_H
#define AI_H

/* §6.169 **软件版本号**（用户 2026-10-17 定的：0.01）。
 * 唯一出处 —— 启动横幅、`::help`、README 都从这一个宏取。
 * ⚠ 它**不是**存档格式版本：二进制兼容看 `model_io.h` 的 `MODEL_VERSION`（现在是 v13），
 * 两个别混（一个是"哪一版程序"，一个是"哪一版文件格式"）。 */
#ifndef AIP_VERSION
#define AIP_VERSION "0.01"
#endif

#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <time.h>       /* §6.135 分段计时用 clock_gettime */

/* 游走跟踪（::wtrace）：把每条候选的路径按**名字**打出来。
 * 名字而不是指针 —— 就是为了能跨进程 diff。 */
static int g_walk_trace = 0;


#define AI_DIALOG_SPLIT_NAME "[原语:分割]"   /* 语料里「输入/回复」的边界标记 */
#define AI_REPLY_END_NAME   "[对话:结束]"   /* 回复结束标记：游走到它就收句 */

/* ============================ 疑问词表（**只此一份**） ============================
 * 这张表以前在 agent.h 里抄了三份（学关系时的「别把问句当陈述」、
 * 检索门槛的「是不是问句」、拒答门槛的「是不是问句」），
 * 三份还各不完全一样 —— 那种重复是 bug 的温床：改一处、忘一处，
 * 表现就是「同一句话在一条路径上算问句、在另一条上不算」。
 * 现在只有这一张表 + 两个查询函数。 */
/* §6.531 **目标③：疑问词表下沉成数据**（`[qword]` 整词 / `[qpart]` 子串）。
 *
 * 为什么：这张表原来写死在 ai.h，加一个新疑问词（`咋`/`啥时候`/`多久`）要重新编译 ——
 * 而"新说法靠数据、不靠改代码"是本项目的核心约束。现在**数据的桶占位**：
 *   · `patterns.txt` 里有 `[qword]`/`[qpart]` 行 ⇒ 用数据（写死的表只当 `AIP_QBUILTIN` 对照）；
 *   · 没有 patterns.txt（比如某些套件在自己的临时目录里跑）⇒ 退回下面这两张**种子表**，
 *     保证"没有数据也不会把问句当陈述学进去"。
 * 数据桶的实现在 agent.h（`aip_pat_bucket`），这里只留读取端 —— 语言层的判据还是在这一处。 */
#define AI_QWORD_MAX  48
#define AI_QPART_MAX  64
static char g_qword[AI_QWORD_MAX][24];
static char g_qpart[AI_QPART_MAX][24];
static size_t g_qword_n, g_qpart_n;
static int g_qword_data, g_qpart_data;   /* 1 = 数据接管（patterns.txt 读到了，桶可能有 0 行） */
static const char *const AI_QUESTION_WORDS[] = {
    "吗", "呢", "么", "吧", "?", "？",
    "谁", "什么", "什么时候", "怎么", "怎么样", "哪", "哪里", "哪儿", "哪个",
    "为什么", "几", "多少", "如何", "干什么", "是不是", "有没有", "对不对", "好不好",
    NULL
};
static inline int ai_is_question_word(const char *w) {
    if (!w || !w[0]) return 0;
    if (g_qword_data) {                      /* §6.531 数据接管：桶里没有就不是 */
        for (size_t i = 0; i < g_qword_n; ++i)
            if (strcmp(w, g_qword[i]) == 0) return 1;
        return 0;
    }
    for (size_t i = 0; AI_QUESTION_WORDS[i]; ++i)
        if (strcmp(w, AI_QUESTION_WORDS[i]) == 0) return 1;
    return 0;
}
/* §6.171 **词里含疑问词**也算问句 —— 分词器会把「多少钱」合成**一个**词，
 * 而整词匹配看不出它是个问句。实测（出厂模型）：`这个 东西 多 少 钱`
 * 被当成事实学进去，图里多了一条 `这个 --[东西]--> 多少钱`（谓语是「东西」！）。
 *
 * 只对**实疑问词**做包含匹配；语气词（吗/呢/么/吧）与单字「几」只认整词 ——
 * 否则「酒吧」「茶几」这种词会被误判成问句（宁可不学，也别错学；但也别把正常词挡了）。 */
static const char *const AI_QUESTION_SUBSTR[] = {
    "什么", "怎么", "为什么", "多少", "如何", "干什么",
    "是不是", "有没有", "对不对", "好不好",
    /* §6.270 「多大/多久/多远/多高…」也是疑问词 —— 以前不在表里，实测
     * `太阳 有 多大` 被当成**事实学进去**（图里多一条 `太阳 --[有]--> 多大`）。 */
    "多大", "多久", "多远", "多高", "多长", "多快", "多贵", "多重", "多深",
    /* §6.386 `是谁` / `是啥`：分词把它们并成**一个词**（`猫 是谁`），逐词检查看不出是问句，
     * 于是 `小明 是谁` 会被当场学成「小明 是谁」这条垃圾事实（实测「记下 了 ： 小 明 是谁」）。*/
    "是谁", "是啥",
    /* §6.373 反问/强调标记：`难不成 猫 会 飞`、`莫不是 猫 属于 动物`、`谁 不 知道 …`、
     * `猫 该 不会 飞 吧`、`猫 岂 是 动物`、`难 道 说 …` —— 这些句子**没有 吗 也是问句**。
     * 不列进来时：`猫岂是动物` 会被当场学成「猫岂 是 动物」（造出新词），
     * `难不成猫会飞` 更糟 —— 被当成**改口**，把「猫不会飞」改写成「猫会飞」。 */
    "难不成", "莫不是", "谁不知道", "该不会", "岂", "难道说",
    /* §6.374 提问式引导语（数据里 `[lead] 请问?` 也标了；这里是一道**不依赖数据的**安全网：
     * 数据没加载时也不能把「请问 猫 会 飞」当成陈述学成「请问猫 会 飞」）。*/
    "请问", "我想问", "想问", "帮我看看", "帮我看一下", "能不能告诉我", "能告诉我",
    NULL
};
static inline int ai_token_has_question_word(const char *w) {
    if (!w || !w[0]) return 0;
    if (ai_is_question_word(w)) return 1;
    /* 最短的实疑问词是 2 个汉字（6 字节）—— 单字词在上面整词那一关就已经排除了，
     * 这里直接返回，省掉每个词一串 strstr（这条函数在热路径上，量得到开销）。 */
    if (strlen(w) < 6) return 0;
    if (g_qpart_data) {                      /* §6.531 数据接管（同整词那张表） */
        for (size_t i = 0; i < g_qpart_n; ++i)
            if (strstr(w, g_qpart[i])) return 1;
        return 0;
    }
    for (size_t i = 0; AI_QUESTION_SUBSTR[i]; ++i)
        if (strstr(w, AI_QUESTION_SUBSTR[i])) return 1;
    return 0;
}
/* 这句话里有没有疑问词（有一个就算问句） */
static inline int ai_has_question_word(char *const *words, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (ai_token_has_question_word(words[i])) return 1;
    return 0;
}
/* §6.532 **答复组织下沉成数据**（`[say] 键=模板`）：把"结论怎么说"从 derive.h 的判断里搬出来。
 *
 * 为什么：`对` / `不 对` / `结论 ， 推导链` / `大概 吧 ， …` 这些措辞原来写死在 `der_say`。
 * 改一句措辞、加一种说法都要改代码 —— 而"答复组织"和"外壳"一样，是语言件，不是引擎。
 * 现在：`patterns.txt` 的 `[say]` 行是唯一来源，模板里 `$1`/`$2` 是槽位
 * （`$1` = 推导链或判词/数字，`$2` = 第二槽：链或跳数）。没有 patterns.txt 时退回下面的种子表。
 *
 * 键名是**语义位**，不是句子：`判词.对` / `判词.不对` / `判词.不定` / `组织.定` / `组织.符号` /
 * `组织.猜一` / `组织.猜多` / `组织.假设` / `组织.缺` / `组织.空`。 */
/* §6.627 128 → 160：加"亲属/原因"两条模板后撞上限（第 129 行被丢，selfcheck 当场抓到）。
 * 这次多留一些：模板还会长。 */
#define AI_SAY_MAX 160
#define AI_SAY_KEY_LEN 40   /* §6.540 24 → 40：`不知道.没听他说过` 是 25 字节，24 会静默丢行 */
#define AI_SAY_TXT_LEN 340   /* §6.542 220 → 340：`删除.不行` 约 240 字节，220 会静默丢掉整行 */
static char g_say_key[AI_SAY_MAX][AI_SAY_KEY_LEN];
static char g_say_txt[AI_SAY_MAX][AI_SAY_TXT_LEN];
static size_t g_say_n;
/* §6.563 撞名计数：加载时检测，撞名的后一行丢弃（先出现的赢）。
 * 标 unused：探针程序（tools/microtest.c 等）只 include 到 ai.h、不加载数据 ⇒ 否则 -Werror 编译不过（同 §6.541）。 */
#if defined(__GNUC__)
#define AI_SAY_MAYBE_UNUSED __attribute__((unused))
#else
#define AI_SAY_MAYBE_UNUSED
#endif
static size_t g_say_dup AI_SAY_MAYBE_UNUSED;
static int g_say_data;      /* 1 = patterns.txt 读到了（数据接管；缺席的键走"退化"而不是种子） */
static const struct { const char *k, *v; } AI_SAY_SEEDS[] = {
    { "判词.对",   "对" },
    { "判词.不对", "不 对" },
    { "判词.不定", "不 确定" },
    { "组织.定",   "$1 ， $2" },
    { "组织.符号", "$1 ： $2" },
    { "组织.猜一", "大概 吧 ， $1 （其中一环是**推测**的，不是知道）" },
    { "组织.猜多", "大概 吧 ， $1 （其中 $2 环是**推测**的，不是知道）" },
    { "组织.假设", "在 假设 下 ： $1 ， $2" },
    { "组织.缺",   "推不出来（链上缺 $1 步，不能下结论）" },
    { "组织.空",   "推不出来" },
    { NULL, NULL }
};
/* 查一条答复模板：数据里有就用数据；数据接管了但没有这一条 ⇒ NULL（调用方走退化形态）；
 * 完全没数据（没有 patterns.txt）⇒ 用种子（保证套件在没有数据的临时目录里也照常回答）。 */
static inline const char *ai_say_tpl(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < g_say_n; ++i)
        if (!strcmp(g_say_key[i], key)) return g_say_txt[i];
    if (g_say_data) return NULL;
    for (size_t i = 0; AI_SAY_SEEDS[i].k; ++i)
        if (!strcmp(AI_SAY_SEEDS[i].k, key)) return AI_SAY_SEEDS[i].v;
    return NULL;
}

/* ============================ 参数区（ai.h 这一层的上界） ============================
 *
 * 按项目的「不要滥用硬编码」：**参数**（上界/阈值/频率）留在 `#define` 里，
 * 但要**集中、有注释**，能一条命令改的就给一条命令 —— 不搬进数据文件
 * （搬进去就把「调参」变成「改数据」，反而更难复现）。
 *
 * 这些常量原来散在结构体定义之间（上界跟着结构体走），改一个要满文件找；
 * 现在集中在这里，一处就能看全 ai.h 这一层的账。其它层各自的参数区：
 *   `rel.h`   REL_MAX_DEPTH / REL_MAX_PROOF / REL_EXCL_* / REL_GUESS_SIM
 *   `derive.h` DER_MAX_STEP（链最多几步）
 *   `think.h`  THINK_DEF_STEPS / THINK_STALL_LIM / THINK_MAX_OPEN /
 *              THINK_MAX_GOAL / THINK_CACHE_MAX（步数预算可用 `::thinkbudget` 改）
 *   `micro.h`  预算上限（MOD_BUDGET_*）、检测器阈值（D1/D2/D3）
 *   `frame.h`  FR_ASSOC_SIM_MIN（Gate B 的 Dice 门槛）
 */
#define AI_MAX_TOPICS    80     /* 语料 [话题:N] 的上限（话题票表） */
#define AI_MAX_SENT      24     /* 整句检索：一句话最多几个词 */
#define AI_SLOT_SIG       8     /* 同位槽类签名长度：左边最多 4 个、右边最多 4 个邻居 */
#define AI_MAX_SLOTS   1024     /* 同位槽类的上限 */
#define AI_MAX_FOCUS      4     /* 一次「注意」最多几个焦点词 */
#define AI_MAX_PATH      64     /* 逐词游走的路径上限（联想链最长走多少步） */
#define AI_MAX_OTHERS     8     /* 最多维护几个「他者」模型 */
#define AI_MAX_BELIEF    24     /* 每个「他」名下最多记几条信念 */
#define AI_MAX_HISTORY   32     /* 内部对话/想象的历史环长度 */
#define COT_MAX_STEP     10     /* 思维链最多记几步（一步=读入/骨架/校验/降级/成句之一） */
#define AI_MAX_HYPO       8     /* 一层假设最多几条假设事实 */
#define AI_MAX_HYPO_DEPTH 4     /* 假设最多嵌套几层 */
/* 因果猜想（「我说了 X，对方接着说 Y」）：
 * **一次巧合不算规律** —— 见够 MIN 次才从「暂时」升格成「可信的猜想」，
 * 而且重复有上限（见得多不代表能把它刷成事实）。反事实（我没说 X，Y 照样发生）
 * 会削弱它：hits 掉到 0 变「暂时」，在「暂时」上再来一次反例就雪藏。
 * 它永远只是**猜想**：带 [因果] 标记、不参与走边、不进关系表（见 ai_learn_causal）。 */
#define AI_CAUSAL_MIN_HITS 3    /* 协变几次才升格（参数，要调就调这里） */
#define AI_CAUSAL_HITS_CAP 8    /* 重复计数上限：防止反复出现把它刷成「事实」 */

/* §6.135 睡眠回放的**访问上限**（默认值；`ai->sleep_max_visits` 可以按脑子改，看护要用）。
 * 理由：`act_seen()` 只看当前 DFS 路径 ⇒ 同一节点被不同路径反复激活 ⇒ 代价随深度指数增长。
 * 实测（出厂模型，每个词的激活耗时）：深度 2 = 14.9 ms、深度 3 = **1648 ms**；睡眠用**深度 6**，
 * 于是它实际上**永远跑不完**（`roundbench` 默认 200 轮卡死 5 分钟以上没动静）。
 * 取 20000：睡眠仍能扩散到几千个节点（深度 3 那次的访问量就在这个量级），但每一步都有硬顶。
 * 实测代价（出厂模型，一次完整睡眠 = 32 个词）：640000 次访问 / 47223 次截断、约 **0.6 s**。 */
#define AI_SLEEP_MAX_VISITS 20000

/* §6.139 **脏思考检查器**（用户点名要的）：说出口的话**连续重复**就当场截停。
 * 判据（都在**输出**这一层，按空格切词，只看前 `AI_DIRTY_MAXW` 个词）：
 *   ① **词级**：同一个词连着出现 >= `AI_DIRTY_RUN` 次 ⇒ 从**第二次**那里截（前面留一份）；
 *   ② **句级**：连续两段**完全相同**的片段（周期 2..`AI_DIRTY_PERIOD` 个词）
 *      —— 「你好 吗 你好 吗」⇒ 从第二段开头截。
 * 两条取**最早**的截点。只**截**不改写：前面干净的部分留着；截光了调用方回「……」。
 * 为什么要有它：思维循环里已经有"原地打转"（**步数**没推进，见 think.h），
 * 但那是**过程**；"说出口的话在重复"这条以前**没有**看护。 */
#define AI_DIRTY_RUN     3
#define AI_DIRTY_PERIOD  8
#define AI_DIRTY_MAXW    64
typedef struct { size_t off, len; } AiDirtyWord;

/* 返回该从哪里截（0 = 没脏）。`kind_out`：1 = 词级连重，2 = 句级周期重复。 */
static inline size_t ai_dirty_cut(const char *out, size_t *kind_out) {
    if (kind_out) *kind_out = 0;
    if (!out) return 0;
    AiDirtyWord w[AI_DIRTY_MAXW];
    size_t n = 0;
    for (const char *p = out; *p && n < AI_DIRTY_MAXW; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *st = p;
        while (*p && *p != ' ') p++;
        w[n].off = (size_t)(st - out); w[n].len = (size_t)(p - st); n++;
    }
    if (n < 2) return 0;
    size_t cut = 0;
    for (size_t i = 0; i + 2 < n; ++i) {                 /* ① 词级连重 */
        if (w[i].len == w[i + 1].len && w[i+1].len == w[i+2].len &&
            memcmp(out + w[i].off, out + w[i+1].off, w[i].len) == 0 &&
            memcmp(out + w[i].off, out + w[i+2].off, w[i].len) == 0) {
            cut = w[i + 1].off; if (kind_out) *kind_out = 1; break;
        }
    }
    for (size_t pd = 2; pd <= AI_DIRTY_PERIOD && !cut; ++pd) {   /* ② 句级周期重复 */
        for (size_t i = 0; i + 2 * pd <= n; ++i) {
            int same = 1;
            for (size_t k = 0; k < pd; ++k) {
                if (w[i+k].len != w[i+pd+k].len ||
                    memcmp(out + w[i+k].off, out + w[i+pd+k].off, w[i+k].len) != 0) { same = 0; break; }
            }
            if (same) { cut = w[i + pd].off; if (kind_out) *kind_out = 2; break; }
        }
    }
    return cut;
}

/* 话题票：某话题下「出现过哪些词」。生成时用输入词投票选话题。 */
typedef struct { Token *seen[192]; size_t n; uint32_t votes; } AITopic;

/* §6.135 **每轮分段计时**（默认关，`AIP_SEGTIME=1`）—— 度量钩子，不进业务逻辑。
 * 为什么要自己量：§6.132 之后每轮 ~1.94 ms，但 gprof 只看得见 `main`
 * （`agent_step` 在 `main.c` 里是内联的），`perf` 在本机被 `perf_event_paranoid=4` 挡住。
 * 口径 = `agent_step` 的 11 个**自然段**（段名见 `AI_SEG_NAMES`），`ai_seg_lap()` 
 * 逐段累加纳秒与次数。关着时每个 lap 只多一条可预测分支（行为逐字节不变）。 */
enum {
    AI_SEG_HEAD = 0,   /* 条件缓存复查 + 驱动折算 */
    AI_SEG_TOK,        /* 分词 + 接地（`ai_tokenize`/逐字模式） */
    AI_SEG_LEARN,      /* 反馈/他者/反事实/因果/预测逐对 + 内生状态 + [时刻:] 链 + 焦点 */
    AI_SEG_INNER,      /* 内部对话（`ai_inner_dialogue`）*/
    AI_SEG_IMAGINE,    /* 想象（`ai_imagine`）*/
    AI_SEG_GEN,        /* 生成种子/焦点 + 话题投票（`ai_detect_topic`）*/
    AI_SEG_THINK,      /* COT 开头 + 思维循环（前段）+ 自由联想 */
    AI_SEG_REL,        /* 关系层（L2）：陈述即学、问关系即推、类比兜底 */
    AI_SEG_LOOKUP,     /* 整句检索优先（含帧 / 逐词游走兜底） */
    AI_SEG_TAIL,       /* 收尾留痕 + 元认知/表达模式 + 接管输出 + 自举 + 时间推进 */
    AI_SEG_KERNEL,     /* 微内核安全点 + 对话触发 + 过程 + 输出侧补丁 */
    AI_SEG_MAINT,      /* 维护：巩固 / GC / 睡眠 */
    AI_SEG_NARR,       /* 漂移 / 诊断 / 叙事 + 释放 */
    AI_SEG_N
};
static const char *const AI_SEG_NAMES[AI_SEG_N] = {
    "头（缓存复查+驱动）", "分词+接地", "学习+内生+时刻链+焦点", "内部对话", "想象",
    "生成+话题投票", "思维前段+自由联想", "关系层(L2)", "整句检索", "收尾+元认知+输出+自举",
    "内核安全点+触发+补丁", "维护(巩固/GC/睡眠)", "漂移/诊断/叙事"
};


/* 「我不知道」的说法池。
 * 关键设计：检索**必须能拒答**。之前不管输入是什么都硬塞一个最像的句子，
 * 于是「1加1等于几」会得到「先喝口水我在这里」—— 这比答不上来糟糕得多。
 * 匹配不到就诚实说不知道，这才是正常交流。 */
/* §6.537 **说法池下沉**：这 6 句原来写死在这里；现在 `patterns.txt` 的 `[refuse] 说法=…` 是唯一来源，
 * 代码里这份只当**没有数据时**的种子。选哪一句仍是代码（按 `n_refusals` 轮换，§6.420）——
 * "说什么" 是语言，"怎么轮换" 是算法。 */
#define AI_REFUSE_MAX 12
#define AI_REFUSE_LEN 64
static char g_refuse[AI_REFUSE_MAX][AI_REFUSE_LEN];
static size_t g_refuse_n;
static int g_refuse_data;
static const char *const AI_REFUSALS[] = {
    "这个我 不 太 懂",
    "我 不 知道 你 能 告诉 我 吗",
    "我 还 没 学 过 这个",
    "我 想 想 好像 不 太 会",
    "这 个 问题 我 答 不 上",
    "我 不 太 确定 你 说 给 我 听",
    NULL
};
static inline size_t ai_refuse_count(void) {
    if (g_refuse_data) return g_refuse_n;
    size_t n = 0;
    while (AI_REFUSALS[n]) n++;
    return n;
}
static inline const char *ai_refuse_at(size_t i) {
    if (g_refuse_data) return (g_refuse_n && i < g_refuse_n) ? g_refuse[i % g_refuse_n] : NULL;
    size_t n = 0;
    while (AI_REFUSALS[n]) n++;
    return n ? AI_REFUSALS[i % n] : NULL;
}

/* 整句检索：语料里「输入句 → 回复句」的成对记忆。
 *
 * 为什么要有它：逐词游走只能用「边的局部权重」决定下一个词，走到第三个词就开始漂。
 * 整句检索换的是另一件事 —— 把语料里的整句当成候选，用输入去匹配，
 * 匹配上就把那一整句原样吐出来。这是「记忆复述」而不是「生成」。
 */
typedef struct {
    Token  *in[AI_MAX_SENT];  size_t in_n;   /* 输入句 */
    Token  *out[AI_MAX_SENT]; size_t out_n;  /* 回复句 */
    uint8_t topic;
} AISentPair;

/* 同位槽类：「在相同上下位里出现」的词算同一类。
 * 例：`我 喜欢 猫` 和 `我 喜欢 狗` 里，猫/狗 的左邻都是 `喜欢`、右邻都是句尾
 *     → 同一个槽类 → 可以互换。
 * 生成时用它做**模板替换**：把学过的句子当骨架，槽位换成语料里没这样配过的词，
 * 于是说出语料里没有的新句子，而句子骨架仍是学来的、所以通顺。 */
typedef struct {
    Token *members[48];
    size_t n;
    uint32_t sig;           /* 签名哈希，用来快速归类 */
} AISlot;


/* ============================ 思维链 ============================
 *
 * 这一轮里**每一步决策**都记下来：在做什么、成没成、为什么。
 *
 * 要点是它不能只是一份日志：后一步必须真的读前一步的结果。
 * 现在的链条是这么咬合的 ——
 *
 *   读入 ──→ 骨架 ──→ 校验 ──→ 降级 ──→ 成句
 *                      │        ↑
 *                      └── 失败才走降级（成功直接成句，降级不参与）
 *   联想 ─┘（给校验提供「这个槽位取值像不像这一类」的证据）
 *   检索 ──┘（骨架和降级都没话说时才轮到它）
 *
 * 也就是说「降级」这一步**只因为「校验」失败才存在**，「检索」**只因为
 * 前面都没话说才存在** —— 这才叫链，不是并列地试四种策略。
 */
typedef enum {
    COT_READ = 1,   /* 读入：分词 + 符号节点代数（拆解/合并/新建/中转） */
    COT_ASSOC,      /* 联想：沿关联链想一遍，拿到候选 */
    COT_PARSE,      /* 骨架：认出哪个帧、哪个槽位 */
    COT_GATE,       /* 校验：槽位取值的类证据够不够 */
    COT_FALL,       /* 降级：证据不够 → 改用不需要证据的诚实答复 */
    COT_LEARN,      /* 记下：用户陈述了一条关系事实，现场学进图里 */
    COT_INFER,      /* 推理：用关系规则推出来的结论（带推导路径） */
    COT_HYP,        /* 假设：进入/退出反事实假设世界 */
    COT_VIEW,       /* 视角：站到别人的立场上推理 */
    COT_RECALL,     /* 检索：整句命中及其分数/覆盖率 */
    COT_ADMIT,      /* 认输：拒答 + 理由 */
    COT_DONE        /* 收尾：最终说了什么 */
} CotKind;

typedef struct {
    CotKind kind;
    int     ok;            /* 这一步成了吗 */
    int     score;         /* 把握 0~100 */
    char    what[80];      /* 在做什么 */
    char    why[160];      /* 为什么 / 为什么没成（关系推导链会占满） */
} CotStep;

typedef struct {
    CotStep step[COT_MAX_STEP];
    size_t  n;
} ChainOfThought;

static inline const char *cot_kind_name(CotKind k) {
    switch (k) {
        case COT_READ:   return "读入";
        case COT_ASSOC:  return "联想";
        case COT_PARSE:  return "骨架";
        case COT_GATE:   return "校验";
        case COT_FALL:   return "降级";
        case COT_LEARN:  return "记下";
        case COT_INFER:  return "推理";
        case COT_HYP:    return "假设";
        case COT_VIEW:   return "视角";
        case COT_RECALL: return "检索";
        case COT_ADMIT:  return "认输";
        case COT_DONE:   return "收尾";
        default:         return "?";
    }
}


typedef struct {
    float predict_accuracy, surprise_level;
    float comfort, discomfort, arousal, fatigue;
} AIInner;

typedef struct { float setpoint_comfort, setpoint_arousal, regulation_rate; } AISetpoint;
typedef struct {
    uint64_t gc_min_age, gc_time_scale;
    float gc_upgrade_rate;
    size_t gc_interval;
    uint8_t gc_delete_state;
} AIGCConfig;
typedef struct {
    float w_gap, w_conflict, w_activation, w_effort, w_novelty;
    float certain_threshold, unknown_threshold;
} UncertainConfig;
typedef struct {
    float confidence, familiarity, fluency, effort, gap_feel, total;
} MetaCog;
typedef enum {
    EXPR_CONFIDENT = 0, EXPR_HEDGE, EXPR_TENTATIVE, EXPR_UNCERTAIN,
    EXPR_LOST, EXPR_CURIOUS, EXPR_SILENT, EXPR_COUNT
} ExprMode;
typedef struct { Token *path[AI_MAX_PATH]; size_t n; } AIPath;

/* 写入来源：**一张表**（谁在改图）。加一个来源 = 表里加一行。 */
typedef enum {
    AI_WSRC_PERCEIVE = 0,   /* 感知：对话里学到的共现（含 [时刻:] 链） */
    AI_WSRC_RELATION,       /* 关系层现场学：听来的「X R Y」 */
    AI_WSRC_INNER,          /* 内部对话 */
    AI_WSRC_IMAGINE,        /* 想象 */
    AI_WSRC_SLEEP,          /* 睡眠巩固 */
    AI_WSRC_DECAY,          /* 衰减 / 巩固 */
    AI_WSRC_MICRO,          /* 微内核：受审通过的意图落地 */
    /* §6.580 **外来**：另一个智能体搬过来的事实（`::parts replay` 的照抄路径）。
     * 单列一个来源，是为了 `::mods` 里能一眼看出"这条不是我学的，是别的脑带过来的"，
     * 也让"本地学 / 外来带"两类写入的条数各自可数。 */
    AI_WSRC_REPLAY,
    AI_WSRC_N
} AiWriteSrc;

static inline const char *ai_write_src_name(AiWriteSrc s) {
    switch (s) {
    case AI_WSRC_PERCEIVE: return "感知";
    case AI_WSRC_RELATION: return "关系学习";
    case AI_WSRC_INNER:    return "内部对话";
    case AI_WSRC_IMAGINE:  return "想象";
    case AI_WSRC_SLEEP:    return "睡眠";
    case AI_WSRC_DECAY:    return "衰减";
    case AI_WSRC_MICRO:    return "受审";
    case AI_WSRC_REPLAY:   return "外来";
    default:               return "?";
    }
}
/* ============================ §6.564 每个智能体自己的修改账 ============================
 *
 * 目标（用户 2026-10-24）：**打开多个智能体，每个智能体只维护自己的修改**。
 * 第一刀不是"隔离进程"，而是把"我改过什么"变成**每个 AI 自己的一本账**：
 *   · 账在 `AI` 里（堆上），**不是全局的** ⇒ 两个 AI 天然各记各的；
 *   · 只记"落地的写"（`ai_write_rel` 成功、`ai_write_assoc` 新加边），不记意图；
 *   · 有界环形缓冲（`AI_MODLOG_MAX` 条），溢出只丢最旧的 —— 账不能把内存吃干；
 *   · 存的是 **Token id 与关系 id**（不是指针）：跨池/跨 agent 也认得，且不进存档。
 * 这层是后面三件事的地基：`::mods`（看自己的账）、局部加载（谁改了哪块图要能追）、
 * 合并/回放（两本账能不能并成一本）。
 */
#define AI_MODLOG_MAX 256u

typedef struct {
    uint32_t a_id, b_id;    /* Token.id（稳定、跨池可比） */
    uint32_t seq;           /* 本 agent 的写入序号（从 1 开始，单调） */
    uint8_t  rid;           /* 关系 id（0 = 无/不适用，如纯共现边） */
    uint8_t  neg;           /* 1 = 写的否定边 */
    uint8_t  src;           /* AiWriteSrc */
    uint8_t  kind;          /* AIMOD_REL / AIMOD_ASSOC */
} AIModEntry;

typedef enum {
    AIMOD_REL = 1, AIMOD_ASSOC = 2, AIMOD_REL_DEL = 3, AIMOD_ASSOC_DEL = 4,
    /* §6.580 跨脑知识语义（`xbrain.h`）：外来事实分三种落法。
     * 记账要把"带过来了"和"被本地知识挡住了"分开 —— 否则一致性问题不可见。 */
    AIMOD_REL_COPY = 5,       /* 照抄进来（本地原来没有） */
    AIMOD_REL_CONFLICT = 6,   /* 与本地知识冲突 ⇒ **没写**（只记账，事后可查） */
    AIMOD_REL_OVERRIDE = 7    /* `force`：明知冲突还是写了（覆盖/并列） */
} AIModKind;

/* 账里那一条是"什么动作"（`::mods` / `::brain mods` 共用，只此一处） */
static inline const char *ai_modlog_kind_name(unsigned kind) {
    switch (kind) {
    case AIMOD_REL:          return "加关系";
    case AIMOD_ASSOC:        return "加共现";
    case AIMOD_REL_DEL:      return "删关系";
    case AIMOD_ASSOC_DEL:    return "删共现";
    case AIMOD_REL_COPY:     return "照抄";
    case AIMOD_REL_CONFLICT: return "冲突未写";
    case AIMOD_REL_OVERRIDE: return "强行覆盖";
    default:                 return "?";
    }
}

typedef struct {
    AIModEntry ent[AI_MODLOG_MAX];
    size_t     n;           /* 累计条数（含被覆盖的）*/
    size_t     head;        /* 下一个要写的槽位 */
} AIModLog;

typedef struct {
    Token  *a;
    Token  *b;
    uint8_t rid;
    uint8_t neg;     /* 1 = 假设「不 R」 */
} HypoFact;

/* 一个「别人」的模型。
 * 原来只有 who/trust/n_interactions 三个字段，谁都没用 —— 「他 以为 X」只是给假设层
 * 贴个标签，轮末就没了，所以「归属」一直只是**标注**。
 * 现在这里多了一张**他的信念表**：他说过的事实存在他名下（写进模型文件 v8），
 * 问「他 觉得 … 吗」查的是它，而我自己的知识一点都不受影响 ——
 * 他信什么是他的事，**我的视角永远不会因此改变**。 */
/* **他者的改口历史**（§6.112，v13 存盘）：`bel[]` 是**覆盖语义**（只说最新那条），
 * 这里补上"他改过几次、最近三次改成什么、什么时候改的"。与 `bel[]` **同下标**
 * （信念只追加、不删除 ⇒ 下标稳定 ✓）。 */
#define AI_MAX_BELIEF_REV 3
typedef struct {
    uint8_t  old_neg[AI_MAX_BELIEF_REV];   /* 那一次改口**之前**是什么（0=认 / 1=不认） */
    uint32_t tick[AI_MAX_BELIEF_REV];
    uint8_t  n;                            /* 记了几条（≤3，满了滚最新） */
    uint32_t total;                        /* 一共改过几次（单调计数，用来回答"改口几次"） */
} BelHist;

typedef struct {
    Token *who;
    Token *belief_of_self;
    float  trust;
    size_t n_interactions;
    HypoFact bel[AI_MAX_BELIEF];   /* 他相信的事实（不一定真） */
    BelHist  hist[AI_MAX_BELIEF];  /* 与 bel[] 同下标的改口历史（§6.112） */
    size_t   n_bel;
    size_t   n_bel_learned;        /* 累计听他说过多少条 */
} OtherModel;


typedef struct {
    float  prev_lexical_diversity;
    size_t prev_perspective_n, prev_abstractions;
    size_t prev_imaginations, prev_imagine_useful;
    uint64_t last_diag_tick;
} DiagState;
typedef struct {
    size_t n_confident, n_confident_wrong;
    float  calibration_bias;
} CalibState;

/* ============================ 推导（Derivation） ============================
 * 类型定义放在这里（AI 结构里要用），逻辑在 derive.h 末尾。
 * 立场：**链才是产物，文字只是它的投影**。 */
/* ============================ 假设层（反事实） ============================
 *
 * 「如果 猫 会 飞，那 猫 会 游泳 吗」需要一个**假设世界**：
 * 往里放几条临时事实，在这个世界里推理，然后**原样撤回**。
 *
 * 铁律：假设**绝不写进图**。它只活在 ai->hypo 这个覆盖层里，
 * 求解器查事实时先看覆盖层再看图；存盘不带它，::hypo clear 或换一句陈述就没了。
 * 违反这条（把假设 tag 进边里）就会污染知识库 —— 那是最恶劣的一类 bug：
 * 用户只是随口「如果」，系统却从此以为真。
 *
 * 每个假设事实在推导链上会被标成 DER_HYP，答复里写明「在假设下」。 */

/* 一层假设 = 一组事实 + 这层归谁。
 * `who` 就是「如果他以为…」里的那个「他」：NULL 表示「我自己的假设」。
 * 多层叠起来就是一个栈 —— 「他以为我以为猫会飞」= 两层，
 * 外层 who=他、内层 who=我。求解时**所有层都算数**（内层不能推翻外层，
 * 那正是「他以为」的意思）。 */
typedef struct {
    HypoFact f[AI_MAX_HYPO];
    size_t   n;
    Token   *who;
} HypoFrame;

typedef struct {
    HypoFrame fr[AI_MAX_HYPO_DEPTH];
    size_t    depth;
} Hypo;

static inline void hypo_clear(Hypo *h) {
    if (!h) return;
    for (size_t i = 0; i < h->depth; ++i) { h->fr[i].n = 0; h->fr[i].who = NULL; }
    h->depth = 0;
}
/* 压一层新的假设（who 可以是 NULL） */
static inline int hypo_push(Hypo *h, Token *who) {
    if (!h || h->depth >= AI_MAX_HYPO_DEPTH) return 0;
    HypoFrame *f = &h->fr[h->depth++];
    f->n = 0; f->who = who;
    return 1;
}
/* 弹掉**最深**那一层（「算了」= 撤掉最近那条假设） */
static inline int hypo_pop(Hypo *h) {
    if (!h || h->depth == 0) return 0;
    h->depth--;
    h->fr[h->depth].n = 0;
    h->fr[h->depth].who = NULL;
    return 1;
}
/* 抽掉指定的那一层（0 起）—— 这是「作用域回溯」：不用非得从最深往下弹。
 * 中间抽掉之后，它上面的层整体下移。 */
static inline int hypo_drop(Hypo *h, size_t idx) {
    if (!h || idx >= h->depth) return 0;
    for (size_t i = idx; i + 1 < h->depth; ++i) h->fr[i] = h->fr[i + 1];
    h->depth--;
    h->fr[h->depth].n = 0;
    h->fr[h->depth].who = NULL;
    return 1;
}

/* 哪一层里有这条假设事实？（0 起；-1 = 没有）
 * 「撤回 猫 属于 鸟」要找的就是它 —— 作用域回溯得能指名道姓。 */
static inline int hypo_layer_of_fact(const Hypo *h, const Token *a, uint8_t rid,
                                     const Token *b) {
    if (!h || !a || !b) return -1;
    for (size_t k = 0; k < h->depth; ++k) {
        const HypoFrame *f = &h->fr[k];
        for (size_t i = 0; i < f->n; ++i)
            if (f->f[i].a == a && f->f[i].rid == rid && f->f[i].b == b) return (int)k;
    }
    return -1;
}
/* 哪一层提到了这个词（主语或宾语都算；从深往浅找，先撤最近说的那条） */
static inline int hypo_layer_of_subject(const Hypo *h, const Token *t) {
    if (!h || !t) return -1;
    for (size_t k = h->depth; k-- > 0; ) {
        const HypoFrame *f = &h->fr[k];
        for (size_t i = 0; i < f->n; ++i)
            if (f->f[i].a == t || f->f[i].b == t) return (int)k;
    }
    return -1;
}

/* 撤掉最深那一层里的**某一条**假设（回退用）。
 *
 * 为什么需要它：`hypo_pop` 只能整层弹，"这一步的假设撤了、上一步的还在"做不到 ——
 * 而过程执行（第 9 条 ④）的回退恰恰是**按步**撤：一步失败，只撤这一步刚加的那些假设。
 * 返回 1 = 撤掉了（后面的条目往前挪，不留空位）。 */
static inline int hypo_del(Hypo *h, const Token *a, const Token *b, uint8_t rid) {
    if (!h || h->depth == 0 || !a || !b || rid == 0) return 0;
    HypoFrame *f = &h->fr[h->depth - 1];
    for (size_t i = 0; i < f->n; ++i) {
        if (f->f[i].a != a || f->f[i].b != b || f->f[i].rid != rid) continue;
        for (size_t k = i + 1; k < f->n; ++k) f->f[k - 1] = f->f[k];
        f->n--;
        memset(&f->f[f->n], 0, sizeof f->f[f->n]);
        return 1;
    }
    return 0;
}

/* 往最深那一层加一条事实 */
static inline int hypo_add(Hypo *h, Token *a, Token *b, uint8_t rid, int neg) {
    if (!h || !a || !b || rid == 0) return 0;
    if (h->depth == 0 && !hypo_push(h, NULL)) return 0;
    HypoFrame *f = &h->fr[h->depth - 1];
    /* 同一层里已经有一条对同一对词、同一关系的假设 → 覆盖它（不叠加矛盾） */
    for (size_t i = 0; i < f->n; ++i) {
        if (f->f[i].a == a && f->f[i].b == b && f->f[i].rid == rid) {
            f->f[i].neg = (uint8_t)(neg ? 1 : 0);
            return 1;
        }
    }
    if (f->n >= AI_MAX_HYPO) return 0;
    HypoFact *fct = &f->f[f->n++];
    fct->a = a; fct->b = b; fct->rid = rid; fct->neg = (uint8_t)(neg ? 1 : 0);
    return 1;
}
/* 覆盖层里有没有这条？从**最深**往浅找（内层优先），*out_who 给出这层归谁。 */
/* 视角过滤：view_who == NULL 表示「我自己」—— 所有层都算数（我知道我全部的假设，
 * 包括我以为他怎么想）。view_who == X 表示「站在 X 的立场」—— 只有归在 X 头上的
 * 那几层算数，**我的假设他不知道**。这就是最小可用的视角推理：
 * 同一个问题换个立场，答案可以不同，而且原因清楚（哪些假设他看不见）。 */
static inline int hypo_frame_visible(const HypoFrame *f, const Token *view_who) {
    if (!view_who) return 1;                 /* 我的视角：全都算 */
    return f->who == view_who;               /* 他的视角：只算归他的层 */
}

static inline const HypoFact *hypo_find(const Hypo *h, const Token *view_who,
                                        const Token *a,
                                        uint8_t rid, const Token *b,
                                        Token **out_who) {
    if (!h) return NULL;
    for (size_t k = h->depth; k-- > 0; ) {
        const HypoFrame *f = &h->fr[k];
        if (!hypo_frame_visible(f, view_who)) continue;
        for (size_t i = 0; i < f->n; ++i) {
            if (f->f[i].a != a || f->f[i].b != b || f->f[i].rid != rid) continue;
            if (out_who) *out_who = f->who;
            return &f->f[i];
        }
    }
    if (out_who) *out_who = NULL;
    return NULL;
}
/* 覆盖层里所有「以 a 为主语、关系 rid、极性 neg_want」的事实（所有层都算） */
static inline size_t hypo_edges(const Hypo *h, const Token *view_who,
                                const Token *a, uint8_t rid,
                                int neg_want, const HypoFact **out, size_t cap) {
    size_t n = 0;
    if (!h) return 0;
    for (size_t k = 0; k < h->depth && n < cap; ++k) {
        const HypoFrame *f = &h->fr[k];
        if (!hypo_frame_visible(f, view_who)) continue;
        for (size_t i = 0; i < f->n && n < cap; ++i) {
            const HypoFact *x = &f->f[i];
            if (x->a != a || x->rid != rid) continue;
            if ((int)x->neg != (neg_want ? 1 : 0)) continue;
            int dup = 0;
            for (size_t q = 0; q < n; ++q) if (out[q] == x) { dup = 1; break; }
            if (!dup) out[n++] = x;
        }
    }
    return n;
}
/* 在这个视角下**看得见**的假设条数（不是总条数）。
 * 「他 觉得 猫 属于 鸟 吗」时，我的假设他看不见 —— 所以那一刻
 * 「在假设下」这个说法根本不该出现（实测第一版就答成
 * 「在 他 看来 ： 在 假设 下 我 推不出来」，自相矛盾）。 */
static inline size_t hypo_visible_n(const Hypo *h, const Token *view_who) {
    if (!h) return 0;
    size_t n = 0;
    for (size_t k = 0; k < h->depth; ++k)
        if (hypo_frame_visible(&h->fr[k], view_who)) n += h->fr[k].n;
    return n;
}

/* 当前最深那层归谁（链上要标「他 以为」） */
static inline Token *hypo_cur_who(const Hypo *h) {
    if (!h || h->depth == 0) return NULL;
    return h->fr[h->depth - 1].who;
}

#define DER_MAX_STEP 24

/* 渲染风格：**语言是最后一步的事**，同一份链可以这么说也可以那么说。
 * （完整说明在 derive.h 的 der_say。） */
#define DER_STYLE_TERSE  0   /* 只给结论 */
#define DER_STYLE_PLAIN  1   /* 结论 + 推导链（默认） */
#define DER_STYLE_CHAIN  2   /* 只给链，不给结论 */
#define DER_STYLE_SYMBOL 3   /* 符号化 */

typedef enum {
    DER_GOAL = 1,
    DER_FACT,       /* 图里的直接**观测**事实（可被验证器独立复查） */
    /* **以前推出来、沉淀下来的**（边上标着 ASSOC_ROLE_DERIVED）。
     * 它和观测事实要分开：证据强度更低（可能已经过时），
     * 而且被观测推翻时允许撤销。验证器会独立复查「这条缓存还在不在」。 */
    DER_KNOWN,
    DER_RULE,       /* 规则应用：传递/继承/对称/逆关系 */
    DER_DEFAULT,    /* 默认推测（类比，非单调） */
    DER_SOURCE,     /* **非逻辑路径的来源**：整句记忆 / 结构化帧 / 降级答复。
                     * 它不做任何断言，所以不参与真值，也不算缺口。 */
    DER_HYP,        /* **靠假设成立的一步**（反事实）。验证器会去覆盖层里核对：
                     * 覆盖层里没有它，就是伪造 → 缺口。 */
    DER_GAP,        /* 缺口：这一步没有依据 */
    DER_CONCLUDE
} DerKind;

typedef struct {
    DerKind kind;
    uint8_t rule;        /* RULE 时：1 传递 2 继承 3 对称 4 逆关系 */
    Token  *subj;        /* 这一步讲的是 subj --[rel]--> obj */
    Token  *obj;
    Token  *src;         /* DEFAULT 时：依据是哪个词（最近邻） */
    uint8_t rid;
    uint8_t neg;         /* 这一步是不是否定 */
    int     truth;       /* +1 真 -1 假 0 未知（GAP 恒为 0） */
    int     sim;         /* DEFAULT 时的相似度 */
    int     verified;    /* 验证器填：这一步验过没有 */
    /* 缺口原因 / 补充说明。**别按「字符数」估大小** —— 中文一个字 3 字节，
     * 早先给 64 字节，一句「否定它需要完备性……」就被截在字符中间，
     * 打出来是乱码（实测）。给足 224 字节。 */
    char    note[224];
} DerStep;

typedef struct {
    DerStep step[DER_MAX_STEP];
    size_t  n;
    int     truth;       /* 整条链的结论真值 */
    int     gaps;        /* 缺口数；>0 就不许下断言 */
    int     guessed;     /* 用到过 DEFAULT */
    /* §6.131 **这条链里有几环是"记忆里的猜测"**（不是"有没有"，是"几环"）。
     * 为什么需要计数：猜一次和猜两次不是一回事 —— 置信度要**按跳数衰减**，
     * 话也要说成「其中两环是推测的」。只用一个 bool 的话，链越接越长、把握却纹丝不动。 */
    int     guess_hops;
    int     conf;        /* 0..100 */
    /* has_claim = 0 表示这一轮**没有逻辑结论**（输出走的是记忆/骨架/降级），
     * 这时 truth 不参与任何判断，链条只记录「话是从哪来的」。
     * 分开这两件事很重要：把「检索到一句像的话」包装成「推导出的结论」是撒谎。 */
    int     has_claim;
    char    source[64];  /* has_claim=0 时的来源说明 */
} Derivation;

typedef struct {
    TokenPool *pool;
    uint32_t rng, gc_rng;

    Token *self, *good, *bad, *unknowing, *causal;
    Token *social_good, *social_bad;
    Token *past_self, *future_self;
    Token *split_tok;               /* [原语:分割]：语料里「输入/回复」的边界 */
    AITopic topics[AI_MAX_TOPICS];  /* 话题表（显式标注累积） */
    uint8_t cur_topic;              /* 本次生成锁定的话题 */

    AISentPair *pairs;              /* 整句检索库 */
    size_t pairs_n, pairs_cap;
    int    use_retrieval;           /* 1 = 优先整句检索 */
    float *idf;                     /* 每个符号节点的 IDF 权重（越小越常见） */
    /* idf 数组的**实际长度**。必须单独记：ai->idf 是按「重建那一刻的
     * pool->count」分配的，而重建之后还会造词（关系词 intern、frame 造词、
     * 抽象符号节点）。原来 ai_idf_of() 拿 pool->count 当边界，于是新词会**越界读**
     * 到别的堆内容 —— 浮点垃圾值每次进程都不一样（地址相关），
     * 结果就是「装载态」和「训练态」的联想链、回复都不一样。
     * 症状隐蔽：整个词池 dump 逐字节相同，只有 idf 那 50 个词是垃圾。 */
    size_t idf_n;
    size_t n_refusals;              /* 「我不知道」次数，用来轮换说法 */
    size_t n_synth;                 /* 说出了多少句语料里没有的句子 */
    int    last_retrieval_cov;      /* 上一次检索的输入覆盖率 */
    size_t last_retrieval_len;      /* 命中那条输入句的词数（长度比用） */
    size_t last_input_len;
    int    last_token_ratio;        /* 命中的输入句里，有多少比例的输入词真的出现过 */
    AISlot slots[AI_MAX_SLOTS];     /* 同位槽类表 */
    size_t slots_n;
    size_t n_frames;                /* 结构化帧生成成功的次数 */
    size_t n_frame_assoc;           /* 其中「靠关联链证据放行槽位」才成功的次数 */
    size_t n_coined;                /* 其中「现场造词」造出来的新符号节点数 */
    size_t n_split;                 /* 其中「拆解切分」才匹配上的次数 */
    size_t n_merge;                 /* 其中「合并切分」才匹配上的次数 */
    size_t n_relay;                 /* 其中「中转词容错」生效的次数 */
    size_t n_hedge;                 /* 其中「降级答复」兜住的次数 */
    size_t n_rel_learned;           /* 对话里现场学到的关系事实条数 */
    /* §6.580 跨脑照抄的账（`::parts replay`）：新增/已知/冲突跳过/强行覆盖/坏行/新建词。
     * 分开计的理由：一次搬运的结论必须是"我带了 N 条，其中 M 条被本地知识挡住"，
     * 而不是一个笼统的"成功 N 条"。 */
    size_t n_xcopy_added, n_xcopy_known, n_xcopy_conflict, n_xcopy_override,
           n_xcopy_bad, n_xcopy_newwords;
    size_t n_revised;               /* 其中「改口」的次数：后说的观测替掉了先说的对立极性 */
    /* §6.585 学过的新事实**与已有独占值冲突**的次数（冲突不静默：回复里会说"跟以前记的哪条冲突"）。
     * 只进内存，给 `::stats`/探针看。 */
    size_t n_conflict_shown;
    /* §6.586 **追问式改口**的待确认状态（只进内存；和 §6.168 的追问回话一个性质）：
     * 刚学了一条冲突事实、问了"要 改 掉 旧 的 吗"，下一轮用户回「对」就撤旧值。
     * 存 id 不存指针（池可能扩容/搬动），`ask_retract_old == 0` 表示没有待确认的。 */
    uint32_t ask_retract_a, ask_retract_old;
    uint8_t  ask_retract_rid;
    size_t n_rel_yes;               /* 靠关系推理答「是」的次数 */
    size_t n_rel_no;                /* 靠关系推理答「不是」的次数 */
    size_t n_rel_deep;              /* 其中需要多跳推导（不是直接边）的次数 */
    size_t n_rel_unknown;           /* 三态里的「不知道」：不装懂，也不硬说不对 */
    size_t n_rel_guess;             /* 靠相似度**推测**出来的次数（明确标注是推测） */
    size_t n_cot;                   /* 思维链累计步数（看链有没有在长） */
    /* ---- 动态关系表：让谓词表**开放** ----
     * 清单（AI_RELS）里只有 7 个核心关系。用户说「我 想念 家」时，
     * 「想念」不在清单里 —— 以前整句就被忽略。现在它会被登记成一个动态关系，
     * id 从 8 起，写进模型（v7）后存盘重载仍然对得上号。
     * 只支持肯定，不支持否定（老实记在文档的局限里）。 */
    char  *dynrel_name[AI_MAX_DYNREL];
    size_t dynrel_n;
    /* ---- 推导：上一轮那条链 ----
     * 输出只是它的投影（见 derive.h）。::der 可以按结构看它，
     * ::style 可以换一种说法说同一件事 —— 语言不是重点，链才是。 */
    Derivation last_der;
    size_t n_der;                   /* 累计推导条数 */
    /* 反事实假设（**栈式**覆盖层：可多层嵌套、每层可归属某个人；
     * **绝不进图**，也不存盘） */
    Hypo   hypo;
    size_t n_hypo;                  /* 累计设过多少次假设 */
    /* 这一轮站在谁的立场上（NULL = 我自己）。由「他 觉得 …」这种输入设置，
     * 用完即清 —— 它是一个**问法**的修饰，不是持久状态。 */
    Token *view_who;
    /* 运行时的「关系是否独占」表：下标 = 关系 id。
     * AI_RELS 里写的是**声明值**（种子），装载后由 rel_excl_build() 按数据修正 ——
     * 这样「独占」就不是纯手写公理，而是从知识库里**量**出来的（数据够才改）。 */
    uint8_t rel_excl[REL_TABLE_MAX + 1];
    size_t  n_rel_excl_learned;   /* 被**数据推翻/改口**的条数 */
    size_t  n_rel_excl_evidenced; /* 判定**有数据支持**的条数（含印证与改口） */
    /* ---- 「独占」判定的**证据**：数据量出来的，摆在明面上 ----
     * 判一条关系是不是函数式（一个主语最多一个宾语），靠的是「主语里有几个是
     * 多宾语的」。样本少的时候不许瞎猜，所以这里把证据和似然比都留着，
     * `::excl` 直接打出来 —— 判错的时候能一眼看出是数据不够还是数据本身如此。 */
    size_t  rel_excl_single[REL_TABLE_MAX + 1];  /* 单宾语主语数 */
    size_t  rel_excl_multi [REL_TABLE_MAX + 1];  /* 多宾语主语数 */
    uint32_t rel_excl_base;                      /* 数据估出的基准多宾语率（千分数） */
    float   rel_excl_llr[REL_TABLE_MAX + 1];     /* 对数似然比：>0 更像多值 */
    /* 开放世界（默认）还是闭合世界。
     * 「知识库里没有」≠「事实为假」—— 默认按开放世界，推不出就说不知道。
     * 想要「没写就是假」的闭合世界语义，得**显式打开**（::world closed），
     * 因为那是一个假设，不是真理。 */
    int    closed_world;
    int    der_style;               /* DerStyle，默认 PLAIN */
    ChainOfThought cot;             /* 这一轮的思维链（每轮开头清空） */
    size_t n_walks;                 /* 逐词关联链（加权游走）跑过的次数 */
    char  *last_idea;               /* 上一次「自由联想」的产物（内部思考，不直接输出） */
    int    walk_output;             /* 1 = 允许把联想结果直接当回复（默认 0） */
    /* **缓存参与自由联想**（①f，默认 0 = 关）：走边那套按 role 取边，而"推出来沉淀的"
     * 缓存（`role=推论`）**不在 order 里** ⇒ 默认按构造就进不了联想。
     * 打开后把它作为**最后一档**放进顺序里（宁可走不到，也别让缓存喧宾夺主：
     * 缓存可能已经过时，让"想出来的"影响联想会自我强化）。 */
    int    cache_walk;
    /* **求值层样板**（② 档，默认 0 = 关）：过程条件的数值比较（`[条件:3<5]`）真的算一遍。
     * 关着时条件照旧走"前提三档"（图内判据），行为与今天逐字节相同。 */
    int    eval_num;
    /* **他者上位降权**（② 档，默认 0 = 关）：他说的比你问的**更宽**时，算**半票参考**
     * （只在没有"确定表态"时才决定倾向）。关着时它只是"相关"、不参与判定（今天的行为）。 */
    int    other_weak_up;
    /* **触发接求解**（② 档，默认 0 = 关）：开放式关系问句（`A R 什么` / `谁 R B`）
     * 走求解器（正向查邻居 / 反向扫一遍），而不是掉"我不太懂"。 */
    int    rel_open;
    int    locative;                 /* §6.218 地点块学习（A V 在 B ⇒ A 在 B），默认开 */
    /* **指代消解最小版**（② 档，默认 0 = 关）：记"最近提到的那个谁"，
     * 于是「他 觉得 猫 会 飞 吗」里的**他/她/它**能指回那个人（而不是掉流水线）。 */
    int    pron_last;
    /* §6.143 **元认知提醒的写入也受审**（默认 0 = 关）：开着时 `[提醒:…]` 那条边走
     * `ai_write_assoc`（统一通道）；关着时照旧直写。原因是外部审计点名的 P1-2：
     * `micro_remind` 一直在直接改「我」的边表（它挂在 `ai->self` 上，是**自撰**，不是观测）。 */
    int    remind_route;
    int    rel_nat;                 /* §6.161 关系层认「自然说法」：能/可以 → 会（默认**关**）*/
    /* §6.168 **追问回话**（默认 0 = 关）：上一轮我对着一个闭合问句说"推不出来"，
     * 把 (A,R,B,neg) 记在下面；这一轮用户只回「会/对/不会/不对」⇒ 当成教学写进图。
     * 为什么值得做：用户原话「主要的知识获取途径还要是互动」——
     * 现在的互动是"我说一句它学一句"（陈述），**问句答不上来这条就没下文了**。
     * 只在**它自己说推不出来**时才记（不靠答复文本里的"不确定"去猜，那是检索来的句子）。 */
    int    nat_coin;                 /* §6.173 自然说法里的**新词**：`麒麟 属于 神兽` 当场造词
                                      * 并学进图（默认**关**；只认核心关系，不造谓语）*/
    int    askback;
    /* §6.594 学到跟已有独占值冲突的事实之后，**问不问**「要 我 改 掉 旧 的 吗」（默认问）。
     * 长会话里每遇冲突都问会烦；关掉之后照样**说清**冲突，只是不挂"待确认"。
     * `AIP_ASKCONFLICT=0` 也能关（`::askconflict off` 同一个开关）。 */
    int    ask_conflict;
    /* §6.607 `A 属于 B，你 记住 了 吗` 这类"陈述 + 问句"混一句时：
     * 陈述那半句学到的东西先攒在这里（`记下 了 ： …`），等这一轮的答复定下来再**拼到前面** */
    char   learn_note[420];
    /* §6.634 **奖励机制（第一版）**：只计量 + 一个可开的"更谨慎"口子。
     * 为什么先只计量：奖励一旦直接改学习强度，就等于给整个学习链加了一个隐形旋钮，
     * 必须先用 A/B 证明"不奖励也对"，再谈"奖励让它更好"。默认 `reward_gate=0`（只记录）。 */
    float  reward;        /* 短期：每轮衰减 0.9，夹在 [-5,+5] */
    float  reward_cum;    /* 会话内累计 */
    int    reward_n;      /* 事件数 */
    int    reward_gate;   /* 0 = 只计量（默认）；1 = 让我"更谨慎"（见 agent.h 的用法） */
    struct { float d; char what[24]; } rlog[8];   /* 最近 8 个事件（环形） */
    size_t rhead;
    /* §6.627 **"因为"只记不推**（会话内）：`因为 C ，A R B` ⇒ 记住 C 是 A R B 的原因。
     * 它不是图里的边（图只有 A R B 三元组），也不进存档 —— 关掉进程就没了（登记在案）。 */
    struct { char a[40], r[20], b[40], why[120]; } reasons[16];
    int    n_reason;
    int    learn_note_only;      /* §6.607 问句那半句本身是"你 记住 了 吗" ⇒ 只回"记下了"，别再拼一句 */
    int    ask_pending;
    int    ask_restored;             /* §6.188：载入后扫过一次 `[追问:…]` 标记了吗（只扫一次）*/
    char   ask_mark[96];             /* §6.188：这一条 pending 对应的标记节点名（回收时按它找）*/
    int    ask_kind;                 /* 1 = 我没把握（askback：回一句就教）
                                      * 2 = 我给了**确定答案**（§6.172：回「不 对」就改口） */
    char   ask_a[64], ask_b[64];
    /* §6.268 上一轮**问句**的（关系, 宾语）—— 供「那狗呢」这种省略问句补全 */
    uint8_t last_q_rid;
    uint8_t last_q_neg;
    char    last_q_b[40];
    uint8_t ask_rid;
    uint8_t ask_neg;                 /* 问句本身是不是否定问法 */
    uint8_t ask_fact_neg;            /* 我**断言**的那条事实的极性（kind=2 用） */
    /* **写入闸门：分层判据**（② 档，默认 0 = 关）：实测 imp 落在 0.587~0.725，
     * 一个阈值切不动（0.65 只切掉 24%、0.70 只切 4.2%）⇒ 打开时改成分档：
     * 高档直接沉淀 / **中档要有旁证**（证据条数或链底分）/ 低档拦下 + 一条硬门槛。 */
    int    wgate_tier;
    /* **生成的原语程序要不要真跑**（C③，默认 0 = 只生成 + 校验）：打开才走 `token_run_names_ctx`。 */
    int    gen_exec;
    /* **C③ 第二件：多轮自我修正**（默认 0 = 关）：第二次还编不过时，用**新的错误**再认一遍
     * 规则、再改一处、再编一次（总次数 ≤ 3）。关着 = 只改一次（与今天逐字节相同）。 */
    int    code_multi;
    /* **假设下的结论沉淀成条件缓存**（§6.118，默认 0 = 关）：假设不进图（铁律 ✓），
     * 但"假设下的结论"不该被当垃圾丢掉 —— 把它写成**有条件成立**的形状
     * （`A —[前提]→ P → B`，P = **整条假设栈的指纹**），前提活着缓存就用得上，
     * 栈一撤前提就不成立（缓存自动失效 ✓）。升格（撤回后能无条件推出 ⇒ 前提不再需要）另做。 */
    int    hypo_cache;
    /* **逐字接地队列**（§6.120，用户 2026-10-11 提的方向，默认关）：
     * 输入先**按顺序**把 UTF-8 字符接进队列（"先接地，待会要用"），
     * 开着 `char_stream` 时**不分词**，直接把这些字符当成"词"按顺序交给思维/对话。
     * ⚠ **分词器保留**（右边那条 `ai_tokenize` 的路一个字都不动）——这一步只是多一条可选旁路，
     * 将来要撤分词器也得先从这里长期跑通（用户明确要求：现在千万不要撤）。 */
    int    char_stream;
    char   cq[512];                  /* 顺序接地的字符（UTF-8 原文，按到达顺序） */
    size_t cq_n;
    size_t n_hypo_cache;             /* 沉淀过几条条件缓存（给人看） */
    /* **条件缓存的登记表**（§6.119 升格用）：造过哪些"假设下结论"，供撤回后复查。
     * 有界（16 条），满了就丢最老的（登记表只影响"能不能升格"，不影响缓存本身的正确性）。 */
    struct { Token *A, *B, *P; uint8_t rid, neg; } hc[16];
    size_t n_hc;
    /* **N1 参与决策**（思维侧第二条，默认 0 = 关）：思维对象定稿时把它抄到这里
     * （ai.h 在 think.h **之前**，调不到 think_of ⇒ 用一层 AI 级缓存转手），
     * 走边没走出好东西时（best_score 低于阈值）就用**它想通的结论**当结果。 */
    int    draft_keep;               /* 草稿留痕（§6.103，默认关）：发送后另存一条 `[草稿:…]` */
    int    n1_decide;
    char   n1_concl[96];
    int    n1_gaps;
    int    n1_verified;
    /* **类比兜底**（② 档，默认 0 = 关）：规则（关系边）答不出时，用"上位/同类"的例子答**大概**，
     * 并把置信度压低（0.45~0.50）；措辞一定带"大概"和依据，绝不硬说"对"。 */
    int    rel_analogy;
    /* **先链后联想**（§6.124，默认 0 = 关）：确定性推理答不上来时，才轮到"同类互推"的
     * **大概**答案。老顺序里联想兜底排在确定性推理**之前** ⇒ 它会**抢答**：
     * 实测 `甲9 会 冬眠9 吗`（继承本来能推出确定答案）老顺序答
     * 「大概 —— 同类里 乙9 也 会 冬眠9」，开着这个开关答「对，甲9-[属于·继承]->类9；类9-[会]->冬眠9」。
     * 关着时行为与一直以来的样子**逐字节相同**。 */
    int    rel_first;
    /* **记住推出来的结论**（§6.125，默认 1 = 开）：把**类比猜出来的结论**记成
     * "记忆里的猜测"（边上打 `[类比:依据词]`、`role=推论`），下一个问题可以拿它**当中转**。
     * 它**不算知识**（`rel_direct` 跳过），所以不会把"猜的"变成"知道的"。 */
    int    rel_remember;
    size_t n_rel_guess_mem;         /* 记住过多少条猜测结论（统计用） */
    /* §6.132 **一轮之内的整句打分备忘**：`ai_detect_topic` 与 `ai_sent_lookup`
     * 用**同一个查询**扫同一批库句，同一对 (查询, 库句) 每轮白算两遍。
     * 键 = 查询内容(token 指针序列) + tick + pairs_n + idf_n ——
     * ⚠ 必须按**内容**比：两个调用点各传一块临时数组（内容相同），按指针比永远不命中。 */
    int      *sc_memo;              /* 每条库句的分数 */
    int      *sc_cov;               /* 每条库句的覆盖率 */
    size_t    sc_cap;               /* 上面两个数组的长度 */
    uint64_t  sc_tick;              /* 这份备忘属于哪个 tick */
    size_t    sc_pairs_n, sc_idf_n; /* 算的时候库有几句 / idf 表多长（变了就作废） */
    const Token *sc_q[64];          /* 查询指纹：token 指针序列 */
    size_t    sc_qn;
    int       sc_valid;
    size_t    n_sc_call, n_sc_miss;   /* §6.132 记账：查了备忘几次 / 其中真重算几次 */
    /* §6.136 **库句侧的「字符集位图」缓存**（每条 256 bit = 32 字节；出厂模型 6701 条
     * ≈ 214 KB）。为什么有用：一趟全扫要摸 6701 条库句，每条都得先切字符、再打位图 ——
     * 库句**不会变**，这些劳动每轮白做。缓存之后绝大多数条只要一次 32 字节的 AND。
     * 判据**精确**（见 ai_sent_score_row 里的预筛）：字符集不相交 ⇒ 一个词都不共享
     * ⇒ cov=0、dice=0，且按现在的实现（逐字符比）字符项也是 0 ⇒ sc 必然是 0。
     * 库句只增不减（`ai_sent_add` 追加；装盘时 `pairs_n=0` 重建）⇒ 按 `cb_n` **懒补**。 */
    uint64_t *cb_bits;
    size_t    cb_cap, cb_n;
    size_t    n_cb_fill;          /* 记账：一共算过几条库句的位图（应该≈pairs_n，不是每轮）*/
    /* §6.140 连**字符切片**一起缓存：位图 + 切片一次算好，之后每轮连 `token_name_chars`
     * 都不用调（实测那一趟切分占"活下来那些条"的一半时间）。两条 blob 分开存
     * （指针 8 B/字符、长度 1 B/字符；出厂模型 51527 个字符 ≈ 464 KB），
     * 偏移按库句号索引（`cb_off[i]`..`cb_off[i+1]`）。同样只进内存、不进存档。 */
    uint32_t    *cb_off;          /* 长度 cb_cap + 1 */
    const char **cb_ptr;
    uint8_t     *cb_len;
    size_t       cb_chused, cb_chcap;
    /* §6.210 **库句侧 bigram 布隆位**（64 bit/条；出厂模型 6701 条 = 54 KB）。
     * 判据**精确**：两边没有共享的**字符 bigram** ⇒ `ai_cb_core` 的 inter 一定是 0
     * ⇒ 字符项必然是 0 —— 跳过它是"提前知道结果是 0"，不是近似（哈希碰撞只会放行）。
     * 只进内存、不进存档，跟着 `cb_bits` 那条懒补一起长。 */
    uint64_t    *cb_bg;
    /* **全图扫描的账**（§6.126）：维护类活动（GC / 缓存复核 / 巩固）每轮走了多少词、多少边。
     * 加它是因为"减少扫全局"这句要求**必须有数字**支撑 —— 改前改后各量一遍才敢说快了。 */
    size_t n_scan_tokens;
    size_t n_scan_edges;
    /* §6.135 分段计时的累加器（只在 `AIP_SEGTIME=1` 时被写；段名见 `AI_SEG_NAMES`）。 */
    uint64_t seg_ns[AI_SEG_N];
    uint32_t seg_hits[AI_SEG_N];
    /* §6.237 维护段再细分（巩固 / GC / 睡眠）——`::segreport` 一起打出来。 */
    uint64_t mnt_ns[3];
    uint32_t mnt_hits[3];
    /* §6.255 指代消解：句首「它/它们/这个/那个」⇒ 换成上一轮的主语 */
    char     last_subject[40];
    /* §6.582 上一轮**学到**的（主语/关系/宾语）—— `$A/$W/$O` 会话变量的来源。
     * 原来这三个在 agent.h 里是**全局**的：两个脑在同一线程里轮流用也会串味，
     * 多线程更是直接竞争（TSan 报 g_lf_a/g_lf_r/g_lf_b）。搬进 AI 之后线程与脑都各有一份。
     * 只进内存，不进存档（和 last_subject 一样是"会话内"的东西）。 */
    char     lf_a[40], lf_r[40], lf_b[40];
    /* §6.253 抽象分片：睡眠的"抽象那一步"摊到多轮（每轮扫一段 i） */
    size_t   abs_i;
    size_t   abs_made;
    /* §6.248 写后读回校验（默认关；`::verifywrites on` / AIP_VERIFYWRITES=1 打开） */
    int      verify_writes;
    size_t   n_verify_ok;
    size_t   n_verify_miss;
    int    (*rel_verify_hook)(const Token *a, const Token *b, uint8_t rid, int neg);
    /* §6.139 脏思考检查器（默认**开**，用户点名要"强制停止"）：输出里连续重复词/句就截停。
     * `AIP_DIRTY=0` 或 `::dirty off` 退回。 */
    int    dirty_guard;
    size_t n_dirty_stops;      /* 截停过几次 */
    size_t n_dirty_words;      /* 一共截掉了多少个词（诊断用） */
    /* **N4 空闲思考主动说话**（A1，默认 0 = 关）：没人说话的那一轮，如果 `think_idle`
     * 想出了东西（痕里新增了一步），就把它**说出来**（带"空闲时想到的"标记），而不是只回"……"。 */
    int    idle_speak;
    /* **流水线回复也走草稿区**（A5，默认 0 = 关）：普通回复先落草稿，由 `::notepad on/hold`
     * 决定发不发（现在的记事本只管"AI 自己写结论"那条路）。 */
    int    note_all;
    Token *last_who;      /* 最近一次"谁 以为/觉得/说 X"里的那个谁（只在开关打开时用） */
    /* **词表扩展**（② 档"分词最长匹配"那一件的正解，§6.84）：分词器本来就是"池内最长匹配"，
     * 切开的词**全是池里没有的**。所以缺口在**词表覆盖**，不在算法 ——
     * `lexicon.txt`（当前目录，一行一个词，`#` 注释）里的词按整词切。没有这个文件时行为不变。 */
    char **lex_extra;
    size_t lex_n;
    int    lex_loaded;      /* 词表扩展只读一次（懒加载：两条构造路都覆盖） */
    /* 0 = 结构化帧允许用关联链证据放行白名单外的槽位取值（默认）。
     * 故意用「反向命名」：这样 calloc 出来的 AI 骨架就是开启状态 ——
     * 这个项目已经被「calloc 骨架漏设默认值导致训练/加载两条路径分叉」坑过一次。 */
    int    assoc_slot_off;
    int    coin_off;                /* 0 = 允许现场造词（默认） */
    int    seg_off;                 /* 0 = 允许换切分（拆解/合并）找帧（默认） */
    int    relay_off;               /* 0 = 允许中转词容错（和/跟 互认）（默认） */
    int    fall_off;                /* 0 = 允许降级答复（默认） */
    int    rel_off;                 /* 0 = 允许关系层（推理 + 现场学知识）（默认） */
    void  *frames;                  /* FrameSet*（frame.h），结构化回复库 */

    size_t turns, n_learned;
    /* ---- 统一写入通道（来源监控的"写入侧"）----
     * 所有往图里写的地方都要**记一笔账**：谁写的、写了多少次。
     * `route_writes` 打开时，**知识类**写入（关系事实）改成提意图，走审查/预算/审计；
     * 感知共现这类保留直写（它是观测本身的丰富记录：hits/pattern/topic 都在里面，
     * 转成意图会丢信息）—— 但账照记，所以"谁在绕过内核"随时说得清。 */
    /* 推理沉淀的账（**放在 AI 上**：来源档的可信度要用它，而那个函数在 think.h 之前）。
     * 以前记在 Think 里，只有报告用；现在它是"学出来的可信度"的证据来源。 */
    size_t  n_cached, n_retracted;
    size_t  n_write_blocked;        /* 被写入闸门挡下来的条数（账要对得上） */
    size_t  writes[AI_WSRC_N];
    int     route_writes;
    /* **这三个是内核唯一知道的东西：三个普通数字**（默认 0）。
     * 内核不知道它们是怎么来的 —— 那张耦合表在**内核之外**（`drive.h` 层），
     * 由代理循环每轮折算一次（用户裁定：情绪不进内核，层的名字也就改成了"驱动"）。
     * 开着的时候每轮把驱动量折算成参数修正（思考预算 / 生成长度 / 激活阈值）。 */
    int     drive_on;
    /* 前提成立的**判据档**（0 = 只看注意窗口（默认）；1 = 窗口里没有时，还在图上"够得着"也算）。
     * 用户点过的局限：前提成立只看"提到过没有"，不看"是不是真的成立/想得到"。
     * 第二档是**图内**判据：从当前注意窗口出发扩散两步，前提被激活到阈值就算成立。 */
    int     prem_hold_activate;   /* 0=只看注意窗口 1=图上也够得着 2=**有据**才算（第三档） */
    /* 写入分级闸门（借参考实现的 imp 评分）：默认关，`::wgate on` 打开。 */
    int     write_gate_off;
    /* 闸门里的**候选证据项**（support）：**默认开**（闸门本身默认关，所以默认路径行为不变）。
     * A/B 三种跑法都绿之后才翻的默认（§6.43）；`AIP_WGATE_EV=0` / `::wgate ev off` 可关回去。
     * 只对"AI 自己想沉淀的候选"生效（那时才有 ctx）；观测/量测台扫图那条路不受影响。 */
    int     wgate_ev;
    double  last_write_imp;       /* 最近一次写入的重要度（-1 = 还没写过；::wgate 看得到） */
    /* 最近一次候选的**证据**（量测与 ::wgate 用；不存盘）：链长 / 链底可信度 / 证据档 */
    int     last_write_hops, last_write_cred, last_write_ev;
    /* 对话触发用哪个匹配器：1 = 说法归一表 + 槽位（新，**默认**）；0 = 关键词表 + 位置算术（老）。
     * 覆盖量到 33/33（老的 25/33）+ 整包两跑法全绿之后才翻的默认（§6.44）；
     * `AIP_TRIGGER=kw` / `::triggers norm off` 可切回老的。 */
    int     trigger_norm;
    /* 唯一时间核的 γ（0 = 用默认）。衰减/新近**只许**通过 ai_recency_weight 走。 */
    float   time_gamma;
    int     drive_d_steps, drive_d_gen;    /* 这一轮实际生效的修正（::drive 看得到） */
    float   drive_d_cutoff;
    /* **受审的外部执行 + 写文件**（`action.h` 层）的开关：默认 0 = 关。
     * 层本身（白名单/超时/账/观测标记）全在 action.h，内核只看到这一个数字 ——
     * 和情绪那张表同一个放法（用户 2026-10-08 拍板：白名单 echo/cat/wc/gcc+grep/head/tail/sed/ls、
     * 写文件只在 _t/work/、不联网）。**不进存档**（格式 v10 不动）。 */
    int     act_on;
    /* **原语程序的干跑校验**（`token_lang.h` 的 `token_check_program`）：默认 0 = 关。
     * 关着时 `::asm` 与今天**逐字节相同**（打错的原语名照旧被静默 intern 成数据词）；
     * 开着（`::asm check on` / `AIP_ASMCHECK=1`）时 `::asm` 先干跑，没过就**不执行**。
     * 校验器只按原语表里**代码真实行为**的栈效应判（见 token_lang.h 那张表的注释）。 */
    int     asm_check_on;
    /* **ASCII 代码词法**（`lex.h`）接不接进代码通道：默认 0 = 关。
     * 开着（`::ascii on` / `AIP_ASCII=1`）时 `::code` 的痕里多一行「逐字保留 ✓/✗」；
     * 关着时 `::code` 的输出与今天逐字节相同（`::lex` 是"看一眼"的命令，任何时候都能用）。 */
    int     lex_ascii;
    /* **他者视角进回答路径**（③，默认 0 = 关）：复数主语（"大家/他们/别人"）问出来的话，
     * 用 §6.45 的四态判定回答（都说认 / 都说不认 / 有分歧（待验）/ 没人说过）。
     * 只读（他者信念只在**他的**名下，判定现算不落库）；关着时整条路不跑（与今天逐字节相同）。 */
    int     others_ask;
    /* **过程执行**（第 9 条 ③ 写引擎，默认 0 = 关）：`::proc run <名字>` 把一条过程跑一遍 ——
     * 逐步核对（认识吗 / 前提成立吗 / 是不是符号节点行为），核对本身只读；真的执行某一步行为
     * 时走的是老那条受审通道。关着时那个命令只说"关着"，一个字的既有行为都不变。 */
    int     proc_run;
    /* **干跑**（第 9 条 ④，默认 0）：过程执行时"前提不成立"改成**假定成立**（记进假设层）、
     * 行为步骤**不执行**（只记"假定能做"）；跑完把假设层 pop 回原样 —— 一个字都不落图。 */
    int     proc_dry;
    /* **过程记号接进对话**（第 9 条 ②入口，默认 0 = 关）：说「记个过程 泡茶」「第 1 步 烧水」
     * 「跑 泡茶」就能建/跑过程（走的还是 `::op` 那条受审通道）。关着时一句都不认。 */
    int     proc_talk;
    /* **过程接进思维**（第 9 条 ② 的思维侧，默认 0 = 关）：一轮思考收尾时，如果这个问题
     * 命中一个我记过的过程（`[过程:X]`，X 出现在这一轮的话里），就在**痕**里说一句
     * "这件事我记过过程 X（N 步）"，引擎开着时再**干跑一遍**（只读）把结果并进痕。
     * 关着时一个字都不加。 */
    int     proc_think;
    /* **过程接进决策**（第 9 条 ② 的下一步，默认 0 = 关）：问"怎么做 X"而我又记过过程 X 时，
     * **直接用计划回答**（第 1 步…→ 第 2 步…），而不是走流水线那句。引擎开着时附上干跑结论。 */
    int     proc_plan;
    float energy, decay, cutoff;
    size_t max_gen;
    size_t consolidate_every;
    float decay_factor;

    AIGCConfig gc;
    UncertainConfig uncertain;
    AISetpoint setpoint;
    AIInner inner;
    MetaCog meta;

    Token *focus_ring[AI_MAX_FOCUS];
    float  focus_weight[AI_MAX_FOCUS];
    size_t focus_n;
    Token *focus;

    Token **trace;
    size_t trace_n, trace_cap;

    Token **last_output;
    size_t last_output_n, last_output_cap;
    /* §6.216 **上一句答复的原文**（用来"再说一遍"）：按词 token 拼回去会丢掉
     * `-[属于]->` 这种排版（实测），所以直接存最终字符串。只进内存。 */
    char   last_reply[256];
    /* §6.217 **最近几轮"用户说了什么"**（环形，只进内存）：用来回答
     * 「我们 刚才 聊 了 什么」/「我 刚才 说 了 什么」。
     * 存**原文**（不做分词/拼接），这样回答里能原样复述用户的说法。 */
    char   hist[6][128];
    size_t hist_n, hist_head;
    Token **last_skipped;
    size_t last_skipped_n, last_skipped_cap;

    AIPath last_path;
    /* 上一轮 plan_and_act 里「分数最高」那条候选的路径。
     * last_path 记的是循环里最后生成的那条候选（未必是选中的），
     * 直接拿它做诊断会让「路径」和「产物」对不上，所以单独留一份。 */
    AIPath last_walk_path;

    Token *expr_root[EXPR_COUNT];
    int    last_expr_mode;

    Token    *timeline[AI_MAX_HISTORY];
    size_t    timeline_n, timeline_head;

    OtherModel others[AI_MAX_OTHERS];
    size_t     others_n;

    /* §6.564 **本智能体的修改账**（只进内存、不进存档）：谁改了哪条边、什么时候、什么来源。
     * 它是"每个智能体只维护自己的修改"的第一刀 —— 两个 AI 各有各的账。 */
    AIModLog   modlog;

    float  imagine_rate;
    size_t n_imaginations, n_imagine_useful;
    float  inner_dialogue_gain;
    size_t n_inner_dialogues;
    size_t narrate_every, n_narrations;
    /* §6.141 **让叙事/漂移显形**：这两件事到轮次就会跑，但以前只在 `::stats` 里留**计数**，
     * 内容一个字都看不到 ⇒ 等于"悄悄发生"。这里把**最近一次**的文本/条数留下来（只进内存），
     * `::narrate` / `::drift` 能当场打出来，也能 `now` 触发一次。 */
    char   narr_last[256];
    size_t narr_last_turn;
    size_t n_drift_last;
    size_t drift_sample_every, n_drifts_detected;

    DiagState diag;
    size_t    n_diagnoses;
    CalibState calib;
    float      last_confidence_at_output;

    size_t n_surprises, n_abstractions, n_sleeps;
    /* §6.135 睡眠激活的账（诊断用）：一共访问了多少节点 / 有多少次被上限截断。
     * 加它的理由：睡眠那条路以前**永远跑不完**，没有任何数字能看出来它跑到哪儿了。 */
    size_t n_sleep_visits, n_sleep_cuts;
    /* 睡眠回放的访问上限（默认 `AI_SLEEP_MAX_VISITS`）。做成字段是为了**可看护**：
     * 看护可以把上限压到 5，用一个小图证明"到顶就停"（不用真造一个指数爆炸的图）。 */
    size_t sleep_max_visits;
    size_t n_gc_deleted_edges, n_gc_deleted_tokens;
    size_t n_gc_skipped_partial;    /* §6.570 因为"图有一部分在磁盘上"而跳过的 GC 次数 */
    size_t n_credit_events, n_expr_learned, n_counterfactual;
    /* 因果猜想：见过多少条（第一次见到就算）与其中多少升格成「可信的猜想」。
     * 注意这两个数**不进存档**（瞬态），和 n_counterfactual 一样只是给 ::stats 看的。 */
    size_t n_causal_seen, n_causal_promoted;
    float  last_confidence;

    int    n_candidates;
    size_t sleep_every;
    size_t sleep_cursor, sleep_target;
    int    sleep_in_abstraction;

    /* ---- 微内核状态（micro.h）----
     * 自修改意图队列 + 安全内核 + 补丁注册表 + 元认知。
     * 用 void* 而不是结构体成员，是为了让 micro.h **整个**放在本文件末尾包含
     * （结构体定义在那边，而 ai_new 排在它前面）。懒分配：第一轮走到安全点时
     * micro_of() 才建，所以它不进存档、也不影响 ai_new 的默认值。
     * 存档格式不含它：补丁是运行时驱动、意图是暂态；
     * 自修改**产生的符号节点和边本身**已经随图存盘了。 */
    void  *micro;

    /* ---- 思维循环（think.h）----
     * 议程 / 未解问题 / 痕迹 / 元认知。同样用 void* 懒分配：
     * 结构体定义在 think.h 里，而那个头文件要用 rel.h/derive.h，只能放在本文件末尾。
     * 它也不进存档：未解问题是**会话内**的东西（想通了就没了），
     * 想通之后落地的只有那些经安全内核审查过的符号节点/边。 */
    void  *think;
} AI;

/* ---- §6.135 分段计时（默认关；`AIP_SEGTIME=1` 才真的读表）----
 * 用法就是一对：`uint64_t t = ai_seg_mark(ai);` 开头，每个段尾 `ai_seg_lap(ai, 段, &t);`。
 * 环境变量只读一次（和别处的开关同一个口径：第一次调用缓存下来）。 */
static inline int ai_seg_on(void) {
    /* §6.582 只读环境变量的缓存 ⇒ 线程局部（每线程自己读一次 getenv，值一样）*/
    static AIP_TLS int cached = -1;
    if (cached < 0) { const char *e = getenv("AIP_SEGTIME"); cached = (e && *e == '1') ? 1 : 0; }
    return cached;
}
static inline uint64_t ai_seg_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t ai_seg_mark(const AI *ai) { (void)ai; return ai_seg_on() ? ai_seg_now() : 0; }
static inline void ai_seg_lap(AI *ai, int seg, uint64_t *t) {
    if (!ai_seg_on() || seg < 0 || seg >= AI_SEG_N) return;
    uint64_t n = ai_seg_now();
    ai->seg_ns[seg] += n - *t;
    ai->seg_hits[seg]++;
    *t = n;
}

/* ---- 思维链的写入接口（要等 AI 结构定义完才能放在这里）---- */
/* 定长拷贝（截断是设计的一部分，所以自己写，别惹那两个编译警告） */
static inline void cot_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

/* 往链上追加一步。满了就丢掉（不覆盖前面的步骤 —— 开头那几步最要紧）。 */
static inline void cot_push(AI *ai, CotKind kind, int ok, int score,
                            const char *what, const char *why) {
    if (!ai || ai->cot.n >= COT_MAX_STEP) return;
    CotStep *s = &ai->cot.step[ai->cot.n++];
    s->kind = kind;
    s->ok = ok;
    s->score = score;
    /* 手写定长拷贝，不用 snprintf("%s") / strncpy：
     * 前者会被 -Wformat-truncation 拦（推导链可能很长），
     * 后者会被 -Wstringop-truncation 拦 —— 而这里**截断本来就是设计**。 */
    cot_copy(s->what, sizeof s->what, what);
    cot_copy(s->why, sizeof s->why, why);
}

static inline void cot_reset(AI *ai) { if (ai) ai->cot.n = 0; }

/* 前向声明 */
static inline Token *ai_predict_readonly(const AI *ai, Token *cur);
static inline uint32_t ai_edge_weight(const AI *ai, const AssocEntry *e);
static inline float sig_gap(const Token *t);
static inline float sig_novelty(AI *ai, char **words, size_t n);
static inline void ai_form_abstractions(AI *ai);

/* ============================ 生命周期 ============================ */

static inline void ai_init_expr_pools(AI *ai) {
    static const char *names[EXPR_COUNT] = {
        "[表达:确定]", "[表达:大概]", "[表达:可能]",
        "[表达:不确定]", "[表达:不知道]", "[表达:好奇]", "[表达:沉默]",
    };
    static const char *variants[EXPR_COUNT][5] = {
        { NULL, NULL, NULL, NULL, NULL },
        { "应该", "大概", "估计", NULL, NULL },
        { "可能", "似乎", "感觉", NULL, NULL },
        { "我不太确定", "说不好", "让我想", NULL, NULL },
        { "不知道", "不清楚", "想不起来", NULL, NULL },
        { "这是什么", "什么意思", "？", NULL, NULL },
        { NULL, NULL, NULL, NULL, NULL },
    };
    for (int i = 0; i < EXPR_COUNT; ++i) {
        ai->expr_root[i] = pool_token_intern(ai->pool, names[i]);
        for (int j = 0; j < 5 && variants[i][j]; ++j) {
            Token *v = pool_token_intern(ai->pool, variants[i][j]);
            token_add_assoc_unique(ai->expr_root[i], v);
        }
    }
}



/* 每轮按表把驱动折算成参数修正（**只在 drive_on 时**动）。
 * 这里只**算出修正量**并记下来；真正施加在用到参数的地方（思考预算在 think_of，
 * 生成长度/阈值在 agent_step）—— 一处算、一处用，别两头各改一遍。 */
/* ============================ 唯一时间核（时间衰减只许走这里） ============================
 *
 * 借参考实现（lingshu）的纪律：**全库只留一个衰减核**，任何别处自己写
 * `exp(-t/τ)` / `×(1-factor)` / EMA 保持率，都算口径分叉（那种代码里同一条知识
 * 会被两套衰减算出两个新旧程度）。这里钉死指数族：
 *
 *     w(age) = exp(-γ · age)        γ = 1 / 半衰期刻度
 *
 * 目前两个用户：逐条可信度的"新近项"、`::order`/租约式的年龄比较。
 * （符号节点的 hits 衰减是**按巩固周期离散**的几何衰减，属于另一套口径 —— 它作用在
 *   hits 计数上，不参与"这条知识多有底气"的判定；两套口径的分工写在这里，别再混。） */
#define AI_TIME_GAMMA_DEFAULT 0.00002f      /* 半衰期约 34657 tick（≈ 训练语料的量级） */

static inline double ai_time_kernel(uint64_t age_ticks, double gamma) {
    if (gamma <= 0.0) return 1.0;
    if (age_ticks == 0) return 1.0;
    if (age_ticks > 4000000u) age_ticks = 4000000u;    /* 数值保护：别把 exp 打爆 */
    return exp(-gamma * (double)age_ticks);
}
/* 这条知识按"新近程度"折算的权重（0..1），**唯一**的新近口径 */
static inline double ai_recency_weight(const AI *ai, uint64_t age_ticks) {
    double g = (ai && ai->time_gamma > 0.0f) ? (double)ai->time_gamma
                                             : (double)AI_TIME_GAMMA_DEFAULT;
    (void)ai;
    return ai_time_kernel(age_ticks, g);
}

/* ============================ 来源档的"学出来的"可信度 ============================
 *
 * 用户点过的：「可信度是启发式，不是学出来的」。这里把它升级成 **Beta 后验**，
 * 而且**不新增存储、不动存档格式** —— 证据直接来自**已有的计数器**：
 *
 *   基准（relations.txt 显式教的）：先验 Beta(6,2) → 0.75，没有反例可记（它就是底）；
 *   观测（听来的/经历的）        ：先验 Beta(4,4) + 命中 (n_learned − n_revised)、反例 n_revised；
 *   推论（想出来沉淀的缓存）      ：先验 Beta(3,7) + 命中 (n_cached − n_retracted)、反例 n_retracted；
 *   他说的（他的信念表）          ：先验 Beta(2,2) → 0.5（没有 ground truth，按中立）。
 *
 * 含义：**改口越多，观测档的可信度越低**；**缓存被撤销越多，推论档越低**。
 * 这就是"从经验里学出来的可信度"最小可用版（要更强的版本就得把 a/b 真正持久化）。 */
static inline double ai_source_posterior(const AI *ai, uint8_t role) {
    double a = 2.0, b = 2.0;
    if (role == ASSOC_ROLE_INNATE) { a = 6.0; b = 2.0; }
    else if (role == ASSOC_ROLE_DERIVED) {
        double nc = ai ? (double)ai->n_cached : 0.0;
        double nr = ai ? (double)ai->n_retracted : 0.0;
        a = 3.0 + nc; b = 7.0 + nr;
    } else {
        /* ⚠ 用**对话里学到的关系事实**（n_rel_learned）而不是 n_learned：
         *   后者把 24 万条训练词对也算进来 → 后验恒等于 ~1.0（观测档直接顶到上限）。
         *   这里要问的是"我现场学的东西，有多少被后来的说法改掉"。
         *   没在现场学过任何东西时退化成先验 Beta(4,4)=0.5（保守）。 */
        double nl = ai ? (double)ai->n_rel_learned : 0.0;
        double nv = ai ? (double)ai->n_revised : 0.0;
        if (nv > nl) nv = nl;
        a = 4.0 + (nl - nv); b = 4.0 + nv;
    }
    if (a < 0.5) a = 0.5;
    if (b < 0.5) b = 0.5;
    return a / (a + b);
}

/* 来源档后验的**百分数**（0..85，和原来的固定档同一个量纲，便于沿用旧公式） */
static inline int ai_source_class_pct(const AI *ai, uint8_t role) {
    double p = ai_source_posterior(ai, role);
    int v = (int)(p * 100.0 + 0.5);
    if (v < 5) v = 5;
    if (v > 85) v = 85;
    return v;
}

/* ---- 前提的第三档：前提本身**有据**吗（借 "evaluated vs asserted" 的二分）----
 * 「够得着」只说明它现在能被想到；「有据」说明它本身是图上站得住的东西：
 * 不是临时草稿、有足够多带命中的观测边。 */
/* 阈值**量过了**（`tools/premholdprobe.c`）：出厂模型 1070 个内容词里，
 *   有据度（命中≥2 的非推论实边条数）分位 p10=0 / **p50=3** / p90=44，
 *   17.9% 的词一条都没有。所以 `≥3 条` 正好是**中位数** → 它把词一分为二（53.1% 算"有据"）；
 *   `命中≥2` 与 ≥1/≥3/≥4 几乎没差别（53.1/53.1/53.0/52.8%）→ 这个数不敏感，留着没坏处。
 * 照实说：出厂模型里**一条前提链接都没有**（0 条），所以三档差异只在"现场建了前提"的会话里
 *   体现 —— 整包在 `AIP_PREMHOLD=verified` 下全绿是**预期**，不是"它没作用"的证据。 */
#define AI_PREMISE_MIN_EDGES 3
#define AI_PREMISE_MIN_HITS  2
static inline int ai_premise_wellfounded(const AI *ai, const Token *p) {
    if (!ai || !p) return 0;
    /* 草稿纸当不了"有据的前提"（判据与 micro.h 的 token_is_temp 同源：`[临:` 命名空间） */
    if (p->name && strncmp(p->name, "[临:", 4) == 0) return 0;
    if (p->name && p->name[0] == '[') return 0;         /* 内核自有标记也不算 */
    size_t solid = 0;
    for (size_t i = 0; i < p->count; ++i) {
        const AssocEntry *e = &token_assocs_ro(p)[i];
        if (!e->main || e->hits < AI_PREMISE_MIN_HITS) continue;
        if (e->role == ASSOC_ROLE_DERIVED) continue;          /* 缓存不算"据" */
        if (e->main->name && e->main->name[0] == '[') continue; /* 内核标记不算 */
        solid++;
    }
    return solid >= AI_PREMISE_MIN_EDGES;
}

/* 一条边的**逐条可信度**（线上口径）：来源档的**后验** + 命中加成，再乘唯一时间核。 */
static inline int ai_credibility(const AI *ai, const AssocEntry *e) {
    if (!e) return 0;
    uint64_t now = (ai && ai->pool) ? ai->pool->current_tick : 0;
    uint64_t age = (now > e->last_touch_tick) ? (now - e->last_touch_tick) : 0;
    int cls = ai_source_class_pct(ai, e->role);
    return assoc_credibility_cls(e, ai_recency_weight(ai, age), cls);
}

/* ---- 知识租约（TTL）：`stable` 不是一个终态，而是**带期限的状态** ----
 * 借参考实现的做法：租约到期 ≡ 可信度衰减到弱阈值以下；**降级只作用于状态，不作用于记录**
 * （我们从不因为"过期"删东西，只改判定与呈现）。TTL 与 γ 都走**唯一时间核**。 */
#define AI_LEASE_TTL_TICKS   200000u   /* 租约期限（tick；≈ 半衰期的 3 倍左右） */
#define AI_LEASE_WEAK_CRED   30        /* 可信度低于它 = 减弱（对应他们的 0.3 阈值） */

typedef enum { AI_LEASE_STABLE = 0, AI_LEASE_WEAK = 1, AI_LEASE_EXPIRED = 2 } AiLease;

static inline AiLease ai_edge_lease(const AI *ai, const AssocEntry *e) {
    if (!e) return AI_LEASE_EXPIRED;
    uint64_t now = (ai && ai->pool) ? ai->pool->current_tick : 0;
    uint64_t age = (now > e->last_touch_tick) ? (now - e->last_touch_tick) : 0;
    if (age >= AI_LEASE_TTL_TICKS) return AI_LEASE_EXPIRED;
    if (ai_credibility(ai, e) < AI_LEASE_WEAK_CRED) return AI_LEASE_WEAK;
    return AI_LEASE_STABLE;
}

static inline const char *ai_lease_name(AiLease l) {
    switch (l) {
    case AI_LEASE_STABLE:  return "稳定";
    case AI_LEASE_WEAK:    return "减弱";
    default:               return "过期";
    }
}

/* ============================ 写入分级闸门（借 lingshu 的 imp 评分） ============================
 *
 * 他们的式子：`imp = 0.30·novelty + 0.25·trust + 0.15·Δ²D + 0.15·Δ²T + 0.15·mention`，
 * ≥0.70 长期层、≥0.40 知识层、**<0.40 情境层且直接丢弃**。
 * 我们**没有** Δ²D/Δ²T 那两条历史序列（缺口与信任的二阶差分），所以如实替换成两个
 * 等价的"当下状态"量：**惊讶度**（discomfort）与**不累**（1−fatigue）——
 * 名字不同、量纲不同，别当成同一个东西（这一条写在这里，免得以后误引）。
 * 另外加了一项他们式子里的"novelty"没有的、但在**图里**判别力强得多的量：
 * **信息量 = 1 − 两端邻居的 Jaccard**（重叠高＝这条连带是意料之中）。
 * 默认**关**：它会让某些"随口一说"不再进图，属行为改变，先实现先 A/B。
 *
 * ⚠ 2026-10-07 量出来的两件事（§6.43，`tools/wgateprobe.c` + 40 条真候选）：
 *   ① trust / 惊讶 / 不累 是**整个 AI 一个值**（标准差 0）→ 它们那 0.35 的权重
 *      只贡献一个**对所有候选都相同**的偏移，对"选哪条"毫无贡献；
 *   ② 更根本：闸门要判的是**AI 自己想沉淀的候选**，而候选自己的证据
 *      （链有多长、链底那条边多可信）在 `think_finish` 那一步**被丢掉了** ——
 *      评分里根本没有"这条推论有多少底气"这一项。
 *   所以加了 `AiWriteCtx`（候选自己的证据）与**候选证据项 support**，
 *   并给新公式单独一档阈值（在**候选总体**上按分位定，见 AI_WGATE_EV_*）。
 *   老路子（ctx == NULL 或开关关着）**一个字节都不变** —— 先 A/B 再翻默认。 */

typedef struct {
    int ev;     /* 证据档（THINK_EV_* / DER_EV_*：4 观测事实 3 规则推导 2 默认 1 假设 0 猜） */
    int hops;   /* 推导链长（步数）；0 = 不是推来的 / 不知道 */
    int cred;   /* 链底那条边的**逐条可信度**（der_base_credibility，0..85） */
} AiWriteCtx;

/* 阈值**量出来的**：
 *   老公式（扫全图 167 条关系边）：最小 0.587 / p25 0.652 / 中位 0.672 / p75 0.675 / 最大 0.725。
 *   借用来的 0.40 在这套评分上**永远打不着火**（一条都拦不住）；
 *   按实测取 p25 当"知识层"门槛、取前 ~4% 当"长期层"门槛。
 *   ⚠ 那 167 条是**基准边**（闸门不管它们，见 §6.43）；新公式的阈值改在**候选总体**上量：
 *     40 条真候选的新评分分位见 AI_WGATE_EV_*。 */
#define AI_WGATE_LONGTERM  0.70
#define AI_WGATE_KNOWLEDGE 0.65

/* 新公式（带候选证据项）的阈值 —— **在候选总体上量的**（40 条真推导候选，
 * `_t/meas/cand2.sh` 逐个开干净会话问出来的；§6.43）：
 *   分布是**双峰**的：弱底那批（链底可信度 55/56）落在 0.62~0.63（4 条），
 *   强底那批（85）落在 0.71~0.78（36 条）。
 *   所以阈值取**空档中间**，不是取分位（分位在双峰上不稳：p10=0.71 就切到强候选了）：
 *     知识层 0.67 = 空档 [0.63, 0.71] 的正中（上距 +0.04、下距 −0.04，两边都留余量）
 *     长期层 0.75 ≈ 强底那批的 p75（显示用的分层边界，不是第二道闸门）
 *   两套阈值各自服务一套公式；新项关着时**仍然**用上面那两个（老行为一位不变）。 */
#define AI_WGATE_EV_KNOWLEDGE 0.67
#define AI_WGATE_EV_LONGTERM  0.75

/* 两个词的**邻居重叠**（有界采样版 Jaccard，0..1）：重叠高 = 这条连带是"意料之中"。
 * 放在这里而不是调 micro.h 的：ai.h 在 micro.h 之前，且这里只要一个近似值。 */
static inline double ai_pair_jaccard(const Token *a, const Token *b) {
    if (!a || !b || a == b) return 0.0;
    size_t sa = a->count > 64 ? 64 : a->count;
    size_t sb = b->count > 64 ? 64 : b->count;
    if (sa == 0 || sb == 0) return 0.0;
    size_t common = 0;
    for (size_t i = 0; i < sa; ++i) {
        const Token *m = token_assocs_ro(a)[i].main;
        if (!m) continue;
        for (size_t j = 0; j < sb; ++j)
            if (token_assocs_ro(b)[j].main == m) { common++; break; }
    }
    size_t uni = sa + sb - common;
    return uni ? (double)common / (double)uni : 0.0;
}

static inline double ai_write_importance(const AI *ai, const Token *a, const Token *b) {
    if (!ai) return 0.0;
    /* novelty：生词更值得记（两端都还很"薄"→ 1.0，见得多 → 0.4） */
    size_t ea = a ? a->count : 0, eb = b ? b->count : 0;
    double novelty = (ea < 6 && eb < 6) ? 1.0 : ((ea + eb < 40) ? 0.6 : 0.4);
    /* **信息量**：两个词的邻居越不重叠，这条连带越是"新闻"（判别力比 novelty 强得多） */
    double info = 1.0 - ai_pair_jaccard(a, b);
    /* trust：用**自模型**的"我最近猜得准不准"（参考实现用的是 self_model.trust_state）。
     * ⚠ 别拿 `ai_source_posterior(观测档)` 顶替：那个是被 24 万次训练学习撑到 ~1.0 的计数，
     *   放进闸门会让它**永远开不了火**（实测：imp 恒定 ≥0.55）。 */
    double trust = (double)ai->inner.predict_accuracy;
    if (trust < 0.0) trust = 0.0;
    if (trust > 1.0) trust = 1.0;
    /* mention：两端在图里被碰过多少次（近似他们的 mention 项） */
    double mention = (double)((a ? token_hits_total(a) : 0) + (b ? token_hits_total(b) : 0));
    mention = mention / 200.0; if (mention > 1.0) mention = 1.0;
    /* 惊讶 / 不累：替代 Δ²D、Δ²T 的"当下状态"量 */
    double surprise = ai->inner.discomfort; if (surprise > 1.0) surprise = 1.0;
    double awake = 1.0 - (double)ai->inner.fatigue; if (awake < 0.0) awake = 0.0;
    double imp = 0.25 * novelty + 0.20 * trust + 0.20 * mention
               + 0.20 * info + 0.075 * surprise + 0.075 * awake;
    if (imp < 0.0) imp = 0.0;
    if (imp > 1.0) imp = 1.0;
    return imp;
}

/* **候选证据项**（只在"AI 自己想沉淀"这条路上有）：这条推论有多少底气。
 * 两个来源，都是**已有的**量，不新造判据：
 *   · `cred` = 链底那条边的逐条可信度（`der_base_credibility`：基准/观测/推论 + hits + 唯一时间核）；
 *   · `hops` = 链长：每多一跳就多一个可能出错的地方 → 按跳数**线性打折**（每跳 −0.08）。
 * 归一：cred 以 85（来源档后验的上限）为满。取 0..1，和别的项同量纲。 */
#define AI_WGATE_HOP_PENALTY 0.08
static inline double ai_write_support(const AiWriteCtx *ctx) {
    if (!ctx) return 0.0;
    double cred = (double)ctx->cred / 85.0;
    if (cred < 0.0) cred = 0.0;
    if (cred > 1.0) cred = 1.0;
    int hops = ctx->hops > 1 ? ctx->hops : 1;
    double s = cred - AI_WGATE_HOP_PENALTY * (double)(hops - 1);
    if (s < 0.0) s = 0.0;
    return s;
}

/* **闸门分层判据**（§6.89）：返回 1 = 放行沉淀，0 = 拦下（`why` 里写清卡在哪一档）。
 * 关着时**不调用**（调用方走原来的单阈值）—— 所以这是可 A/B 的开关，不是改行为。
 * 三档 + 一条硬门槛（都按实测分布 0.587~0.725 划的）：
 *   ≥0.70 高：直接沉淀（这条区间本来就少，放行）；
 *   0.60~0.70 中：**要有旁证** —— 候选证据 ≥1 条 **或** 链底分 ≥60，否则拦；
 *   <0.60 低：拦；
 *   硬门槛（先于分档）：既没旁证也没链底支撑、且分不到高档 ⇒ 拦（分数高也说明不了"有依据"）。 */
static inline int ai_wgate_tiered(const AI *ai, double imp, int ev, int cred,
                                  char *why, size_t cap) {
    (void)ai;
    if (why && cap) why[0] = 0;
    if (imp >= 0.70) {
        if (why && cap) snprintf(why, cap, "高档（%.2f ≥ 0.70）⇒ 沉淀", imp);
        return 1;
    }
    if (ev <= 0 && cred <= 0) {
        if (why && cap) snprintf(why, cap, "硬门槛：没旁证（证据 0 条、链底 0 分）⇒ 拦（%.2f）", imp);
        return 0;
    }
    if (imp >= 0.60) {
        if (why && cap) snprintf(why, cap, "中档（%.2f）但有旁证（证据 %d 条 / 链底 %d 分）⇒ 沉淀",
                                 imp, ev, cred);
        return 1;
    }
    if (why && cap) snprintf(why, cap, "低档（%.2f < 0.60）⇒ 拦（有旁证也不够）", imp);
    return 0;
}

/* 带候选证据的评分。`ctx == NULL`（观测/基准/量测台扫图那些调用）或开关关着时，
 * 结果与 `ai_write_importance()` **逐位相同** —— 这是 A/B 的前提。 */
static inline double ai_write_importance_ctx(const AI *ai, const Token *a, const Token *b,
                                             const AiWriteCtx *ctx) {
    double imp = ai_write_importance(ai, a, b);
    if (!ai || !ctx || !ai->wgate_ev) return imp;
    double sup = ai_write_support(ctx);
    /* 权重：从三个**常数项**（trust 0.20 / 惊讶 0.075 / 不累 0.075）里各让一点，
     * 腾出 0.20 给候选证据 —— 常数项对"选哪条"本来就没贡献（标准差 0，见 §6.43）。 */
    size_t ea = a ? a->count : 0, eb = b ? b->count : 0;
    double novelty = (ea < 6 && eb < 6) ? 1.0 : ((ea + eb < 40) ? 0.6 : 0.4);
    double info = 1.0 - ai_pair_jaccard(a, b);
    double trust = (double)ai->inner.predict_accuracy;
    if (trust < 0.0) trust = 0.0;
    if (trust > 1.0) trust = 1.0;
    double mention = (double)((a ? token_hits_total(a) : 0) + (b ? token_hits_total(b) : 0));
    mention = mention / 200.0; if (mention > 1.0) mention = 1.0;
    double surprise = ai->inner.discomfort; if (surprise > 1.0) surprise = 1.0;
    double awake = 1.0 - (double)ai->inner.fatigue; if (awake < 0.0) awake = 0.0;
    double imp2 = 0.20 * novelty + 0.10 * trust + 0.20 * mention
                + 0.20 * info + 0.20 * sup + 0.05 * surprise + 0.05 * awake;
    if (imp2 < 0.0) imp2 = 0.0;
    if (imp2 > 1.0) imp2 = 1.0;
    return imp2;
}
static inline double ai_write_importance2(const AI *ai, const Token *a, const Token *b) {
    return ai_write_importance_ctx(ai, a, b, NULL);
}
static inline const char *ai_write_layer(double imp) {
    if (imp >= AI_WGATE_LONGTERM)  return "长期层";
    if (imp >= AI_WGATE_KNOWLEDGE) return "知识层";
    return "情境层（随口一说）";
}
/* 当前生效的阈值（新项开着就换新公式量出来的那一套）—— 取阈值只有这一处 */
static inline double ai_wgate_knowledge(const AI *ai) {
    return (ai && ai->wgate_ev) ? (double)AI_WGATE_EV_KNOWLEDGE : (double)AI_WGATE_KNOWLEDGE;
}
static inline double ai_wgate_longterm(const AI *ai) {
    return (ai && ai->wgate_ev) ? (double)AI_WGATE_EV_LONGTERM : (double)AI_WGATE_LONGTERM;
}
static inline const char *ai_write_layer_of(const AI *ai, double imp) {
    if (imp >= ai_wgate_longterm(ai))  return "长期层";
    if (imp >= ai_wgate_knowledge(ai)) return "知识层";
    return "情境层（随口一说）";
}


/* ============================ 开关 / 钩子的**唯一**默认值处 ============================
 *
 * 教训：`ai_load_file` **不调用 `ai_new()`**（它自己铺一遍常量默认值，见 model_io.h），
 * 所以任何新的 per-AI 开关如果在 `ai_new` 里初始化，**读盘那条路会拿到 calloc 的 0**
 * —— 实测 `write_gate_off` 就是这么变成"默认开"的（本该默认关）。
 * 这里把（后加的）开关与测量钩子集中成一处，**两条路都调它**。
 * （更早那批常量默认值仍有两份 —— 历史包袱，要动它得单独 A/B。） */
static inline void ai_switches_init(AI *ai) {
    if (!ai) return;
    ai->time_gamma = AI_TIME_GAMMA_DEFAULT;
    ai->prem_hold_activate = 0;      /* 0=只看注意窗口（默认）/1=够得着/2=有据 */
    ai->write_gate_off = 1;          /* 写入分级闸门默认**关** */
    ai->wgate_ev = 1;                /* 候选证据项默认**开**（A/B 三种跑法全绿后才翻的） */
    ai->last_write_imp = -1.0;       /* -1 = 还没写过任何关系事实 */
    ai->last_write_hops = ai->last_write_cred = ai->last_write_ev = -1;
    /* **翻默认（§6.92）**：自撰写入**默认走受审通道**（"所有自撰都受审"是铁律的形状）。
     * 想回到"只有关系事实受审"的老行为：`AIP_ROUTE=0`（显式关）—— 两个方向都能测。 */
    ai->route_writes = 1;
    ai->drive_on = 0;
    /* §6.139 脏思考检查器默认**开**（用户明确要"重复就强制停"）；`AIP_DIRTY=0` 退回。 */
    ai->dirty_guard = 1;
    { const char *dg = getenv("AIP_DIRTY");
      if (dg && dg[0] == '0') ai->dirty_guard = 0;
      else if (dg && dg[0] == '1') ai->dirty_guard = 1; }
    ai->act_on = 0;                  /* 外部执行 + 写文件：默认**关**（action.h 层） */
    ai->asm_check_on = 0;            /* 原语程序干跑校验：默认**关**（token_lang.h） */
    ai->lex_ascii = 0;               /* ASCII 代码词法接进代码通道：默认**关**（lex.h） */
    { const char *rt = getenv("AIP_ROUTE");
      if (rt && rt[0] == '1') ai->route_writes = 1;
      else if (rt && rt[0] == '0') ai->route_writes = 0;   /* 显式关：留一条回到老行为的退路 */ }
    { const char *af = getenv("AIP_DRIVE"); if (af && af[0] == '1') ai->drive_on = 1; }
    { const char *ac = getenv("AIP_ACT");   if (ac && ac[0] == '1') ai->act_on = 1; }
    { const char *ck = getenv("AIP_ASMCHECK"); if (ck && ck[0] == '1') ai->asm_check_on = 1; }
    { const char *lx = getenv("AIP_ASCII");    if (lx && lx[0] == '1') ai->lex_ascii = 1; }
    ai->others_ask = 0;              /* 他者视角进回答路径：默认**关** */
    ai->proc_run = 0;                /* 过程执行（第 9 条 ③）：默认**关** */
    /* **翻默认（§6.106）**：过程执行引擎默认开 —— 配合 §6.105 的干跑默认开，
     * 默认行为是"**在假设世界里演练**"（有痕、无副作用）；真要动的动作仍由动作层把关
     * （`::act on`，默认关）。想回老行为：`AIP_PROCRUN=0`（显式关）。 */
    ai->proc_run = 1;
    { const char *pr = getenv("AIP_PROCRUN");
      if (pr && pr[0] == '1') ai->proc_run = 1;
      else if (pr && pr[0] == '0') ai->proc_run = 0; }
    /* **翻默认（§6.105）**：过程**干跑**默认开 —— 它在"假设世界"里演练（前提不成立就假定成立、
     * 记进假设层、跑完 pop 干净），**没有副作用**，是"会做事"最安全的那一半。想回老行为：
     * `AIP_PROCDRY=0`（显式关）。实跑仍由 `proc_run` 与动作层各自把关。 */
    ai->proc_dry = 1;                /* 干跑（假设世界）：默认**关** */
    { const char *pd = getenv("AIP_PROCDRY");
      if (pd && pd[0] == '1') ai->proc_dry = 1;
      else if (pd && pd[0] == '0') ai->proc_dry = 0;   /* 显式退回老行为（两个方向都测得动） */ }
    /* **翻默认（§6.108）**：过程记号接进对话（`::proc talk`）默认开；`AIP_PROCTALK=0` 退回。 */
    ai->proc_talk = 1;               /* 过程记号接进对话：默认**关** */
    { const char *pt = getenv("AIP_PROCTALK");
      if (pt && pt[0] == '1') ai->proc_talk = 1;
      else if (pt && pt[0] == '0') ai->proc_talk = 0; }
    ai->proc_think = 1;              /* §6.108 翻默认；`AIP_PROCTHINK=0` 退回 */              /* 过程接进思维：默认**关** */
    { const char *pk = getenv("AIP_PROCTHINK");
      if (pk && pk[0] == '1') ai->proc_think = 1;
      else if (pk && pk[0] == '0') ai->proc_think = 0; }
    ai->proc_plan = 1;               /* §6.108 翻默认；`AIP_PROCPLAN=0` 退回 */               /* 过程接进决策（用计划回答）：默认**关** */
    { const char *pl = getenv("AIP_PROCPLAN");
      if (pl && pl[0] == '1') ai->proc_plan = 1;
      else if (pl && pl[0] == '0') ai->proc_plan = 0; }
    ai->cache_walk = 0;              /* 缓存参与自由联想：默认**关** */
    { const char *cw = getenv("AIP_CACHEWALK"); if (cw && cw[0] == '1') ai->cache_walk = 1; }
    ai->eval_num = 0;                /* 求值层（数值比较样板）：默认**关** */
    { const char *en = getenv("AIP_EVALNUM"); if (en && en[0] == '1') ai->eval_num = 1; }
    ai->other_weak_up = 0;           /* 他者上位降权：默认**关** */
    { const char *ow = getenv("AIP_OTHERWEAK"); if (ow && ow[0] == '1') ai->other_weak_up = 1; }
    /* §6.200 **触发接求解（开放式关系问句）默认翻成开**：`猫 属于 什么` / `谁 会 飞` /
     * `哪些 动物 会 飞` 这些"缺一端"的问句，以前掉「我不太懂」；开着会从图里查出来，
     * 查不到时说「图里 没 记 过（不 编）」。**反向测量**（`AIP_RELOPEN=1` 整包 41/41）绿了才翻的。
     * 想回到老行为：`AIP_RELOPEN=0` 或 `::rel open off`。 */
    ai->rel_open = 1;
    /* §6.218 地点块学习（`A V 在 B` / `A 在 B V` ⇒ `A 在 B`）：默认开，
     * `AIP_LOCATIVE=0` 关掉（A/B 用）。 */
    ai->locative = 1;
    { const char *lc = getenv("AIP_LOCATIVE");
      if (lc && lc[0] == '1') ai->locative = 1;
      else if (lc && lc[0] == '0') ai->locative = 0; }
    { const char *ro = getenv("AIP_RELOPEN");
      if (ro && ro[0] == '1') ai->rel_open = 1;
      else if (ro && ro[0] == '0') ai->rel_open = 0; }
    ai->pron_last = 0;               /* 指代消解最小版：默认**关** */
    ai->remind_route = 0;            /* §6.143 提醒写入受审：默认**关**（反向测量：AIP_REMINDROUTE=1）*/
    { const char *rr = getenv("AIP_REMINDROUTE"); if (rr && rr[0] == '1') ai->remind_route = 1; }
    /* §6.233 **自然说法默认翻成开**：`鸟 能 飞 吗`、`猫 是 一种 动物`（陈述与问句两边一致）——
     * 以前默认关 ⇒ 这些最常见的说法要么答"不确定"、要么学不进。
     * 反向整包 `AIP_RELNAT=1` 早就绿了、A/B 也逐字节相同，才翻的。
     * 退回老行为：`AIP_RELNAT=0` 或 `::relnat off`。 */
    ai->rel_nat = 1;
    { const char *rn = getenv("AIP_RELNAT");
      if (rn && rn[0] == '1') ai->rel_nat = 1;
      else if (rn && rn[0] == '0') ai->rel_nat = 0; }
    /* §6.264 **造词默认翻成开**：用户最自然的教法就是「考拉 属于 动物」，以前因为新词学不进、
     * 得先 `::natcoin on`。反向整包（`AIP_NATCOIN=1`）早就 42/42，翻之前又把 `在` 从造词里摘掉
     * （`我 在 想 事情` 会造出"想事情"这种垃圾地点）。退回老行为：`AIP_NATCOIN=0` 或 `::natcoin off`。 */
    ai->nat_coin = 1;
    { const char *nc = getenv("AIP_NATCOIN");
      if (nc && nc[0] == '1') ai->nat_coin = 1;
      else if (nc && nc[0] == '0') ai->nat_coin = 0; }
    /* §6.202 **追问回话默认翻成开**：它说「没把握」之后，你回一句「会/不会/对/不对」
     * 就当教学写进图；给了确定答案之后，回「不对」就改口。以前默认关 ⇒ 这条路等于不存在。
     * 翻之前先修了两处（`::thinklearn off` 时不落标记、空行不作废追问），
     * 反向整包 `AIP_ASKBACK=1` 41/41 绿了才翻。退回老行为：`AIP_ASKBACK=0` 或 `::askback off`。 */
    ai->askback = 1;                 /* §6.168 起默认关；§6.202 起**默认开** */
    { const char *ak = getenv("AIP_ASKBACK");
      if (ak && ak[0] == '1') ai->askback = 1;
      else if (ak && ak[0] == '0') ai->askback = 0; }
    ai->ask_conflict = 1;
    { const char *ac = getenv("AIP_ASKCONFLICT");
      if (ac && ac[0] == '1') ai->ask_conflict = 1;
      else if (ac && ac[0] == '0') ai->ask_conflict = 0; }
    ai->ask_pending = 0;
    ai->ask_restored = 0;
    ai->ask_mark[0] = 0;
    ai->ask_kind = 0;
    ai->ask_a[0] = ai->ask_b[0] = 0;
    ai->last_q_rid = 0; ai->last_q_neg = 0; ai->last_q_b[0] = 0;
    ai->ask_rid = 0;
    ai->ask_neg = 0;
    ai->ask_fact_neg = 0;
    { const char *pl2 = getenv("AIP_PRONLAST"); if (pl2 && pl2[0] == '1') ai->pron_last = 1; }
    ai->wgate_tier = 0;              /* 闸门分层判据：默认**关** */
    { const char *wt = getenv("AIP_WGATETIER"); if (wt && wt[0] == '1') ai->wgate_tier = 1; }
    ai->gen_exec = 0;                /* 生成的原语程序真跑：默认**关**（只生成 + 校验） */
    { const char *ge = getenv("AIP_GENEXEC"); if (ge && ge[0] == '1') ai->gen_exec = 1; }
    ai->code_multi = 0;              /* 多轮自我修正：默认**关** */
    ai->hypo_cache = 0;              /* 假设结论沉淀成条件缓存：默认**关** */
    ai->char_stream = 0;             /* 逐字模式：默认**关**（走分词器，行为不变） */
    ai->cq_n = 0;
    { const char *cs = getenv("AIP_CHARSTREAM"); if (cs && cs[0] == '1') ai->char_stream = 1; }
    { const char *hc = getenv("AIP_HYPOCACHE"); if (hc && hc[0] == '1') ai->hypo_cache = 1; }
    ai->n_hypo_cache = 0;
    ai->n_hc = 0;
    { const char *cm = getenv("AIP_CODEMULTI"); if (cm && cm[0] == '1') ai->code_multi = 1; }
    ai->n1_decide = 0;               /* N1 参与决策：默认**关** */
    { const char *nd = getenv("AIP_N1DECIDE"); if (nd && nd[0] == '1') ai->n1_decide = 1; }
    ai->draft_keep = 0;
    { const char *dk = getenv("AIP_DRAFTKEEP"); if (dk && dk[0] == '1') ai->draft_keep = 1; }
    /* **翻默认（§6.121）**：答复层的**同类互推**默认开 —— "联想出来的东西能用"这条要求
     * 就落在它身上（问 `甲熊 会 冬眠 吗` ⇒ 「大概 —— 同类里 乙熊 也 会 冬眠（…；**类比**，不是直接知识）」）。
     * 措辞自带"大概 + 标明类比"，绝不装成确定；想回老行为：`AIP_RELANALOGY=0` 或 `::rel analogy off`。 */
    ai->rel_analogy = 1;             /* 类比兜底：默认**关** */
    { const char *ra = getenv("AIP_RELANALOGY");
      if (ra && ra[0] == '1') ai->rel_analogy = 1;
      else if (ra && ra[0] == '0') ai->rel_analogy = 0;   /* 显式退回（两个方向都测得动） */ }
    /* **翻默认（§6.124）**：先链后联想默认**开** —— "证明过的答案不许被猜的答案抢掉"。
     * 两包实测（optest 默认档 / `AIP_RELFIRST=1` 档，整包 33 套）都全绿 ⇒ 敢翻；
     * 想回老顺序：`AIP_RELFIRST=0` 或 `::rel first off`。 */
    ai->rel_first = 1;
    { const char *rf = getenv("AIP_RELFIRST");
      if (rf && rf[0] == '1') ai->rel_first = 1;
      else if (rf && rf[0] == '0') ai->rel_first = 0; }   /* 显式退回：两个方向都测得动 */
    /* **翻默认（§6.125）**：记住猜出来的结论默认**开** —— "在交互中学习 / 结论能复用"
     * 这条要求就落在它身上（问 `类9 会 冬眠9 吗` 类比答完后，再问 `甲9 会 冬眠9 吗`
     * 就能拿那条**推测**当中转，答复照旧标「大概…其中一环是推测的」）。
     * 它**不算知识**（`rel_direct` 跳过）⇒ 不会把猜的变成知道的。
     * 想回老行为：`AIP_RELMEM=0` 或 `::rel mem off`。 */
    ai->rel_remember = 1;
    { const char *rmm = getenv("AIP_RELMEM");
      if (rmm && rmm[0] == '1') ai->rel_remember = 1;
      else if (rmm && rmm[0] == '0') ai->rel_remember = 0; }   /* 显式退回：两个方向都测得动 */
    ai->n_rel_guess_mem = 0;
    ai->idle_speak = 0;              /* N4 主动说话：默认**关** */
    { const char *is = getenv("AIP_IDLESPEAK"); if (is && is[0] == '1') ai->idle_speak = 1; }
    ai->note_all = 0;                /* 流水线回复进草稿：默认**关** */
    { const char *na = getenv("AIP_NOTEALL"); if (na && na[0] == '1') ai->note_all = 1; }
    { const char *oa = getenv("AIP_OTHERS"); if (oa && oa[0] == '1') ai->others_ask = 1; }
    { const char *wg = getenv("AIP_WGATE");  if (wg && wg[0] == '1') ai->write_gate_off = 0; }
    /* 显式给 0 就关（默认是开，所以要能反向关掉 —— 反向测量靠它） */
    { const char *we = getenv("AIP_WGATE_EV"); if (we) ai->wgate_ev = (we[0] == '1') ? 1 : 0; }
    /* 对话触发的匹配器：**默认新那套**（说法归一表 + 槽位）；`AIP_TRIGGER=kw` 切回老的 */
    ai->trigger_norm = 1;
    { const char *tg = getenv("AIP_TRIGGER");
      if (tg && !strcmp(tg, "norm")) ai->trigger_norm = 1;
      else if (tg && !strcmp(tg, "kw")) ai->trigger_norm = 0; }
    { const char *ph = getenv("AIP_PREMHOLD");
      if (ph && !strcmp(ph, "activate"))      ai->prem_hold_activate = 1;
      else if (ph && !strcmp(ph, "verified")) ai->prem_hold_activate = 2; }
}

/* 前向声明：词表扩展定义在后面（分词那一带），`ai_new`/懒加载要用它 */
static inline void ai_lexicon_load(AI *ai, const char *path);
static inline void ai_lexicon_ensure(AI *ai);

static inline AI *ai_new(void) {
    AI *ai = (AI *)calloc(1, sizeof *ai);
    if (!ai) return NULL;
    ai->pool = pool_new();
    if (!ai->pool) { free(ai); return NULL; }

    ai_switches_init(ai);
    ai_lexicon_load(ai, "lexicon.txt");       /* 有就加词；没有就算了（行为不变） */
    ai->rng = 0x9E3779B9u;
    ai->gc_rng = 0x85EBCA6Bu;
    ai->energy = 1.0f; ai->decay = 0.7f; ai->cutoff = 0.05f;
    ai->max_gen = 6;
    ai->consolidate_every = 50; ai->decay_factor = 0.98f;
    ai->sleep_max_visits = AI_SLEEP_MAX_VISITS;   /* §6.135：睡眠回放的访问上限（看护可改小） */
    /* §6.184 **测量钩子**：`AIP_SLEEPVISITS=<n>` 覆盖睡眠回放上限（默认仍 20000）。
     * 为什么要它：厚图上一次睡眠单步顶到 20000 访问、截断 ~1 万（§6.183）⇒ 一个周期
     * 0.5–0.8 s 摊在 32 轮上。要改"分摊/缩放"得先量"改小之后睡眠质量（抽象/遗忘）变不变"。 */
    { const char *sv = getenv("AIP_SLEEPVISITS");
      if (sv && sv[0]) { long v = strtol(sv, NULL, 10); if (v > 0) ai->sleep_max_visits = (size_t)v; } }

    ai->gc.gc_min_age = 3000; ai->gc.gc_time_scale = 1000;
    ai->gc.gc_upgrade_rate = 0.15f; ai->gc.gc_interval = 100;
    ai->gc.gc_delete_state = MEM_FORGOTTEN;

    ai->uncertain.w_gap = 0.30f; ai->uncertain.w_conflict = 0.15f;
    ai->uncertain.w_activation = 0.25f; ai->uncertain.w_effort = 0.15f;
    ai->uncertain.w_novelty = 0.15f;
    ai->uncertain.certain_threshold = 0.60f;
    ai->uncertain.unknown_threshold = 0.35f;

    ai->setpoint.setpoint_comfort = 0.70f;
    ai->setpoint.setpoint_arousal = 0.50f;
    ai->setpoint.regulation_rate = 0.15f;

    ai->inner.predict_accuracy = 0.5f;
    ai->inner.comfort = 0.5f;
    ai->inner.arousal = 0.5f;

    ai->n_candidates = 3;
    ai->use_retrieval = 1;          /* 默认优先整句检索 */
    ai->frames = NULL;              /* 结构化帧库，由 frame_io.h 装载 */
    ai->assoc_slot_off = 0;         /* 结构化帧默认允许读关联链产物（见 frame.h Gate B） */
    ai->coin_off = 0;               /* 默认允许现场造词（见 frame.h fr_coin） */
    ai->seg_off = 0;                /* 默认允许换切分找帧（见 frame.h fr_generate） */
    ai->verify_writes = 0;          /* §6.248 默认关（反向测量：AIP_VERIFYWRITES=1 或 ::verifywrites on）*/
    { const char *vw = getenv("AIP_VERIFYWRITES"); if (vw && vw[0] == '1') ai->verify_writes = 1; }
    ai->relay_off = 0;              /* 默认允许中转词容错（见 frame.h FR_RELAY） */
    ai->fall_off = 0;               /* 默认允许降级答复（见 frame.h fr_try_fall） */
    ai->rel_off = 0;                /* 默认允许关系层（见 rel.h） */
    ai->der_style = DER_STYLE_PLAIN;/* 默认「结论 + 推导链」 */
    ai->closed_world = 0;           /* 默认开放世界 */
    ai->view_who = NULL;
    /* 关系独占表的**声明值**（种子）。真正生效的以 ai->rel_excl 为准，
     * 装载后会被 rel_excl_build() 按数据覆盖。 */
    ai->rel_excl[REL_IS_A]    = 1;
    ai->rel_excl[REL_CONTAIN] = 0;
    ai->rel_excl[REL_HAS]     = 0;
    ai->rel_excl[REL_CAN]     = 0;
    ai->rel_excl[REL_FEAR]    = 0;
    ai->rel_excl[REL_LIKE]    = 0;
    ai->rel_excl[REL_EAT]     = 0;
    ai->rel_excl[REL_AT]      = 1;
    ai->rel_excl[REL_NEED]    = 0;   /* 一件事可以有很多前提（多值） */
    ai->sleep_every = 200;
    ai->last_expr_mode = -1;
    ai->imagine_rate = 0.03f;
    ai->inner_dialogue_gain = 0.15f;
    ai->narrate_every = 500;
    ai->drift_sample_every = 200;

    ai->self        = pool_token_intern(ai->pool, "我");
    ai->good        = pool_token_intern(ai->pool, "[符号:好]");
    ai->bad         = pool_token_intern(ai->pool, "[符号:坏]");
    ai->unknowing   = pool_unknowing(ai->pool);
    ai->causal      = pool_token_intern(ai->pool, AI_CAUSAL_MARK);   /* 名字只有一处：token.h */
    ai->social_good = pool_token_intern(ai->pool, "[社会:对]");
    ai->social_bad  = pool_token_intern(ai->pool, "[社会:错]");
    ai->past_self   = pool_token_intern(ai->pool, "[我:过去]");
    ai->future_self = pool_token_intern(ai->pool, "[我:未来]");
    ai->split_tok   = pool_token_intern(ai->pool, AI_DIALOG_SPLIT_NAME);

    token_add_assoc_unique(ai->self, ai->good);
    token_add_assoc_unique(ai->self, ai->bad);
    token_add_assoc_unique(ai->self, ai->unknowing);
    token_add_assoc_unique(ai->self, ai->past_self);
    token_add_assoc_unique(ai->self, ai->future_self);

    ai_init_expr_pools(ai);
    return ai;
}

/* §6.191 `frames_free` 在 frame_io.h（本文件之后才包含）⇒ 这里留一个函数指针，
 * main/tools 在包含完 frame_io.h 之后把它指向真正的实现（见 frame_io.h 末尾）。
 * 不直接 #include 是为了保持"ai.h 不依赖 frame 层"这条包含顺序。 */
/* §6.582 这个钩子是**所有线程共用**的（`frames_load` 每次读盘都会挂一遍，值恒等）；
 * 用原子指针：并发建脑时不再有"写一半被别人读"的竞争。 */
typedef void (*AiFramesFreeFn)(AI *ai);
AIP_ATOMIC_PTR(AiFramesFreeFn, ai_frames_free, NULL);

static inline void ai_free(AI *ai) {
    if (!ai) return;
    for (size_t i = 0; i < ai->lex_n; ++i) free(ai->lex_extra[i]);
    free(ai->lex_extra);
    ai->lex_extra = NULL; ai->lex_n = 0;
    pool_free(ai->pool);
    free(ai->trace);
    free(ai->sc_memo); free(ai->sc_cov);      /* §6.132 打分备忘（calloc 起家，可能是 NULL） */
    free(ai->cb_bits);                        /* §6.136 库句字符集位图（同上，可能是 NULL）*/
    free(ai->cb_off); free(ai->cb_ptr); free(ai->cb_len);   /* §6.140 库句字符切片 */
    free(ai->cb_bg);                          /* §6.210 库句 bigram 布隆位 */
    free(ai->last_output);
    free(ai->last_skipped);
    free(ai->pairs);
    free(ai->idf);
    ai->idf = NULL;
    ai->idf_n = 0;
    free(ai->last_idea);
    /* §6.191 **帧库也要还**：`frames_load` 每次 calloc 一整个 `FrameSet`（出厂 frames.txt
     * ≈ 900 KB），以前 `ai_free` 只 free 了 AI 自己 ⇒ 每 `::load` 一次泄 900 KB。
     * 这个函数在 frame_io.h（ai.h 之后才被包含）⇒ 这里用**前向声明**的那份（见文件上方）。 */
    { AiFramesFreeFn ff = AIP_LD(ai_frames_free); if (ff) ff(ai); }
    for (size_t i = 0; i < ai->dynrel_n; ++i) free(ai->dynrel_name[i]);
    free(ai->micro);        /* 微内核状态；内建补丁用静态壳，没有别的分配 */
    free(ai->think);        /* 思维循环状态（议程/未解问题/痕迹） */
    free(ai);
}

/* ============================ 分词 ============================ */

/* 固定词表：中文里必须整体成词的常用多字词。
 *
 * 为什么要有它：只靠「池里认识的词」做最长匹配会陷入鸡生蛋 —— 池里没有多字词就
 * 永远切不出多字词，于是所有中文都被切成单字，生成出来就是「好 活 动 物 质」。
 * 放进一张固定词表后，**语料构建和用户输入走的是同一张表**，切法必然一致，
 * 而且不依赖任何运行顺序。表外的词自动退回单字（也是两侧一致）。 */
static const char *const AI_LEXICON[] = {
    /* 这张表决定 CJK 怎么切词：语料构建和用户输入共用它，切法必然一致。
     * 表外的多字词退回单字切分。内容由语料 + 基础功能词生成。 */
    /* 6 字词 */
    "很高兴见到你",
    /* 4 字词 */
    "什么时候", "春夏秋冬", "是什么样", "说不出来",
    /* 3 字词 */
    "不喜欢", "不客气", "不知道", "不认识", "为什么", "什么样", "你好吗",
    "加衣服", "听起来", "告诉我", "喜欢你", "在哪里", "好不好", "对不对", "对不起",
    "差不多", "开心吗", "很难过", "怎么办", "怎么样", "想知道", "我在的", "早上好",
    "是不是", "是什么", "晚上好", "有意思", "有没有", "没关系", "的时候", "累了吗",
    "能不能", "记得吗", "陌生人", "饿了吗",
    /* 2 字词 */
    "一下", "一个", "一会", "一切", "一只", "一天", "一定", "一样",
    "一点", "一直", "一种", "一类", "一起", "一遍", "三十", "上去",
    "下午", "下去", "下来", "下次", "下雨", "不仅", "不会", "不好",
    "不懂", "不是", "不用", "不错", "世界", "东西", "中午", "中间",
    "之前", "之间", "乐意", "也许", "了解", "事情", "事物", "互相",
    "交给", "什么", "今天", "从前", "从来", "他们", "以为", "以前",
    "以后", "以来", "价值", "休息", "伙伴", "会吗", "伤害", "但是",
    "体谅", "作业", "你们", "你好", "佩服", "保护", "保暖", "保证",
    "值得", "假如", "做梦", "偶尔", "充实", "公园", "关心", "关系",
    "兴趣", "其实", "再见", "再说", "写字", "农民", "几个", "几点",
    "出去", "出来", "分享", "刚刚", "刚才", "别人", "办法", "动物",
    "助手", "努力", "医生", "医院", "危险", "厉害", "原来", "原谅",
    "反义", "发生", "发誓", "受伤", "句子", "只有", "只要", "可以",
    "可能", "司机", "合理", "同意", "名字", "后悔", "听话", "告诉",
    "哥哥", "哪个", "哪儿", "哪里", "商店", "善良", "喜欢", "回来",
    "回答", "因为", "因果", "在吗", "地上", "地方", "地点", "坚持",
    "城市", "声音", "外面", "多少", "多谢", "夜晚", "大人", "大家",
    "大概", "天上", "天体", "天气", "太阳", "失望", "失败", "头发",
    "女人", "奶奶", "她们", "好久", "好人", "好吗", "好奇", "好好",
    "好看", "如何", "如果", "如此", "妈妈", "妹妹", "姐姐", "存在",
    "季节", "孤单", "学习", "学校", "学生", "学过", "孩子", "安心",
    "安静", "完全", "客气", "害怕", "家人", "家里", "对吗", "将来",
    "小孩", "小心", "就是", "尽力", "尾巴", "属于", "山上", "嵌套",
    "工人", "工作", "工具", "工厂", "已经", "希望", "帮助", "帮忙",
    "帮我", "平常", "平静", "幸福", "幸运", "应该", "建议", "开始",
    "开心", "弟弟", "当然", "形状", "彼此", "很多", "很快", "心思",
    "心情", "心里", "忘掉", "忘记", "快乐", "怎么", "您好", "情绪",
    "惊喜", "惊讶", "想法", "想起", "意义", "意思", "感到", "感动",
    "感情", "感染", "感激", "感觉", "感谢", "愿意", "慢慢", "懂吗",
    "成功", "我们", "我在", "或者", "房子", "所以", "手机", "才能",
    "找到", "担心", "拒绝", "拥有", "支持", "收获", "改变", "放弃",
    "放心", "放松", "故事", "故意", "敌人", "教我", "散开", "整理",
    "方便", "方向", "无聊", "日子", "早上", "时候", "时间", "明天",
    "明白", "星星", "昨天", "是的", "是谁", "晚上", "晚点", "最好",
    "最近", "月亮", "有意", "有点", "朋友", "未来", "本来", "机会",
    "机器", "村庄", "条件", "果子", "树上", "样子", "桌子", "椅子",
    "植物", "欣赏", "每个", "每天", "水里", "永远", "没事", "没有",
    "注意", "活动", "活着", "清楚", "温暖", "温柔", "游戏", "游泳",
    "满意", "满足", "漂亮", "激动", "热闹", "然后", "照顾", "爷爷",
    "爸爸", "物质", "特别", "现在", "珍惜", "理由", "理解", "生命",
    "生气", "生活", "电脑", "男人", "痛苦", "白天", "的话", "相信",
    "相同", "看到", "看看", "看见", "真的", "真话", "眼睛", "着急",
    "睡觉", "知识", "知道", "确定", "礼物", "社会", "秘密", "程序",
    "稍等", "空气", "空间", "立刻", "第一", "答应", "答案", "简单",
    "算数", "紧张", "练习", "终于", "经常", "结尾", "结束", "继续",
    "羞愧", "翅膀", "老师", "老虎", "老鼠", "而且", "耐心", "职业",
    "聪明", "肚子", "肯定", "自己", "自然", "自由", "舒服", "节日",
    "虽然", "行动", "行吗", "衣服", "见到", "见面", "觉得", "解释",
    "认为", "认真", "认识", "讨厌", "让我", "记住", "记得", "记忆",
    "试试", "语言", "说话", "请求", "谎言", "谎话", "谢谢", "走走",
    "起来", "路上", "身份", "身体", "身边", "转折", "过去", "过得",
    "过来", "过程", "还是", "这个", "这些", "这样", "这里", "进去",
    "远处", "选择", "递进", "道歉", "那个", "那么", "那些", "那样",
    "那里", "重要", "长大", "问题", "附近", "陌生", "随时", "难过",
    "需要", "非常", "音乐", "顺便", "颜色", "飞机", "食物", "马上",
    "骄傲", "骨头", "高兴",
    NULL
};

static inline int ai_in_lexicon(const char *w) {
    for (size_t i = 0; AI_LEXICON[i]; ++i)
        if (strcmp(AI_LEXICON[i], w) == 0) return 1;
    return 0;
}

/* 扩展词表（`lexicon.txt`）里有没有这个词 —— 一行一个，`#` 开头是注释 */
/* 懒加载（⚠ `ai_load_file` **不走 `ai_new()`** —— 它自己铺一遍常量默认值，
 * 所以装载只写在 `ai_new` 里的话，读盘路径永远读不到词表文件。§6.84 实测踩到）。
 * `ai_match_word` 拿的是 const 指针，这里去掉 const 只是为了复用同一个入口（与 §6.30 同例）。 */
static inline void ai_lexicon_ensure(AI *ai) {
    if (!ai || ai->lex_loaded) return;
    ai->lex_loaded = 1;
    ai_lexicon_load(ai, "lexicon.txt");
}

static inline int ai_lex_extra_has(const AI *ai, const char *w) {
    if (!ai || !ai->lex_extra || !w) return 0;
    for (size_t i = 0; i < ai->lex_n; ++i)
        if (ai->lex_extra[i] && strcmp(ai->lex_extra[i], w) == 0) return 1;
    return 0;
}

/* 读 `lexicon.txt`（没有就当空；一行一个词，允许前后空白与 `#` 注释）。
 * **为什么是文件而不是硬编码**：词表是**数据**（跟 frames.txt / relations.txt 一样）——
 * 加词不用改代码、不用动存档格式；文件不在时行为与今天逐字节相同。 */
static inline void ai_lexicon_load(AI *ai, const char *path) {
    if (!ai || !path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        char *e = q + strlen(q);
        while (e > q && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*q || *q == '#') continue;
        if (strlen(q) < 4) continue;                  /* 单字/两字节的没意义 */
        char **nw = (char **)realloc(ai->lex_extra, (ai->lex_n + 1) * sizeof *nw);
        if (!nw) break;
        ai->lex_extra = nw;
        ai->lex_extra[ai->lex_n] = (char *)malloc(strlen(q) + 1);
        if (!ai->lex_extra[ai->lex_n]) break;
        memcpy(ai->lex_extra[ai->lex_n], q, strlen(q) + 1);
        ai->lex_n++;
    }
    fclose(f);
}

/* 一个 UTF-8 字符占几个字节（只认合法起始字节） */
static inline size_t cjk_char_len(const unsigned char *p) {
    if ((*p & 0xE0) == 0xC0) return 2;
    if ((*p & 0xF0) == 0xE0) return 3;
    if ((*p & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 从 text 的 pos 起，切出一个「池里认识的」最长单词。
 *
 * 这一步是整个对话能力的关键：语料里写的是多字词（「什么」「我们」「老师」），
 * 而用户输入如果不按同一套词汇去切，就会出现「语料里的 `什么`」和「输入切出来的
 * `什` + `么`」是两个不同符号节点的错位 —— 表现为答非所问/只会复读。
 * 所以这里做「最长优先」的词汇匹配：先试 4/3/2 字，池里没有就退回单字。 */
static inline size_t ai_match_word(const AI *ai, const unsigned char *p, size_t remain) {
    if (!ai || !ai->pool) return 0;
    ai_lexicon_ensure((AI *)ai);          /* 词表扩展：第一次分词时才读（两条构造路都覆盖） */
    size_t best = 0;
    size_t off = 0, maxc = 0;
    while (off < remain && maxc < 8) { off += cjk_char_len(p + off); maxc++; }
    if (off > remain) off = remain;
    /* 从长到短试 */
    size_t candidates[8]; size_t nc = 0;
    size_t o2 = 0, c2 = 0;
    while (o2 < off && c2 < 8) {
        size_t l = cjk_char_len(p + o2);
        if (o2 + l > off) break;
        o2 += l; c2++;
        candidates[nc++] = o2;
    }
    while (nc > 0) {
        size_t l = candidates[--nc];
        if (l < 2) break;                       /* 单字不用查池，直接兜底 */
        char buf[32];
        if (l >= sizeof buf) continue;
        memcpy(buf, p, l); buf[l] = 0;
        if (ai_in_lexicon(buf) || pool_find(ai->pool, buf) || ai_lex_extra_has(ai, buf)) {
            best = l; break;
        }
    }
    if (best == 0) best = cjk_char_len(p);      /* 兜底：单字 */
    if (best > remain) best = remain;
    return best;
}

static inline char **ai_tokenize(AI *ai, const char *text, size_t *out_n) {
    *out_n = 0;
    if (!text) return NULL;
    size_t cap = 16, n = 0;
    char **words = (char **)malloc(cap * sizeof *words);
    if (!words) return NULL;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        /* 原语标记是一个整体：[原语:对象]、[原语:分割] …
         * 必须先于「单字节 ASCII」分支处理，否则 '[' 会被当成一个孤立标点，
         * 整个标记被拆成 原|语|对|象 —— 与语料里的符号节点完全对不上。 */
        if (*p == '[') {
            const char *close = strchr((const char *)p, ']');
            if (close && close > (const char *)p + 1) {
                size_t len = (size_t)(close - (const char *)p) + 1;
                if (n == cap) { cap *= 2; words = (char **)realloc(words, cap * sizeof *words); }
                char *w = (char *)malloc(len + 1);
                if (!w) break;
                memcpy(w, p, len); w[len] = 0;
                words[n++] = w;
                p = (const unsigned char *)close + 1;
                continue;
            }
        }
        if (isspace(*p) || (*p < 128 && ispunct(*p))) { p++; continue; }
        size_t len;
        if (*p >= 0x80) {
            /* 池里有的多字词优先；没有就单字 */
            size_t remain = strlen((const char *)p);
            len = ai ? ai_match_word(ai, p, remain) : cjk_char_len(p);
            if (len == 0) len = cjk_char_len(p);
        } else {
            const unsigned char *s = p;
            while (*p && *p < 128 && !isspace(*p) && !ispunct(*p)) p++;
            len = (size_t)(p - s);
            if (len == 0) { p++; continue; }
            if (n == cap) { cap *= 2; words = (char **)realloc(words, cap * sizeof *words); }
            char *w = (char *)malloc(len + 1);
            if (!w) break;
            memcpy(w, s, len); w[len] = 0;
            words[n++] = w;
            continue;
        }
        if (n == cap) { cap *= 2; words = (char **)realloc(words, cap * sizeof *words); }
        char *w = (char *)malloc(len + 1);
        if (!w) break;
        memcpy(w, p, len); w[len] = 0;
        words[n++] = w;
        p += len;
    }
    *out_n = n;
    return words;
}
static inline void ai_free_words(char **words, size_t n) {
    if (!words) return;
    for (size_t i = 0; i < n; ++i) free(words[i]);
    free(words);
}
static inline void ai_learn_text(AI *ai, const char *text) {
    if (!ai || !text) return;
    size_t n;
    char **words = ai_tokenize(ai, text, &n);
    if (!words || n == 0) { ai_free_words(words, n); return; }
    token_observe_names(ai->pool, (const char *const *)words, n, 2);
    ai->n_learned += n;
    ai_free_words(words, n);
}

/* ============================ 内生状态 ============================ */

static inline void ai_inner_update(AI *ai, float surprise, int correct) {
    if (!ai) return;
    AIInner *s = &ai->inner;
    s->predict_accuracy = s->predict_accuracy * 0.95f + (correct ? 1.0f : 0.0f) * 0.05f;
    s->surprise_level = s->surprise_level * 0.95f + (surprise > 1 ? 1.0f : surprise) * 0.05f;
    s->comfort = s->predict_accuracy * 0.9f + 0.1f;
    s->discomfort = s->surprise_level * 0.9f + 0.1f;
    s->arousal = s->arousal * 0.9f + (correct ? 0.1f : 0.9f) * 0.1f;
    s->fatigue += 0.001f;
    if (s->fatigue > 1.0f) s->fatigue = 1.0f;

    Token *self = ai->self;
    if (!self) return;
    if (correct) {
        for (size_t i = 0; i < self->count; ++i)
            if (token_assocs(self)[i].main == ai->good)
                token_touch_entry(self, &token_assocs(self)[i]);
    } else {
        for (size_t i = 0; i < self->count; ++i)
            if (token_assocs(self)[i].main == ai->bad)
                token_touch_entry(self, &token_assocs(self)[i]);
    }
}

/* ============================ 设定点 ============================ */

static inline float ai_comfort_error(const AI *ai) {
    return ai->inner.comfort - ai->setpoint.setpoint_comfort;
}
static inline float ai_arousal_error(const AI *ai) {
    return ai->inner.arousal - ai->setpoint.setpoint_arousal;
}
static inline int ai_exploration_bias(const AI *ai) {
    float ec = -ai_comfort_error(ai), ea = -ai_arousal_error(ai);
    float drive = (ec + ea) * 0.5f;
    float bias = 1.0f + drive * (1.0f + ai->setpoint.regulation_rate * 5.0f);
    if (bias < 0.5f) bias = 0.5f;
    if (bias > 2.5f) bias = 2.5f;
    int n = (int)((float)ai->n_candidates * bias + 0.5f);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return n;
}
static inline int ai_effective_candidates(const AI *ai) { return ai_exploration_bias(ai); }
static inline float ai_effective_energy(const AI *ai) {
    float e = ai->energy * (1.0f - ai->inner.fatigue * 0.5f)
                          * (0.7f + ai->inner.arousal * 0.6f);
    if (e > 1.0f) e = 1.0f;
    if (e < 0.1f) e = 0.1f;
    return e;
}

/* ============================ 多焦点 ============================ */

static inline void ai_focus_push(AI *ai, Token *t, float w) {
    if (!ai || !t || t->is_garbage) return;
    for (size_t i = 0; i < ai->focus_n; ++i) {
        if (ai->focus_ring[i] == t) {
            ai->focus_weight[i] += w;
            if (ai->focus_weight[i] > 3.0f) ai->focus_weight[i] = 3.0f;
            return;
        }
    }
    if (ai->focus_n == AI_MAX_FOCUS) {
        size_t worst = 0;
        for (size_t i = 1; i < AI_MAX_FOCUS; ++i)
            if (ai->focus_weight[i] < ai->focus_weight[worst]) worst = i;
        ai->focus_ring[worst] = t; ai->focus_weight[worst] = w;
        return;
    }
    ai->focus_ring[ai->focus_n] = t;
    ai->focus_weight[ai->focus_n] = w;
    ai->focus_n++;
}
static inline float ai_focus_get(const AI *ai, const Token *t) {
    if (!ai || !t) return 0.0f;
    for (size_t i = 0; i < ai->focus_n; ++i)
        if (ai->focus_ring[i] == t) return ai->focus_weight[i];
    return 0.0f;
}
/* ============================ 前提成立吗（推理层按前提决定用不用） ============================
 *
 * 用户的话：「**联想本来不依赖前提**，可以正常联想……但是逻辑会知道这两个之间的前提
 * 符号节点是什么」+「推理/思维层按前提决定用不用」。所以判据放在**逻辑**这一侧：
 * **前提符号节点出现在当前注意窗口里（输入里的词会被推进焦点环）→ 前提成立**。
 * 它只看注意力，不看权重：简单、确定、说出来也听得懂（"你刚才提到过它"）。
 * 联想/走边**不看这个** —— 那是刻意的（前提不该挡住回忆）。 */
/* 前提在图上"够得着"吗：从当前注意窗口出发做一次**受限扩散**（深度 ≤2、能量 ≥0.25），
 * 看这条前提会不会被激活到。判据全在图里，不新造状态。 */
typedef struct { const Token *want; int hit; } PremProbe;

static inline void prem_probe_visit(Token *t, size_t depth, float energy, void *user) {
    (void)depth;
    PremProbe *pr = (PremProbe *)user;
    if (pr && t == pr->want && energy >= 0.25f) pr->hit = 1;
}

static inline int ai_premise_reachable(const AI *aic, const Token *p) {
    if (!aic || !p || aic->focus_n == 0) return 0;
    /* 扩散只**读**图（token_touch 是时间戳维护），所以这里把 const 去掉用；AI 本身不改。 */
    AI *ai = (AI *)aic;
    PremProbe pr;
    pr.want = p; pr.hit = 0;
    ActivateCtx act;
    activate_ctx_init(&act, 0.7f, 0.25f, 2);      /* 衰减/门槛/深度：只看两步 */
    act.on_visit = prem_probe_visit;
    act.user = &pr;
    for (size_t i = 0; i < ai->focus_n && !pr.hit; ++i) {
        Token *seed = ai->focus_ring[i];
        if (!seed || seed == p) continue;
        if (ai->focus_weight[i] < 0.30f) continue; /* 只从"现在确实还在意"的词出发 */
        token_activate_from(seed, (Token *const *)ai->focus_ring, ai->focus_n, 0.9f, &act);
    }
    activate_ctx_free(&act);
    return pr.hit;
}

/* 前提成立吗：① 在注意窗口里（默认档）；② 开了第二档时，**图上够得着**也算。
 * 注意：它只回答"前提成立不成立"，不回答"世界上是不是真的" —— 那是知识层的事。 */
/* 前向声明：求值层在文件后半（定义处有注释），前提判据要先用它 */
static inline int ai_eval_cmp(const char *expr, int *out);

#define AI_HYPO_PREFIX "[假设栈:"
/* **整条假设栈的指纹**（§6.118）：按**层序**把每一层的每条事实（a/b/rid/neg）都喂进 FNV-1a。
 * ⚠ 必须用**整栈**，不能用最深那层 —— 多层嵌套时"最深一样、外层的假设不同"会算出同一个指纹，
 * 于是缓存会被错误地留着（用户 2026-10-11 明确点名这一点）。栈空时给全 0 指纹（不进任何缓存）。 */
static inline void ai_hypo_fingerprint(const AI *ai, char *out, size_t cap) {
    if (!out || cap < 12) { if (out && cap) out[0] = 0; return; }
    uint32_t h = 2166136261u;
    if (ai) {
        uint32_t dep = (uint32_t)ai->hypo.depth;
        for (uint32_t k = 0; k < 4; ++k) { h ^= (dep >> (8 * k)) & 0xFFu; h *= 16777619u; }
        for (size_t i = 0; i < ai->hypo.depth; ++i) {
            const HypoFrame *f = &ai->hypo.fr[i];
            uint32_t nn = (uint32_t)f->n;
            for (uint32_t k = 0; k < 4; ++k) { h ^= (nn >> (8 * k)) & 0xFFu; h *= 16777619u; }
            for (size_t j = 0; j < f->n; ++j) {
                const HypoFact *fa = &f->f[j];
                uint32_t vals[4] = { fa->a ? fa->a->id : 0u, fa->b ? fa->b->id : 0u,
                                     (uint32_t)fa->rid, (uint32_t)fa->neg };
                for (int q = 0; q < 4; ++q)
                    for (uint32_t k = 0; k < 4; ++k) { h ^= (vals[q] >> (8 * k)) & 0xFFu; h *= 16777619u; }
            }
        }
    }
    snprintf(out, cap, AI_HYPO_PREFIX "%08x]", (unsigned)h);
}

/* **顺序接地**（§6.120）：把一段 UTF-8 文本按**字符**（不是字节）依次接进队列。
 * 队列满就丢**最老**的（滚动窗口 —— 顺序不乱，这是"接地"的关键）。 */
static inline size_t ai_utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
static inline void ai_char_ground(AI *ai, const char *text) {
    if (!ai || !text) return;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        size_t l = ai_utf8_len(*p);
        for (size_t k = 0; k < l; ++k) if (!p[k]) { l = k; break; }
        if (ai->cq_n + l > sizeof ai->cq) {           /* 满了：丢最老的一个字符 */
            size_t drop = ai_utf8_len((unsigned char)ai->cq[0]);
            if (drop > ai->cq_n) drop = ai->cq_n;
            memmove(ai->cq, ai->cq + drop, ai->cq_n - drop);
            ai->cq_n -= drop;
            continue;
        }
        for (size_t k = 0; k < l; ++k) ai->cq[ai->cq_n++] = (char)p[k];
        p += l;
    }
}
/* 把队列里的字符按顺序切成"词"（每个 UTF-8 字符一个词）—— 返回条数。
 * `buf` 用来存每段的终止符（调用方给 n 个 '\0' 的空间）。 */
static inline size_t ai_char_words(const AI *ai, const char **w, char *buf, size_t n) {
    if (!ai || !w || !buf) return 0;
    size_t i = 0, k = 0, cnt = 0;
    while (i < ai->cq_n && cnt < n) {
        size_t l = ai_utf8_len((unsigned char)ai->cq[i]);
        if (i + l > ai->cq_n) l = ai->cq_n - i;
        memcpy(buf + k, ai->cq + i, l);
        buf[k + l] = 0;
        w[cnt++] = buf + k;
        k += l + 1;
        i += l;
    }
    return cnt;
}

/* **逐字"分词"**（§6.120）：先顺序接地，再把每个 UTF-8 字符当成一个"词"。
 * 返回形状与 `ai_tokenize` 一致（`char**` + 每项单独 malloc）⇒ 调用方的释放路径一个字都不用改 ✓。 */
static inline char **ai_char_tokenize(AI *ai, const char *input, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!ai || !input) return NULL;
    ai_char_ground(ai, input);                      /* ① 顺序接地（待会要用） */
    const char *w[512];
    char buf[512 * 5];                 /* ⚠ 最坏 = 512 个 4 字节 UTF-8 字 + 每段一个终止符 */
    size_t n = ai_char_words(ai, w, buf, 512);
    if (!n) return NULL;
    char **arr = (char **)calloc(n, sizeof *arr);
    if (!arr) return NULL;
    for (size_t i = 0; i < n; ++i) {
        arr[i] = (char *)malloc(strlen(w[i]) + 1);
        if (!arr[i]) { ai_free_words(arr, i); return NULL; }
        memcpy(arr[i], w[i], strlen(w[i]) + 1);
    }
    if (out_n) *out_n = n;
    return arr;
}

static inline int ai_premise_holds(const AI *ai, const Token *p) {
    if (!ai || !p) return 0;
    /* **§6.118 假设栈指纹型前提**：P = `[假设栈:xxxxxxxx]` ⇒ 它成立当且仅当
     * **当前整条假设栈**的指纹正好是这个 —— 栈一撤（或换成别的假设）立刻不成立，
     * 于是那条条件缓存自动失效（不用去删它 ✓）。栈空时一律不成立。 */
    if (p->name && strncmp(p->name, AI_HYPO_PREFIX, strlen(AI_HYPO_PREFIX)) == 0) {
        if (!ai->hypo.depth) return 0;
        char fp[32];
        ai_hypo_fingerprint(ai, fp, sizeof fp);
        return strcmp(fp, p->name) == 0;
    }
    /* **§6.98 求值层也管前提**（② 档那条的延伸）：前提的名字本身是**可计算的**
     * （`3<5` 这种）时，先真算一遍 —— 图内三档（提到过/够得着/有据）对"算式"永远说不清。
     * 只在 `eval_num` 打开时生效（默认关 ⇒ 与今天逐字节相同）；算不出来就照旧走图内判据。 */
    if (ai->eval_num && p->name && p->name[0]) {
        int v = 0;
        if (ai_eval_cmp(p->name, &v)) return v;
    }
    for (size_t i = 0; i < ai->focus_n; ++i)
        if (ai->focus_ring[i] == p) return 1;
    if (ai->prem_hold_activate >= 1) {
        if (!ai_premise_reachable(ai, p)) return 0;
        /* 第三档：**够得着还不够，前提本身要有据**（借 evaluated vs asserted 的二分） */
        if (ai->prem_hold_activate >= 2 && !ai_premise_wellfounded(ai, p)) return 0;
        return 1;
    }
    return 0;
}

/* 一条前提的条件成立吗：+1 = 要它在窗口里；-1 = 要它**不在**窗口里。 */
static inline int ai_premise_cond_ok(const AI *ai, const Token *p, int pol) {
    if (!ai || !p) return 0;
    int holds = ai_premise_holds(ai, p);
    return pol < 0 ? !holds : holds;
}

/* A 经前提到 B 这条连结现在能用吗：没有前提 → 能用；有前提 → 条件全成立才用。 */
static inline int ai_premise_link_ok(const AI *ai, const Token *from, const Token *to) {
    const Token *ps[4];
    int pol[4];
    size_t np = token_premises(from, to, ps, pol, 4);
    for (size_t i = 0; i < np; ++i)
        if (!ai_premise_cond_ok(ai, ps[i], pol[i])) return 0;
    return 1;
}

static inline void ai_focus_decay(AI *ai, float factor) {
    if (!ai) return;
    for (size_t i = 0; i < ai->focus_n; ) {
        ai->focus_weight[i] *= factor;
        if (ai->focus_weight[i] < 0.1f) {
            ai->focus_ring[i] = ai->focus_ring[ai->focus_n - 1];
            ai->focus_weight[i] = ai->focus_weight[ai->focus_n - 1];
            ai->focus_n--;
        } else i++;
    }
}
static inline void ai_set_focus(AI *ai, Token *t) { if (ai) ai->focus = t; }

/* ============================ 自我模型 ============================ */

static inline void ai_self_update(AI *ai) {
    if (!ai || !ai->self) return;
    Token *self = ai->self;
    float c = ai->inner.comfort, a = ai->inner.arousal, f = ai->inner.fatigue;
    for (size_t i = 0; i < self->count; ++i) {
        AssocEntry *e = &token_assocs(self)[i];
        if (e->main == ai->good) {
            if (c * (1.0f - f) > 0.5f) token_touch_entry(self, e);
        } else if (e->main == ai->bad) {
            if ((1.0f - c) * a > 0.4f) token_touch_entry(self, e);
        }
    }
}

/* 强化一次「输入尾词 → 分隔符」和「分隔符 → 回复首词」。
 * 语料里每出现一个分隔符，就说明前面是输入、后面是回复 —— 这两条边是「接话」的
 * 主干，必须比普通的共现边重，否则从输入尾词出发时会被高频虚词的边抢走。 */
static inline void ai_boost_reply_bridge(AI *ai, Token *tail, Token *head) {
    if (!ai || !ai->split_tok) return;
    Token *sp = ai->split_tok;
    if (tail) {
        for (size_t i = 0; i < tail->count; ++i) {
            AssocEntry *e = &token_assocs(tail)[i];
            if (e->main != sp) continue;
            /* 顶 24 次，相当于在语料里多见了 24 遍 */
            if (e->hits < 24) e->hits = 24;
            break;
        }
    }
    if (head) {
        for (size_t i = 0; i < sp->count; ++i) {
            AssocEntry *e = &token_assocs(sp)[i];
            if (e->main != head) continue;
            if (e->hits < 24) e->hits = 24;
            else e->hits += 4;
            break;
        }
    }
}

/* ============================ 权重 ============================ */

static inline uint32_t ai_edge_weight(const AI *ai, const AssocEntry *e) {
    /* 线性 hits，但封顶。开方会把所有边压到同一量级、让游走退化成随机漫步
     * （实测反而更容易漂进 反义/什么 这类环），所以保留线性、只压住极端长尾。 */
    uint32_t h = e->hits + 1;
    if (h > 400) h = 400;
    uint32_t w = h;
    if (w < 1) w = 1;
    if (e->mem_state == MEM_FADING) w = (w + 1) / 2;
    /* 从分隔符出发的边 = 「回复首词」。它比别的续接重要得多，单独加权，
     * 否则游走很容易在回复开头就漂到无关的高频词上（表现为答非所问）。 */
    if (ai->split_tok && e->hits && e->hits >= (ai->split_tok->count / 8 + 1))
        w *= 4;
    if (ai->focus_n > 0) {
        float fw = ai_focus_get(ai, e->main);
        if (fw > 0.0f) w = (uint32_t)((float)w * (1.0f + fw * 2.0f));
        /* v10：边上没有语境字段了，所以不再有"语境词也在注意环里 → 再加权"那一档
         * （那一档只在带语境的边上生效，而出厂模型一条都没有）。 */
    }
    return w;
}

/* ============================ 预测 / 惊讶 ============================ */

static inline Token *ai_predict_readonly(const AI *ai, Token *cur) {
    if (!cur || cur->count == 0) return NULL;
    AssocEntry *best = NULL;
    uint32_t best_w = 0;
    for (size_t i = 0; i < cur->count; ++i) {
        AssocEntry *e = &token_assocs(cur)[i];
        if (edge_is_causal_guess(e)) continue;   /* 因果猜想不是知识，预测不认它 */
        if (e->mem_state >= MEM_DORMANT) continue;
        if (e->main && e->main->name && e->main->name[0] == '[') continue;  /* 跳过原语 */
        uint32_t w = ai_edge_weight(ai, e);
        if (w > best_w) { best_w = w; best = e; }
    }
    return best ? best->main : NULL;
}
static inline void ai_weaken_edge(Token *from, Token *to) {
    if (!from || !to) return;
    for (size_t i = 0; i < from->count; ++i) {
        AssocEntry *e = &token_assocs(from)[i];
        if (e->main != to || edge_is_causal_guess(e)) continue;
        if (e->hits > 0) e->hits--;
        if (e->hits == 0 && e->mem_state == MEM_ACTIVE) e->mem_state = MEM_FADING;
        return;
    }
}
static inline void ai_learn_pair(AI *ai, Token *prev, Token *actual) {
    if (!ai || !prev || !actual) return;
    Token *pred = ai_predict_readonly(ai, prev);
    if (!pred || pred == actual) {
        token_observe_pair(prev, actual);
    } else {
        token_observe_pair(prev, actual);
        ai_weaken_edge(prev, pred);
        ai->n_surprises++;
    }
}
static inline void ai_push_trace(AI *ai, Token *t) {
    if (!ai || !t) return;
    if (ai->trace_n == ai->trace_cap) {
        size_t ncap = ai->trace_cap ? ai->trace_cap * 2 : 64;
        Token **nt = (Token **)realloc(ai->trace, ncap * sizeof *nt);
        if (!nt) return;
        ai->trace = nt; ai->trace_cap = ncap;
    }
    ai->trace[ai->trace_n++] = t;
    if (ai->trace_n > 2048) {
        /* §6.575 用"当前长度的一半"而不是常量 1024：语义一样（2048 时就是 1024），
         * 但 GCC 看不到 realloc 后的容量时会按"常量 1024 × 8 > 已分配 512"报
         * `-Werror=array-bounds` 假阳性 —— 这是加了 part_touch 之后内联变化才暴露的。 */
        size_t half = ai->trace_n / 2;
        memmove(ai->trace, ai->trace + half, half * sizeof *ai->trace);
        ai->trace_n = half;
    }
}

/* ============================ 不确定性 ============================ */

static inline float sig_gap(const Token *t) {
    if (!t) return 1.0f;
    uint32_t rm = t->recent_misses, ht = token_hits_total(t);
    return (float)rm / (float)(rm + ht + 1);
}
static inline float sig_conflict(const Token *t, Token *const *ctx, size_t n_ctx, float energy) {
    if (!t || t->count == 0) return 0.0f;
    size_t reach = token_reach(t, energy);
    uint32_t best = 0, second = 0;
    size_t n_match = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &token_assocs_ro(t)[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        if (e->mem_state >= MEM_DORMANT) continue;
        uint32_t w = e->hits + 1; n_match++;
        if (w > best) { second = best; best = w; }
        else if (w > second) second = w;
    }
    if (n_match < 2 || best == 0) return 0.0f;
    float ratio = (float)second / (float)best;
    float many = n_match > 5 ? 1.0f : (float)n_match / 5.0f;
    return ratio * many;
}
static inline float sig_low_activation(const Token *t, Token *const *ctx, size_t n_ctx, float energy) {
    if (!t || t->count == 0) return 1.0f;
    size_t reach = token_reach(t, energy);
    uint32_t best = 0;
    int found = 0;
    for (size_t i = 0; i < reach; ++i) {
        const AssocEntry *e = &token_assocs_ro(t)[i];
        if (!entry_matches(e, ctx, n_ctx)) continue;
        if (e->mem_state >= MEM_DORMANT) continue;
        found = 1;
        if (e->hits > best) best = e->hits;
    }
    if (!found) return 1.0f;
    return 1.0f / (1.0f + (float)best);
}
static inline float sig_effort(size_t gen_steps, size_t max_steps) {
    if (max_steps == 0) return 0.0f;
    float r = (float)gen_steps / (float)max_steps;
    return r > 1.0f ? 1.0f : r;
}
static inline float sig_novelty(AI *ai, char **words, size_t n) {
    if (!ai || !words || n == 0) return 0.0f;
    size_t novel = 0;
    for (size_t i = 0; i < n; ++i) {
        Token *t = pool_token_intern(ai->pool, words[i]);
        if (!t) continue;
        if (token_hits_total(t) == 0 && t->misses == 0 &&
            t->mem_state == MEM_ACTIVE && t->count == 0) novel++;
    }
    return (float)novel / (float)n;
}
static inline float ai_confidence(AI *ai, Token *t, Token *const *ctx, size_t n_ctx,
                                  float energy, size_t gen_steps, size_t max_gen,
                                  char **input_words, size_t n_words) {
    if (!ai) return 0.0f;
    float g = sig_gap(t);
    float c = sig_conflict(t, ctx, n_ctx, energy);
    float a = sig_low_activation(t, ctx, n_ctx, energy);
    float e = sig_effort(gen_steps, max_gen);
    float nv = sig_novelty(ai, input_words, n_words);
    const UncertainConfig *cfg = &ai->uncertain;
    float dis = cfg->w_gap * g + cfg->w_conflict * c +
                cfg->w_activation * a + cfg->w_effort * e + cfg->w_novelty * nv;
    float conf = 1.0f - dis;
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 1.0f) conf = 1.0f;
    return conf;
}

/* ============================ 元认知 + 表达 ============================ */

static inline float ai_familiarity(const AI *ai, char **words, size_t n) {
    if (!ai || !words || n == 0) return 0.0f;
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        Token *t = pool_token_intern(ai->pool, words[i]);
        if (t) total += token_hits_total(t);
    }
    return 1.0f - 1.0f / (1.0f + (float)total / (float)n);
}
static inline float ai_fluency(size_t steps, size_t max_steps) {
    if (max_steps == 0) return 0.0f;
    float r = (float)steps / (float)max_steps;
    return 1.0f - (r > 1.0f ? 1.0f : r);
}
static inline MetaCog ai_metacog(AI *ai, Token *seed, float confidence,
                                 size_t gen_steps, size_t max_gen,
                                 char **input_words, size_t n_words) {
    MetaCog m = {0};
    m.confidence = confidence;
    m.familiarity = ai_familiarity(ai, input_words, n_words);
    m.fluency = ai_fluency(gen_steps, max_gen);
    m.effort = 1.0f - m.fluency;
    m.gap_feel = seed ? sig_gap(seed) : 1.0f;
    m.total = 0.40f * m.confidence + 0.20f * m.familiarity +
              0.15f * m.fluency + 0.10f * (1.0f - m.effort) +
              0.15f * (1.0f - m.gap_feel);
    if (m.total > 1.0f) m.total = 1.0f;
    if (m.total < 0.0f) m.total = 0.0f;
    return m;
}
static inline ExprMode ai_pick_expr_mode(const AI *ai, const MetaCog *m, float novelty) {
    (void)ai;
    /* 只有「真的不确定」才加口头前缀（大概/可能/我不太确定…）。
     * 原来的分档太靠上，正常一问一答也会被套上「应该，」「感觉，」，
     * 读起来像噪声；这里把 HEDGE 归到不加前缀，TENTATIVE 以下才开口。 */
    if (novelty > 0.6f && m->total < 0.45f) return EXPR_CURIOUS;
    if (m->total >= 0.50f) return EXPR_CONFIDENT;
    if (m->total >= 0.35f) return EXPR_HEDGE;
    if (m->total >= 0.22f) return EXPR_TENTATIVE;
    if (m->total >= 0.12f) return EXPR_UNCERTAIN;
    return EXPR_LOST;
}
static inline const char *ai_render_expr(AI *ai, ExprMode mode) {
    if (mode == EXPR_CONFIDENT || mode == EXPR_SILENT) return NULL;
    Token *root = ai->expr_root[mode];
    if (!root || root->count == 0) return NULL;
    Token *pick = token_associate_weighted(root, NULL, 0, 1.0f, &ai->rng);
    if (!pick) return NULL;
    for (size_t i = 0; i < root->count; ++i) {
        if (token_assocs(root)[i].main == pick) {
            token_touch_entry(root, &token_assocs(root)[i]);
            break;
        }
    }
    return pick->name;
}
static inline int ai_feedback_hint(const char *input) {
    if (!input) return 0;
    if (strstr(input, "不对") || strstr(input, "不是") || strstr(input, "错了") ||
        strstr(input, "说错") || strstr(input, "no") || strstr(input, "wrong")) return -1;
    if (strstr(input, "对") || strstr(input, "是的") || strstr(input, "没错") ||
        strstr(input, "正确") || strstr(input, "yes") || strstr(input, "right")) return 1;
    return 0;
}
static inline void ai_learn_expression(AI *ai, ExprMode mode, int feedback) {
    if (!ai || feedback == 0) return;
    Token *root = ai->expr_root[mode];
    if (!root) return;
    for (size_t i = 0; i < ai->focus_n; ++i) {
        Token *ctx = ai->focus_ring[i];
        if (!ctx || ctx->is_garbage) continue;
        if (feedback > 0) {
            token_add_assoc_unique(ctx, root);
            for (size_t k = 0; k < ctx->count; ++k)
                if (token_assocs(ctx)[k].main == root) {
                    token_touch_entry(ctx, &token_assocs(ctx)[k]);
                    break;
                }
        } else {
            ai_weaken_edge(ctx, root);
        }
    }
    ai->n_expr_learned++;
}

/* ============================ 生成 ============================ */

/* 这个符号节点是不是「表达池」用的脚手架（[表达:*] 及其变体 应该/大概/…）？
 * 它们是给 ai_render_expr 拼前后缀用的，不该被当成内容词再吐出来 ——
 * 否则输出会变成「大概，猫人知道地上来了再见」这种带前缀残留的句子。 */
static inline int ai_is_expr_scaffold(const AI *ai, const Token *t) {
    if (!ai || !t) return 0;
    for (int i = 0; i < EXPR_COUNT; ++i) {
        Token *root = ai->expr_root[i];
        if (!root) continue;
        if (t == root) return 1;
        for (size_t j = 0; j < root->count; ++j)
            if (token_assocs(root)[j].main == t) return 1;
    }
    return 0;
}

/* 分隔符：语料里「输入 <分割> 回复」的边界。
 * 必须让它被走到 —— 从输入尾词出去权重最大的往往就是它，若当成死胡同就永远
 * 接不上回复。它不参与文本输出，但参与游走。 */
static inline int ai_is_dialog_split(const Token *t) {
    return t && t->name && strcmp(t->name, AI_DIALOG_SPLIT_NAME) == 0;
}

/* 回复结束标记：语料里每条回复后面都跟着它，所以「回复的最后一词 → 它」是一条
 * 真实学到的边。生成时走到它就收句 —— 这就是句长的来源，不用瞎猜步数。 */
static inline int ai_is_reply_end(const Token *t) {
    return t && t->name && strcmp(t->name, AI_REPLY_END_NAME) == 0;
}

/* 这条边能不能走（结构性的过滤，和角色无关） */
static inline int ai_edge_walkable(const AI *ai, const AssocEntry *e, float energy) {
    if (edge_is_causal_guess(e)) return 0;   /* 因果猜想不参与走边（v10：判据在动作槽上） */
    if (e->mem_state >= MEM_DORMANT) return 0;
    if (e->mem_state == MEM_FADING && energy < 0.5f) return 0;
    /* hits==0 的边是「建了但没在任何语料上下文里被强化过」的残留
     * （比如解释器建的原语边、观察建了却没打角色的边）。它们没有语义依据，
     * 却带着权重 1 参与抽签，会把游走带偏。只放行两个边界标记。 */
    if (e->hits == 0 && !ai_is_reply_end(e->main) && !ai_is_dialog_split(e->main))
        return 0;
    if (e->main && e->main->name && e->main->name[0] == '[' &&
        !ai_is_dialog_split(e->main) && !ai_is_reply_end(e->main))
        return 0;                       /* 跳过原语，只放行两个边界标记 */
    if (ai_is_expr_scaffold(ai, e->main)) return 0;
    return 1;
}

/* 到了 t 之后还能继续走吗？
 * 不能走的有两类：① 没有任何可用出边（比如 [原语:分割]/[原语:连接] 这种
 * 只出现在行尾的标记）；② 只有原语/脚手架出边。
 * 走到这种词上就等于句子被硬截断 —— 实测「你好 → [原语:分割] → 死」就是它。
 * [对话:结束] 不算死胡同：它是合法的收句点。 */
/* 到了 t 之后还能继续走吗？from 是我们出发的那个词 ——
 * t == from 的自环不算出路（走一步就撞环检测，等于句子被截断）。 */
static inline int ai_can_continue_from(AI *ai, const Token *from, const Token *t, float energy) {
    if (!t) return 0;
    if (t == from) return 0;               /* 自环不算出路 */
    if (ai_is_reply_end(t)) return 1;      /* 合法的收句点 */
    for (size_t i = 0; i < t->count; ++i)
        if (ai_edge_walkable(ai, &token_assocs_ro(t)[i], energy)) return 1;
    /* 没有任何可用出边的词一律算死胡同 —— 包括 [原语:分割]。
     * 语料里所有 [原语:分割] 都在行尾、后面什么都没有（320 行全如此），
     * 走进去必然断句；必须靠「不能续」把它挡在外面，句长才能靠
     * [对话:结束] 和角色边自然决定。 */
    return 0;
}

/* 带角色的选边。
 *
 * want = ASSOC_ROLE_REPLY 时，**只**在标了「回复」角色的边里选 —— 这就是「按我建议的干」
 * 的那一步：从回复首词出去时，只能走同样是回复内部学到的转移，语料别处的高频共现
 * （反义/什么/属 于…）进不来。没有该角色的边时才退化成通用边，保证不会走投无路。 */
/* 这个符号节点出现在某个话题里吗（投票表） */
static inline int ai_topic_has(const AI *ai, uint8_t topic, const Token *t) {
    if (!ai || !t || topic >= AI_MAX_TOPICS) return 0;
    const AITopic *T = &ai->topics[topic];
    for (size_t i = 0; i < T->n; ++i)
        if (T->seen[i] == t) return 1;
    return 0;
}

/* 用输入词给话题投票，返回票数最高的话题（都 0 票就返回 ASSOC_TOPIC_NONE） */
static inline uint8_t ai_detect_topic_words(const AI *ai, Token *const *words, size_t n) {
    if (!ai || !words || n == 0) return ASSOC_TOPIC_NONE;
    size_t best = 0; uint32_t best_v = 0;
    for (uint8_t t = 1; t < AI_MAX_TOPICS; ++t) {
        if (ai->topics[t].n == 0) continue;
        uint32_t v = 0;
        for (size_t i = 0; i < n; ++i)
            if (ai_topic_has(ai, t, words[i])) v++;
        if (v > best_v) { best_v = v; best = t; }
    }
    return best_v ? (uint8_t)best : ASSOC_TOPIC_NONE;
}

/* 话题投票：把语料里 [话题:N] 块内的内容词记进话题表 */
static inline void ai_topic_vote(AI *ai, uint8_t topic, Token *t) {
    if (!ai || !t || topic == ASSOC_TOPIC_NONE || topic >= AI_MAX_TOPICS) return;
    AITopic *T = &ai->topics[topic];
    for (size_t i = 0; i < T->n; ++i)
        if (T->seen[i] == t) return;
    if (T->n < sizeof T->seen / sizeof T->seen[0]) T->seen[T->n++] = t;
}

/* ---------------- 整句检索 ---------------- */

static inline void ai_sent_add(AI *ai, Token *const *in, size_t in_n,
                               Token *const *out, size_t out_n, uint8_t topic) {
    if (!ai || !in || !out || in_n == 0 || out_n == 0) return;
    if (in_n > AI_MAX_SENT) in_n = AI_MAX_SENT;
    if (out_n > AI_MAX_SENT) out_n = AI_MAX_SENT;
    if (ai->pairs_n == ai->pairs_cap) {
        size_t nc = ai->pairs_cap ? ai->pairs_cap * 2 : 256;
        AISentPair *np = (AISentPair *)realloc(ai->pairs, nc * sizeof *np);
        if (!np) return;
        ai->pairs = np; ai->pairs_cap = nc;
    }
    AISentPair *p = &ai->pairs[ai->pairs_n++];
    memcpy(p->in, in, in_n * sizeof *in);   p->in_n = in_n;
    memcpy(p->out, out, out_n * sizeof *out); p->out_n = out_n;
    p->topic = topic;
}

/* 一个词在句子里的「上下文签名」：左边最多 4 个邻居 + 右边最多 4 个邻居。
 * 同一签名 = 占据相同的句法位置 = 可以互换。 */
static inline uint32_t ai_slot_sig(Token *const *seq, size_t n, size_t i) {
    /* 只用**紧邻**的左右邻居 + 是否在句尾。
     * 之前用了左右各 4 个邻居，签名太细，1 万多个签名塞进 128 个类，
     * 结果几乎每个类只有一个成员 —— 「同位」永远不成立，合成从不触发（实测 1/50）。
     * 紧邻就够了：`我 喜欢 猫` 和 `我 喜欢 狗` 里 猫/狗 的左邻都是 喜欢、
     * 右邻都是句尾，归为一类；而 `猫 吃 鱼` 里的 猫 左邻是句首、右邻是 吃，另一类。 */
    uint32_t h = 2166136261u;
    h ^= (i > 0 && seq[i-1]) ? seq[i-1]->id : 0xFFFFFFFFu;
    h *= 16777619u;
    h ^= 0x5BF03635u; h *= 16777619u;            /* 分隔左右 */
    int at_end = (i + 1 == n);
    h ^= at_end ? 0xFEEDFACEu
                : (seq[i+1] ? seq[i+1]->id : 0xFFFFFFFFu);
    h *= 16777619u;
    return h;
}

/* 从整句库归纳同位槽类。只在「输入句」上做 —— 我们靠它决定
 * 「用户换了个词，回复里对应的那个词也该换」。 */
static inline void ai_slots_build(AI *ai) {
    if (!ai) return;
    ai->slots_n = 0;
    if (!ai->pairs || ai->pairs_n == 0) return;
    /* §6.207 **sig → 槽下标**的开放寻址表（只在建表期间用，栈上 8 KB）：
     * 原来每来一条候选都要线性扫已建好的槽类（槽类满 1024 时 = 每次 1024 次比较，
     * 库句到十万级就是十亿次）。表里只放"槽下标+1"，空槽 = 0 ——
     * **判等还是回读 `ai->slots[idx].sig`**，不另存一份 sig（不会有第二份真相）。
     * 语义与原来完全一致：同一个 sig 只有一个槽，插入顺序不变 ⇒ 槽下标不变。 */
    enum { SLOT_HASH_SZ = 2048 };         /* 2×AI_MAX_SLOTS：装载因子 ≤ 0.5 */
    uint32_t sh[SLOT_HASH_SZ];
    memset(sh, 0, sizeof sh);
    for (size_t i = 0; i < ai->pairs_n; ++i) {
        const AISentPair *p = &ai->pairs[i];
        if (p->in_n < 2) continue;
        for (size_t k = 0; k < p->in_n; ++k) {
            Token *t = p->in[k];
            if (!t || !t->name) continue;
            if (t->name[0] == '[') continue;              /* 跳过标记词 */
            if (t->name[0] && strlen(t->name) > 6) continue;
            uint32_t sg = ai_slot_sig(p->in, p->in_n, k);
            /* 找已有同类（哈希探测；命中判据是"槽里存的 sig 相等"） */
            size_t at = ai->slots_n;
            size_t h = (size_t)(sg * 2654435761u) & (SLOT_HASH_SZ - 1);
            while (sh[h]) {
                size_t cand = (size_t)sh[h] - 1;
                if (cand < ai->slots_n && ai->slots[cand].sig == sg) { at = cand; break; }
                h = (h + 1) & (SLOT_HASH_SZ - 1);
            }
            if (at == ai->slots_n) {
                if (ai->slots_n >= AI_MAX_SLOTS) continue;
                ai->slots[at].n = 0;
                ai->slots[at].sig = sg;
                ai->slots_n++;
                sh[h] = (uint32_t)(at + 1);
            }
            AISlot *sl = &ai->slots[at];
            int dup = 0;
            for (size_t q = 0; q < sl->n; ++q)
                if (sl->members[q] == t) { dup = 1; break; }
            if (!dup && sl->n < 48) sl->members[sl->n++] = t;
        }
    }
}

/* a 和 b 是同一槽类吗（可以互换）？ */
static inline int ai_same_slot(const AI *ai, const Token *a, const Token *b) {
    if (!ai || !a || !b) return 0;
    if (a == b) return 1;
    for (size_t q = 0; q < ai->slots_n; ++q) {
        const AISlot *sl = &ai->slots[q];
        int ha = 0, hb = 0;
        for (size_t k = 0; k < sl->n; ++k) {
            if (sl->members[k] == a) ha = 1;
            if (sl->members[k] == b) hb = 1;
        }
        if (ha && hb) return 1;
    }
    return 0;
}

/* 从整句库统计 IDF：某个词出现在越多的「输入句」里，权重越低。
 * 这是换匹配器的关键 —— 纯 Dice 里 你/我/吗 和 帮/想 一个分量，
 * 才会出现「你能帮我个忙吗」串到「你会想我吗」。 */
/* ============================ 分布式表示（emb） ============================
 *
 * 这是「神经」那一半：把符号图里的共现统计投影成每个词一个 1024 维稠密向量，
 * 让**语义相近的词向量真的靠近**。
 *
 *   emb(t) = Σ_c PPMI(t,c) · r_c        （r_c = 由 c 的稳定 id 生成的 ±1 随机向量）
 *
 * 外加一项反向贡献：c 也受「把它当上下文的词」影响。这一项对**稀疏词**很关键 ——
 * 有些词出边只有一两条（比如 `危险`），但入边不少；只算正向的话 emb 几乎是空的。
 *
 * 为什么不做「多轮邻居聚合」：那种做法迭代几轮后所有向量都会趋同
 * （oversmoothing），相似度全都变高，等于什么都没说。随机索引投影是**一遍**的，
 * 且保留 PPMI 空间的内积，不会塌。
 *
 * 成本 O(E × EMB_DIM)：实测 model.bin（35858 条边）约 0.2 秒，
 * 只在「训练收尾」和「读盘之后」各跑一次。
 *
 * emb **不写进模型文件**：它是 (边 + hits + 稳定 id) 的确定性函数，
 * 现算即可 —— 格式保持 v5 不变。 */
static inline void ai_embed_build(AI *ai) {
    if (!ai || !ai->pool) return;
    TokenPool *p = ai->pool;
    size_t n = p->count;
    if (n == 0) return;

    int32_t *acc = (int32_t *)calloc(n * EMB_DIM, sizeof(int32_t));
    int64_t *marg = (int64_t *)calloc(n, sizeof(int64_t));
    int8_t  *rv = (int8_t *)malloc(EMB_DIM);
    if (!acc || !marg || !rv) { free(acc); free(marg); free(rv); return; }

    /* ⓪ df：一个上下文被多少个**不同的**词指向。
     * 这一步是整个嵌入能不能用的关键。实测不做过的时候，猫/狗/雨/鱼 的
     * PPMI 最高上下文**全是同一批模板话**：
     *
     *   猫: 是什么(7.75) 在哪里(7.06) 是谁(7.06) 软(5.96) ...
     *   狗: 是什么(8.45) 在哪里(7.75) 是谁(7.35) 暗(6.25) ...
     *   雨: 是什么(7.83) 声(7.20)   在哪里(7.14) 是谁(7.14) ...
     *
     * 「是什么/在哪里/是谁」只跟名词搭配，所以单看 PMI 它们比任何实词都高；
     * 可它们跟**每一个**名词都搭配 —— 信息量为零，却把所有名词的向量拉到一起。
     * 结果就是 猫~雨 79.9% 比 猫~狗 62.2% 还高（等于分不出来）。
     *
     * 修法：df 超过内容词总数 15% 的上下文一律不当上下文用。
     * 这是**数据驱动**的，不是手写停用词表。 */
    size_t n_content = 0;
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        if (t->name && t->name[0] != '[') n_content++;
    }
    uint32_t *df = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!df) { free(acc); free(marg); free(rv); return; }
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        if (!t->name || t->name[0] == '[') continue;
        for (size_t j = 0; j < t->count; ++j) {
            Token *m = token_assocs(t)[j].main;
            if (!m || !m->name || m->name[0] == '[') continue;
            if (token_assocs(t)[j].hits) df[m->id]++;
        }
    }
    const uint32_t DF_MAX = (uint32_t)(n_content * 15 / 100);
    #define EMB_CTX_OK(m) ((m)->name && (m)->name[0] != '[' && df[(m)->id] <= DF_MAX)

    /* ① 行和：每个内容符号节点的出边权重之和 */
    int64_t tot = 0;
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        if (!t->name || t->name[0] == '[') continue;
        for (size_t j = 0; j < t->count; ++j) {
            Token *m = token_assocs(t)[j].main;
            if (!m || !EMB_CTX_OK(m)) continue;
            uint32_t h = token_assocs(t)[j].hits;
            if (h == 0) continue;
            marg[i] += h;
            tot += h;
        }
    }
    if (tot == 0) { free(acc); free(marg); free(rv); free(df); return; }

    /* ② 两遍累积：正向（t 的上下文）+ 反向（谁把 t 当上下文）
     *    PPMI = max(0, log( p(c|t) / p(c) )) */
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        if (!t->name || t->name[0] == '[' || marg[i] == 0) continue;
        for (size_t j = 0; j < t->count; ++j) {
            Token *m = token_assocs(t)[j].main;
            if (!m || !EMB_CTX_OK(m)) continue;
            uint32_t h = token_assocs(t)[j].hits;
            if (h == 0) continue;
            size_t mi = m->id;
            if (mi >= n) continue;
            if (marg[mi] == 0) continue;      /* 目标也没有可用的上下文 */
            double p_c_given_t = (double)h / (double)marg[i];
            double p_c = (double)marg[mi] / (double)tot;
            if (p_c > 0) {
                double pmi = log(p_c_given_t / p_c);
                if (pmi > 0) {
                    if (pmi > 10.0) pmi = 10.0;
                    emb_randvec(m->id, rv);
                    int32_t *a = acc + i * EMB_DIM;
                    int32_t w = (int32_t)(pmi * 1024.0);
                    for (size_t d = 0; d < EMB_DIM; ++d) a[d] += w * rv[d];
                }
            }
            /* 反向：m 的向量里加上 t 的随机向量（稀疏词的救命稻草） */
            if (marg[mi] > 0) {
                double p_t_given_c = (double)h / (double)marg[mi];
                double p_t = (double)marg[i] / (double)tot;
                if (p_t > 0) {
                    double pmi2 = log(p_t_given_c / p_t);
                    if (pmi2 > 0) {
                        if (pmi2 > 10.0) pmi2 = 10.0;
                        emb_randvec(t->id, rv);
                        int32_t *a = acc + mi * EMB_DIM;
                        int32_t w = (int32_t)(pmi2 * 1024.0);
                        for (size_t d = 0; d < EMB_DIM; ++d) a[d] += w * rv[d];
                    }
                }
            }
        }
    }

    #undef EMB_CTX_OK

    /* ③ all-but-the-top：去均值 + 去掉前 3 个主成分。
     *
     * 这一步不是可选的。实测不做的时候：`猫` 的最近邻居是 雨 82.9% / 椅子 80.7%，
     * 而 鱼 与 狗 的余弦（77.3%）比 猫 与 狗（67.2%）还高 —— 也就是高频名词
     * （雨/山/太阳/鱼，它们和几乎所有词都共现过）把「共现向量」的公共方向
     * 撑成了第一主成分，于是所有常见名词彼此都"像"，向量里剩下的全是频率信息。
     *
     * 去掉公共方向之后，留下的是「这个词**特别**的上下文」。这是 embedding
     * 后处理的常规做法（all-but-the-top），这里用幂迭代取前 3 个方向，
     * 固定初值、固定轮数 → 完全确定性。 */
    float *E = (float *)calloc(n * EMB_DIM, sizeof(float));
    float *mu = (float *)calloc(EMB_DIM, sizeof(float));
    float *v = (float *)malloc(EMB_DIM * sizeof(float));
    float *u = (float *)malloc(n * sizeof(float));
    if (!E || !mu || !v || !u) {
        /* 内存不够就退回「不做后处理」的老路，至少还有向量 */
        for (size_t i = 0; i < n; ++i) {
            Token *t = p->tokens[i];
            if (!t->emb) continue;
            { void *own = aip_buf_own(t->emb, EMB_DIM); if (!own) continue; t->emb = (int8_t *)own; }
            int32_t *a = acc + i * EMB_DIM;
            int32_t mx = 0;
            for (size_t d = 0; d < EMB_DIM; ++d) { int32_t q2 = a[d] < 0 ? -a[d] : a[d]; if (q2 > mx) mx = q2; }
            if (mx == 0) { memset(t->emb, 0, EMB_DIM); continue; }
            for (size_t d = 0; d < EMB_DIM; ++d) {
                int32_t q2 = (int32_t)(((int64_t)a[d] * 120) / mx);
                if (q2 > 127) q2 = 127;
                if (q2 < -128) q2 = -128;
                t->emb[d] = (int8_t)q2;
            }
        }
        free(acc); free(marg); free(rv); free(E); free(mu); free(v); free(u); free(df);
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t *a = acc + i * EMB_DIM;
        int32_t mx = 0;
        for (size_t d = 0; d < EMB_DIM; ++d) { int32_t q2 = a[d] < 0 ? -a[d] : a[d]; if (q2 > mx) mx = q2; }
        if (mx == 0) continue;                       /* 空向量不参与统计 */
        float *e = E + i * EMB_DIM;
        for (size_t d = 0; d < EMB_DIM; ++d) e[d] = (float)a[d] / (float)mx;
        used++;
        for (size_t d = 0; d < EMB_DIM; ++d) mu[d] += e[d];
    }
    if (used == 0) {
        free(acc); free(marg); free(rv); free(E); free(mu); free(v); free(u); free(df);
        return;
    }
    for (size_t d = 0; d < EMB_DIM; ++d) mu[d] /= (float)used;
    for (size_t i = 0; i < n; ++i) {
        int32_t *a = acc + i * EMB_DIM;
        int32_t mx = 0;
        for (size_t d = 0; d < EMB_DIM; ++d) { int32_t q2 = a[d] < 0 ? -a[d] : a[d]; if (q2 > mx) mx = q2; }
        if (mx == 0) continue;
        float *e = E + i * EMB_DIM;
        for (size_t d = 0; d < EMB_DIM; ++d) e[d] -= mu[d];
    }
    /* 前 3 个主成分：幂迭代 + 逐次 deflate */
    {
        double inv = 1.0 / sqrt((double)EMB_DIM);
        for (int comp = 0; comp < 3; ++comp) {
            for (size_t d = 0; d < EMB_DIM; ++d) v[d] = (float)inv;   /* 固定初值 */
            for (int it = 0; it < 8; ++it) {
                for (size_t i = 0; i < n; ++i) {
                    float *e = E + i * EMB_DIM;
                    float acc2 = 0;
                    for (size_t d = 0; d < EMB_DIM; ++d) acc2 += e[d] * v[d];
                    u[i] = acc2;
                }
                double nu = 0;
                for (size_t i = 0; i < n; ++i) nu += (double)u[i] * (double)u[i];
                if (nu <= 0) break;
                nu = sqrt(nu);
                for (size_t i = 0; i < n; ++i) u[i] = (float)(u[i] / nu);
                for (size_t d = 0; d < EMB_DIM; ++d) v[d] = 0;
                for (size_t i = 0; i < n; ++i) {
                    float *e = E + i * EMB_DIM;
                    float ui = u[i];
                    if (ui == 0) continue;
                    for (size_t d = 0; d < EMB_DIM; ++d) v[d] += e[d] * ui;
                }
                double nv = 0;
                for (size_t d = 0; d < EMB_DIM; ++d) nv += (double)v[d] * (double)v[d];
                if (nv <= 0) break;
                nv = sqrt(nv);
                for (size_t d = 0; d < EMB_DIM; ++d) v[d] = (float)(v[d] / nv);
            }
            /* 从每个向量里减掉在主方向上的投影 */
            for (size_t i = 0; i < n; ++i) {
                float *e = E + i * EMB_DIM;
                /* 只对有向量的词做：空向量的 E 行是 0，投影也是 0，不影响 */
                float p2 = 0;
                for (size_t d = 0; d < EMB_DIM; ++d) p2 += e[d] * v[d];
                if (p2 == 0) continue;
                for (size_t d = 0; d < EMB_DIM; ++d) e[d] -= p2 * v[d];
            }
        }
    }
    /* ④ 量化进 int8：每个词按自己的峰值归一（余弦只看方向） */
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        if (!t->emb) continue;
        /* §6.581 写前复制：emb 可能是**和别的脑共享**的（`pool_share_emb`）⇒ 写之前先要一份自己的。
         * 不复制就会把兄弟脑的向量一起改掉（内存看着没问题，答案悄悄变）。 */
        { void *own = aip_buf_own(t->emb, EMB_DIM); if (!own) continue; t->emb = (int8_t *)own; }
        int32_t *a = acc + i * EMB_DIM;
        int32_t mx = 0;
        for (size_t d = 0; d < EMB_DIM; ++d) { int32_t q2 = a[d] < 0 ? -a[d] : a[d]; if (q2 > mx) mx = q2; }
        if (mx == 0) { memset(t->emb, 0, EMB_DIM); continue; }
        float *e = E + i * EMB_DIM;
        float p3 = 0;
        for (size_t d = 0; d < EMB_DIM; ++d) { float q3 = e[d] < 0 ? -e[d] : e[d]; if (q3 > p3) p3 = q3; }
        if (p3 <= 0) { memset(t->emb, 0, EMB_DIM); continue; }
        for (size_t d = 0; d < EMB_DIM; ++d) {
            int32_t q4 = (int32_t)((e[d] / p3) * 120.0f);
            if (q4 > 127) q4 = 127;
            if (q4 < -128) q4 = -128;
            t->emb[d] = (int8_t)q4;
        }
    }
    free(acc); free(marg); free(rv); free(E); free(mu); free(v); free(u); free(df);
}

static inline void ai_idf_rebuild(AI *ai) {
    if (!ai || !ai->pool) return;
    size_t ntok = ai->pool->count;
    if (!ntok) return;
    if (!ai->idf) {
        ai->idf = (float *)malloc(ntok * sizeof *ai->idf);
        if (!ai->idf) { ai->idf_n = 0; return; }
    } else {
        float *nidf = (float *)realloc(ai->idf, ntok * sizeof *ai->idf);
        if (!nidf) { ai->idf_n = 0; return; }
        ai->idf = nidf;
    }
    ai->idf_n = ntok;          /* 先认长度：就算下面 df 分配失败，边界也是对的 */
    for (size_t i = 0; i < ntok; ++i) ai->idf[i] = 0.0f;
    uint32_t *df = (uint32_t *)calloc(ntok, sizeof *df);
    if (!df) return;
    size_t ndoc = ai->pairs_n;
    for (size_t i = 0; i < ndoc; ++i) {
        const AISentPair *p = &ai->pairs[i];
        for (size_t k = 0; k < p->in_n; ++k) {
            Token *t = p->in[k];
            if (t && t->id < ntok) df[t->id]++;
        }
    }
    for (size_t i = 0; i < ntok; ++i) {
        float v = (df[i] == 0)
                ? 2.5f                                   /* 只在回复里出现 → 最有区分度 */
                : 1.0f + logf((float)(ndoc + 1) / (float)(df[i] + 1));
        if (v < 0.25f) v = 0.25f;
        if (v > 4.0f) v = 4.0f;
        ai->idf[i] = v;
    }
    free(df);
}

static inline float ai_idf_of(const AI *ai, const Token *t) {
    /* 边界用 idf_n（数组真实长度），不是 pool->count（当前词数）。
     * 重建 idf 之后还会造词，用当前词数当边界就会越界读。 */
    if (!ai || !ai->idf || !t || t->id >= ai->idf_n) return 1.0f;
    float v = ai->idf[t->id];
    return v > 0.0f ? v : 1.0f;
}

/* 把符号节点名的 UTF-8 拆成「字符」，喂给 bigram 用 */
static inline size_t ai_name_chars(const char *name, const char **out, uint8_t *lens,
                                   size_t max) {
    if (!name) return 0;
    const unsigned char *p = (const unsigned char *)name;
    size_t n = 0;
    while (*p && n < max) {
        size_t l = 1;
        if ((*p & 0xE0) == 0xC0) l = 2;
        else if ((*p & 0xF0) == 0xE0) l = 3;
        else if ((*p & 0xF8) == 0xF0) l = 4;
        /* 别越过字符串结尾：否则会拿越界的「长度」去 memcmp */
        for (size_t k = 1; k < l; ++k)
            if (!p[k]) { l = k; break; }
        out[n] = (const char *)p;
        lens[n] = (uint8_t)l;
        n++;
        p += l;
    }
    return n;
}

/* **带缓存的字符切分**（§6.127）：token.h 的 `Token.ch_p/ch_len/n_ch` 是对
 * `name` 的**纯派生**缓存（名字 intern 后不再变）。这里不重切、只按名字指针读回。
 * 超长名字（n_ch==255 哨兵）与"没名字"照旧现场切 —— 与旧行为**逐字节相同**，
 * 只是把重复的字节遍历去掉。这是 `ai_char_bigram_score` 里最贵的一步：
 * 那一项占全轮 76% 的 CPU，而每次调用都要把两边名字重切一遍。 */
static inline size_t token_name_chars(const Token *t, const char **out, uint8_t *lens,
                                      size_t max) {
    if (!t || !t->name) return 0;
    if (t->n_ch == 0) {                       /* 第一次用：切一遍存下来 */
        Token *m = (Token *)t;
        size_t n = ai_name_chars(t->name, m->ch_p, m->ch_len, TOKEN_CH_MAX);
        uint32_t sig = 0;                     /* 顺带把这名字的**字符集位图**算出来 */
        for (size_t i = 0; i < n; ++i) {
            uint32_t h = 2166136261u;
            for (uint8_t k = 0; k < m->ch_len[i]; ++k) {
                h ^= (uint8_t)m->ch_p[i][k]; h *= 16777619u;
            }
            sig |= (uint32_t)1u << (h & 31u);
        }
        m->ch_sig = sig;
        m->n_ch = (n >= TOKEN_CH_MAX) ? 255u : (uint8_t)n;   /* 满了 ⇒ 哨兵：以后不再缓存 */
    }
    if (t->n_ch != 255u && t->n_ch <= max) {
        for (uint8_t i = 0; i < t->n_ch; ++i) { out[i] = t->ch_p[i]; lens[i] = t->ch_len[i]; }
        return t->n_ch;
    }
    return ai_name_chars(t->name, out, lens, max);           /* 超长：现场切（旧路径） */
}
/* 名字的**字符集位图**（跟着上面的缓存一起算，之后是 O(1) 读）。
 * 用途见 ai_char_bigram_score 的预筛：位图不相交 ⇒ 字符 bigram 必然不相交。
 * ⚠ 只能是**超集**（宁可多置位）：少置一位就会把"其实有重合"错判成 0，那才是改分数。 */
static inline uint32_t token_name_sig(const Token *t) {
    if (!t || !t->name) return 0;
    if (t->n_ch == 0) {
        const char *o[TOKEN_CH_MAX]; uint8_t l[TOKEN_CH_MAX];
        (void)token_name_chars(t, o, l, TOKEN_CH_MAX);
    }
    if (t->n_ch != 255u) return t->ch_sig;
    uint32_t s = 0;                           /* 超长名字（没缓存）：现场算，位图仍是超集 */
    const char *o[96]; uint8_t l[96];
    size_t n = ai_name_chars(t->name, o, l, 96);
    for (size_t i = 0; i < n; ++i) {
        uint32_t h = 2166136261u;
        for (uint8_t k = 0; k < l[i]; ++k) { h ^= (uint8_t)o[i][k]; h *= 16777619u; }
        s |= (uint32_t)1u << (h & 31u);
    }
    return s;
}

/* 一个字符是否相同。**不用 memcmp** ：长度是变量、gcc 内联不了 ⇒ 每次都是一次
 * 真正的函数调用，而这里的内层循环每轮跑几百万次（§6.127 量出来的头号热点）。
 * 1~4 字节（UTF-8 的全部字符宽度）走内联比较，超长才落到 memcmp。 */
static inline int ai_ch_eq(const char *x, const char *y, uint8_t l) {
    if (l == 3) return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];   /* 汉字：最常见 */
    if (l == 1) return x[0] == y[0];
    if (l == 2) return x[0] == y[0] && x[1] == y[1];
    if (l == 4) return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
    return memcmp(x, y, l) == 0;
}

/* **查询侧的字符切分备忘**（§6.127）：检索是"一个查询 × 几千条库句"，
 * 每一对都重新切一遍**同一句查询**的字符（`_t/roundbench_last.tsv`：每轮 1.3 万次调用）。
 * 切一次、存下来，后面几千次直接读回。
 * 键是**内容**（符号节点指针序列）+ 脑 + 池子 —— 调用方每次传的是两块**不同的临时数组**
 * （内容相同），按指针比会一次都不命中（这一点是量出来的）；而带上 `ai`/`pool`
 * 是为了防"同一块内存被另一个脑子复用"（工具里连着建两个 AI 时真的会撞地址）。 */
/* §6.136 **字符集位图**（256 bit）：把一个字符（`ch`，长 `l`）打进位图。
 * 比 §6.127 那张 32 位的 `ch_sig` 细 8 倍 —— 实测放行率因此从 61~70% 掉到 ~17%。 */
enum { AI_CB_WORDS = 4 };        /* 256 bit */
static inline void ai_cb_set(uint64_t *bits, const char *ch, uint8_t l) {
    uint32_t h = 2166136261u;
    for (uint8_t k = 0; k < l; ++k) { h ^= (uint8_t)ch[k]; h *= 16777619u; }
    uint32_t b = h & 255u;
    bits[b >> 6] |= 1ull << (b & 63);
}
static inline void ai_cb_fill(uint64_t *bits, const char *const *c, const uint8_t *l, size_t nc) {
    for (size_t i = 0; i < nc; ++i) ai_cb_set(bits, c[i], l[i]);
}
static inline int ai_cb_any(const uint64_t *x, const uint64_t *y) {
    for (int i = 0; i < AI_CB_WORDS; ++i) if (x[i] & y[i]) return 1;
    return 0;
}

typedef struct {
    uint64_t     serial;    /* pool 的序号（不受 malloc 地址复用影响） */
    const Token *tok[64];
    size_t       n, nc;
    const char  *c[96];
    uint8_t      l[96];
    uint32_t     sig;
    uint64_t     bg64;                /* §6.210 查询侧字符 bigram 布隆位（64 bit）*/
    uint64_t     cbits[AI_CB_WORDS];  /* §6.136 查询侧 256 位字符集位图（切一次、几千次复用）*/
} AiCharQueryMemo;
/* §6.582 **每次问答的字符 bigram 备忘**：原来是一个全局结构 ⇒ 两个线程同时问
 * 会互相把"上一次的字符表"覆盖掉（TSan 实测 13 处竞争）。线程局部，单线程行为不变。 */
static AIP_TLS AiCharQueryMemo g_charq;

/* 就绪之后：ca/la_/na/sig 是这次查询的字符切分 */
static inline int ai_char_query_prep(uint64_t serial, Token *const *a, size_t an) {
    enum { MAXC = 96, MAXT = 64 };
    /* §6.193 **长输入不许越界**：`tok[]` 只有 64 格，以前只在**备忘命中**那条判断里限了
     * `an <= 64`，填充循环没有 ⇒ 一个 60 KB 的长句（两万字）直接写越界（ASan 报
     * global-buffer-overflow）。这里统一按 `lim = min(an,64)` 处理（字符集本来就只看前 64 个词）。 */
    size_t lim = an > (size_t)MAXT ? (size_t)MAXT : an;
    if (g_charq.serial == serial &&
        g_charq.n == lim && (lim == 0 || memcmp(g_charq.tok, a, lim * sizeof *a) == 0))
        return 1;
    g_charq.serial = serial; g_charq.n = lim;
    for (size_t i = 0; i < lim; ++i) g_charq.tok[i] = a[i];
    size_t nc = 0;
    uint32_t sig = 0;
    for (size_t i = 0; i < lim && nc + 8 < MAXC; ++i) {
        nc += token_name_chars(a[i], g_charq.c + nc, g_charq.l + nc, MAXC - nc);
        sig |= token_name_sig(a[i]);
    }
    g_charq.nc = nc; g_charq.sig = sig;
    {   /* §6.210 查询侧的字符 bigram 布隆位（与库句侧同一个哈希函数）*/
        uint64_t bgv = 0;
        for (size_t k = 0; k + 1 < nc; ++k) {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t q = 0; q < g_charq.l[k]; ++q) h = (h ^ (uint8_t)g_charq.c[k][q]) * 1099511628211ull;
            for (uint8_t q = 0; q < g_charq.l[k+1]; ++q) h = (h ^ (uint8_t)g_charq.c[k+1][q]) * 1099511628211ull;
            bgv |= 1ull << (h & 63);
        }
        g_charq.bg64 = bgv;
    }
    memset(g_charq.cbits, 0, sizeof g_charq.cbits);      /* §6.136：查询侧 256 位字符集位图 */
    ai_cb_fill(g_charq.cbits, g_charq.c, g_charq.l, nc);
    return 0;
}

/* `AIP_CHARBG=0` 把字符这一项整条关掉（只做性能标定，不改默认行为）。 */
static inline int ai_charbg_on(void) {
    static AIP_TLS int on = -1;
    if (on < 0) { const char *e = getenv("AIP_CHARBG"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}

/* §6.140 这一项的比较核心：两边都是**展平的字符列表**（谁摊平的无所谓）。
 * 名字叫 bigram，其实是**逐字符**：a 的每个字符（**最后一个除外**）在 b 里（**最后一个除外**）
 * 出现过就算一个 inter —— 这个口径是 §6.136 实测出来的，改它就是改分数（要单独一轮 + A/B）。 */
static inline int ai_cb_core(const char *const *ca, const uint8_t *la_, size_t na,
                             const char *const *cb, const uint8_t *lb_, size_t nb) {
    size_t inter = 0, tot_a = 0, tot_b = 0;
    for (size_t i = 0; i + 1 < na; ++i) {
        tot_a++;
        for (size_t j = 0; j + 1 < nb; ++j) {
            if (la_[i] == lb_[j] && ai_ch_eq(ca[i], cb[j], la_[i])) { inter++; break; }
        }
    }
    for (size_t j = 0; j + 1 < nb; ++j) tot_b++;
    size_t tot = tot_a + tot_b;
    return tot ? (int)((inter * 200) / tot) : 0;   /* 0..100 */
}

/* 字符 bigram 重合度：Dice。抓「怎么说」相似而词不完全一样的情况
 * （「你平常做什么」对「你平时做什么」词重叠只有一半，字符几乎全同）。 */
static inline int ai_char_bigram_score(uint64_t serial,
                                       Token *const *a, size_t an,
                                       Token *const *b, size_t bn) {
    if (!ai_charbg_on()) return 0;      /* `AIP_CHARBG=0`：整条关掉（只做标定）*/
    enum { MAXC = 96 };
    const char *cb[MAXC];
    uint8_t lb_[MAXC];
    (void)ai_char_query_prep(serial, a, an);
    const char *const *ca = g_charq.c;
    const uint8_t *la_ = g_charq.l;
    size_t na = g_charq.nc, nb = 0;
    for (size_t i = 0; i < bn && nb + 8 < MAXC; ++i)
        nb += token_name_chars(b[i], cb + nb, lb_ + nb, MAXC - nb);
    if (na < 2 || nb < 2) return 0;
    /* **字符集位图预筛**（§6.127）：这一趟是每次检索的**头号热点**
     * （`gprof`：占全部 CPU 的 44%，出厂模型下每轮 1.3 万次调用 —— 每轮把
     * 几千条库句逐条算一遍字符 bigram，`_t/roundbench_last.tsv`）。
     * 判据是**精确**的：一个字符都不共享 ⇒ 一个字符 bigram 也不可能共享 ⇒ inter=0。
     * 所以这是"提前知道结果是 0"，不是近似 —— 位图碰撞只会让预筛**放行**
     * （多算一次），永远不会改分数。省掉的是那圈 O(na×nb) 的比较。 */
    {
        uint32_t sb = 0;
        for (size_t j = 0; j < bn; ++j) sb |= token_name_sig(b[j]);
        if ((g_charq.sig & sb) == 0) return 0;
    }
    return ai_cb_core(ca, la_, na, cb, lb_, nb);
}

/* §6.140 用**缓存好的库句字符切片**算这一项（不再摊平、不再碰库句的符号节点）。
 * 调用方（`ai_sent_score_row`）已经用 256 位字符集位图证明过"两边至少共享一个字符"，
 * 所以 §6.127 那张 32 位 `ch_sig` 预筛在这里是**冗余**的（共享字符 ⇒ 两张位图都命中）。 */
static inline int ai_char_bigram_score_cached(uint64_t serial, Token *const *a, size_t an,
                                              const char *const *cb, const uint8_t *lb_, size_t nb) {
    if (!ai_charbg_on()) return 0;
    (void)ai_char_query_prep(serial, a, an);         /* 查询侧备忘（与老路同一个键）*/
    size_t na = g_charq.nc;
    if (na < 2 || nb < 2) return 0;
    return ai_cb_core(g_charq.c, g_charq.l, na, cb, lb_, nb);
}

/* 输入句与库里的输入句有多像 —— 三路信号合成：
 *   ① IDF 加权重合（主信号）：少见词比「你/我/吗」重要得多。
 *      纯 Dice 的毛病就是「你能帮我个忙吗」和「你会想我吗」共享 你/我/吗，
 *      分数接近 → 串话题。IDF 把这三个高频词的权重压下去。
 *   ② 字符 bigram：抓「换个说法问同一件事」。
 *   ③ 词级 Dice：短句兜底（「你好」这种没什么可加权的）。
 * 结果 0..100。 */
static inline int ai_sent_score_cov(AI *ai, Token *const *a, size_t an,
                                    Token *const *b, size_t bn, int *out_cov);

static inline int ai_pair_cb_slice(AI *ai, size_t i,
                                   const char *const **c, const uint8_t **l, size_t *n);
static inline const uint64_t *ai_pair_cb_bits(AI *ai, size_t i);   /* §6.210 前向（打分段要用）*/

/* §6.146 **查询侧的 IDF 权重备忘**（纯性能，行为零改动）：一句话要对几千条库句打分，
 * 而"查询词的 IDF 权重"和"查询侧权重和 wa"**每条库句都重算一遍**是白搭。
 * 实测（出厂模型 6701 条）：一对要查 `an + bn` ≈ 9.6 次 idf（其中 `bn` 次是给 `wb` 的，
 * 而 `wb` **从来没被用过** —— 见下面 `ai_sent_score_cov_ex` 里的注释）。
 * 键 = 池序号 + 查询词指针序列 + idf 表（长度/指针都比，idf 重建就作废）。 */
/* §6.582 打分用的临时表（每次问答重填）⇒ 线程局部。**尤其是 `g_qidf_tok`**
 * —— 它存的是 `Token*`，全局时会把"我池里的词"泄给别的线程（TSan 报的那处
 * "heap block ... 另一个线程在 pool_token 里 memcpy 名字"就是它）。 */
static AIP_TLS float        g_qidf[64];
static AIP_TLS const Token *g_qidf_tok[64];
static AIP_TLS size_t       g_qidf_n;
static AIP_TLS float        g_qidf_wa;
static AIP_TLS size_t       g_qidf_idf_n;
static AIP_TLS const float *g_qidf_idf;

static inline void ai_qidf_prep(const AI *ai, Token *const *a, size_t an) {
    if (!ai || !a || an == 0 || an > 64) { g_qidf_n = 0; return; }
    if (g_qidf_n == an && g_qidf_idf == ai->idf && g_qidf_idf_n == ai->idf_n &&
        memcmp(g_qidf_tok, a, an * sizeof *a) == 0)
        return;                                   /* 还是那句查询 ⇒ 直接用 */
    float wa = 0.0f;
    for (size_t i = 0; i < an; ++i) {
        g_qidf[i] = ai_idf_of(ai, a[i]);
        wa += g_qidf[i];
        g_qidf_tok[i] = a[i];
    }
    g_qidf_n = an;
    g_qidf_wa = wa;
    g_qidf_idf = ai->idf;
    g_qidf_idf_n = ai->idf_n;
}

/* §6.140 按**库句号**打分：字符那一项走缓存切片（§6.136/§6.140 的库句缓存）。
 * 拿到切片就完全不用碰库句的符号节点；没拿到（分配失败等）退回老路。 */
static inline int ai_sent_score_pair(AI *ai, Token *const *a, size_t an, size_t pi, int *out_cov) {
    if (out_cov) *out_cov = 0;
    if (!ai || pi >= ai->pairs_n) return 0;
    const AISentPair *p = &ai->pairs[pi];
    Token *const *b = (Token *const *)p->in;
    size_t bn = p->in_n;
    if (an == 0 || bn == 0) return 0;
    /* §6.146 一趟算出 wi 与 inter（以前是两趟），而且查询权重走备忘、**不再碰 idf**；
     * `wb`（库句那一侧的权重和）本来就没被任何判据用 —— 顺手不再算。 */
    ai_qidf_prep(ai, a, an);
    float wa = g_qidf_wa, wi = 0.0f;
    size_t inter = 0;
    for (size_t i = 0; i < an; ++i) {
        int hit = 0;
        for (size_t j = 0; j < bn; ++j)
            if (a[i] == b[j]) { hit = 1; break; }
        if (hit) { wi += g_qidf[i]; inter++; }
    }
    int cov = (wa > 0.0f) ? (int)(100.0f * wi / wa) : 0;
    int dice = (int)((inter * 200) / (an + bn));
    const char *const *cc = NULL; const uint8_t *ll = NULL; size_t nn = 0;
    int bg;
    if (ai_pair_cb_slice(ai, pi, &cc, &ll, &nn)) {
        /* §6.210 **bigram 布隆预筛**：跟这条库句一个 bigram 都不共享 ⇒ 字符项必然是 0
         * （`ai_cb_core` 的 inter 会算成 0），直接跳过那一趟字符比较。
         * 先确保懒补到了这一条（`ai_pair_cb_bits` 里连 `cb_bg` 一起补）。 */
        (void)ai_pair_cb_bits(ai, pi);
        if (ai->cb_bg && (g_charq.bg64 & ai->cb_bg[pi]) == 0)
            bg = 0;
        else
            bg = ai_char_bigram_score_cached(ai->pool ? ai->pool->serial : 0, a, an, cc, ll, nn);
    } else bg = ai_char_bigram_score(ai->pool ? ai->pool->serial : 0, a, an, b, bn);
    int sc = (cov * 55 + bg * 30 + dice * 15) / 100;
    if (sc > 100) sc = 100;
    if (out_cov) *out_cov = cov;
    return sc;
}

static inline int ai_sent_score(AI *ai, Token *const *a, size_t an,
                                Token *const *b, size_t bn) {
    return ai_sent_score_cov(ai, a, an, b, bn, NULL);
}

/* 同时给出「输入被覆盖了多少」（cov，0..100）。cov 才是「这个输入真的属于
 * 这条记忆吗」的判据 —— 总分里混了字符 bigram，短输入随便共享几个字就能凑到
 * 60 分，于是任何问题都被硬塞一个答案（实测「地球为什么是圆的」sc=63 是假匹配）。 */
static inline int ai_sent_score_cov(AI *ai, Token *const *a, size_t an,
                                    Token *const *b, size_t bn, int *out_cov) {
    if (out_cov) *out_cov = 0;
    if (an == 0 || bn == 0) return 0;
    /* ⚠ 这一份是**参考实现**（optest 的"行 vs 逐条对拍"拿它当基准）：故意保持"笨而直白"。
     * 里面的 `wb` 确实没被用到（`cov` 只用 wa/wi）—— 别顺手删：留着一份**不共享**的实现，
     * 对拍才有意义（§6.146 的性能改动只动行那条路）。 */
    float wa = 0.0f, wb = 0.0f, wi = 0.0f;
    for (size_t i = 0; i < an; ++i) {
        float w = ai_idf_of(ai, a[i]);
        wa += w;
        for (size_t j = 0; j < bn; ++j)
            if (a[i] == b[j]) { wi += w; break; }
    }
    for (size_t j = 0; j < bn; ++j) wb += ai_idf_of(ai, b[j]);
    int cov = (wa > 0.0f) ? (int)(100.0f * wi / wa) : 0;      /* 输入被覆盖多少 */
    size_t inter = 0;
    for (size_t i = 0; i < an; ++i)
        for (size_t j = 0; j < bn; ++j)
            if (a[i] == b[j]) { inter++; break; }
    int dice = (int)((inter * 200) / (an + bn));
    int bg = ai_char_bigram_score(ai && ai->pool ? ai->pool->serial : 0, a, an, b, bn);
    int sc = (cov * 55 + bg * 30 + dice * 15) / 100;
    if (sc > 100) sc = 100;
    if (out_cov) *out_cov = cov;
    return sc;
}

/* 完整的话题判定：词表票 + 整句相似度票，同场竞争。
 * 词表投票只要命中一个词就得分，会把真正的意图压掉；
 * 整句相似度是更精确的信号，必须能翻盘（「早上好」就是靠它定到问候话题的）。 */
/* §6.136 取第 i 条库句的**字符集位图**（懒补：库里新加的句子在这里第一次被打出来）。
 * 库句只增不减、`in` 写入后不变 ⇒ 打一次就够；`cb_n > pairs_n` 那个守卫是防
 * "往一个旧脑子里重新装盘"。 */
static inline const uint64_t *ai_pair_cb_bits(AI *ai, size_t i) {
    if (!ai || i >= ai->pairs_n) return NULL;
    if (ai->cb_n > ai->pairs_n) ai->cb_n = 0;              /* 库被重建过 ⇒ 缓存作废 */
    if (i >= ai->cb_n) {
        if (ai->cb_cap < ai->pairs_n) {
            size_t ncap = ai->pairs_n + 256;
            uint64_t *nb = (uint64_t *)realloc(ai->cb_bits, ncap * AI_CB_WORDS * sizeof *nb);
            if (!nb) return NULL;
            uint64_t *ng = (uint64_t *)realloc(ai->cb_bg, ncap * sizeof *ng);
            if (!ng) { return NULL; }            /* 布隆位拿不到就整条老路（切片也当没有）*/
            ai->cb_bits = nb; ai->cb_bg = ng; ai->cb_cap = ncap;
        }
        /* 切片那两条 blob 的容量一起保证（每包多留 256 条 × 96 字符的空间，够用很久）*/
        if (ai->cb_chcap < ai->pairs_n * 8 + 512) {
            size_t ncap = ai->pairs_n * 16 + 2048;
            uint32_t *no = (uint32_t *)realloc(ai->cb_off, (ncap + 1) * sizeof *no);
            const char **np = (const char **)realloc(ai->cb_ptr, ncap * sizeof *np);
            uint8_t *nl = (uint8_t *)realloc(ai->cb_len, ncap * sizeof *nl);
            if (!no || !np || !nl) { free(no); free(np); free(nl); return NULL; }
            ai->cb_off = no; ai->cb_ptr = np; ai->cb_len = nl; ai->cb_chcap = ncap;
        }
        if (!ai->cb_off) { /* 没分到偏移表：老路照走（返回位图，切片那侧返回 0）*/ }
        enum { MAXC = 96 };
        const char *cb[MAXC]; uint8_t lb_[MAXC];
        if (ai->cb_off && ai->cb_chused == 0) ai->cb_off[0] = 0;
        for (; ai->cb_n <= i; ++ai->cb_n) {
            const AISentPair *p = &ai->pairs[ai->cb_n];
            size_t bn = 0;
            for (size_t j = 0; j < p->in_n && bn + 8 < MAXC; ++j)
                bn += token_name_chars(p->in[j], cb + bn, lb_ + bn, MAXC - bn);
            uint64_t *dst = ai->cb_bits + ai->cb_n * AI_CB_WORDS;
            memset(dst, 0, AI_CB_WORDS * sizeof *dst);
            ai_cb_fill(dst, cb, lb_, bn);
            if (ai->cb_bg) {      /* §6.210：把这条库句的字符 bigram 压成 64 位布隆 */
                uint64_t bgv = 0;
                for (size_t k = 0; k + 1 < bn; ++k) {
                    uint64_t h = 1469598103934665603ull;
                    for (uint8_t q = 0; q < lb_[k]; ++q) h = (h ^ (uint8_t)cb[k][q]) * 1099511628211ull;
                    for (uint8_t q = 0; q < lb_[k+1]; ++q) h = (h ^ (uint8_t)cb[k+1][q]) * 1099511628211ull;
                    bgv |= 1ull << (h & 63);
                }
                ai->cb_bg[ai->cb_n] = bgv;
            }
            if (ai->cb_off) {
                while (ai->cb_chused + bn > ai->cb_chcap) {      /* 不够就翻倍（切片不失效）*/
                    size_t ncap = ai->cb_chcap * 2 + 1024;
                    const char **np = (const char **)realloc(ai->cb_ptr, ncap * sizeof *np);
                    uint8_t *nl = (uint8_t *)realloc(ai->cb_len, ncap * sizeof *nl);
                    uint32_t *no = (uint32_t *)realloc(ai->cb_off, (ncap + 1) * sizeof *no);
                    if (!np || !nl || !no) { free(np); free(nl); free(no); ai->cb_off = NULL; break; }
                    ai->cb_ptr = np; ai->cb_len = nl; ai->cb_off = no; ai->cb_chcap = ncap;
                }
                if (ai->cb_off) {
                    for (size_t k = 0; k < bn; ++k) { ai->cb_ptr[ai->cb_chused] = cb[k]; ai->cb_len[ai->cb_chused] = lb_[k]; ai->cb_chused++; }
                    ai->cb_off[ai->cb_n + 1] = (uint32_t)ai->cb_chused;
                }
            }
            ai->n_cb_fill++;
        }
    }
    return ai->cb_bits + i * AI_CB_WORDS;
}

/* §6.140 取第 i 条库句的**字符切片**（先在位图那条路上懒补好）。
 * 返回 1 = 拿到了（`c`/`l`/`n` 是展平的字符列表）；0 = 没有缓存（调用方走老路）。 */
static inline int ai_pair_cb_slice(AI *ai, size_t i,
                                   const char *const **c, const uint8_t **l, size_t *n) {
    if (!ai || i >= ai->pairs_n) return 0;
    if (i >= ai->cb_n) { const uint64_t *b = ai_pair_cb_bits(ai, i); if (!b) return 0; }
    if (!ai->cb_off || i + 1 > ai->cb_n) return 0;
    size_t a = ai->cb_off[i], z = ai->cb_off[i + 1];
    if (z > ai->cb_chused) return 0;
    *c = ai->cb_ptr + a; *l = ai->cb_len + a; *n = z - a;
    return 1;
}

/* §6.132：把"查询句 vs 每条库句"的分数量**一轮只算一遍**，两个检索循环共用。
 * 返回分数数组（长度 = ai->pairs_n），`out_cov` 拿到覆盖率数组；失败返回 NULL。 */
static inline const int *ai_sent_score_row(AI *ai, Token *const *words, size_t n,
                                           const int **out_cov) {
    if (out_cov) *out_cov = NULL;
    if (!ai || !words || n == 0 || n > 64) return NULL;
    ai->n_sc_call++;
    uint64_t tick = ai->pool ? ai->pool->current_tick : 0;
    int valid = ai->sc_valid && ai->sc_tick == tick &&
                ai->sc_pairs_n == ai->pairs_n && ai->sc_idf_n == ai->idf_n &&
                ai->sc_qn == n && memcmp(ai->sc_q, words, n * sizeof *words) == 0;
    if (!valid) {
        ai->n_sc_miss++;
        if (ai->sc_cap < ai->pairs_n) {
            size_t ncap = ai->pairs_n * 2 + 16;
            int *a = (int *)malloc(ncap * sizeof *a);
            int *b = (int *)malloc(ncap * sizeof *b);
            if (!a || !b) { free(a); free(b); return NULL; }
            free(ai->sc_memo); free(ai->sc_cov);
            ai->sc_memo = a; ai->sc_cov = b; ai->sc_cap = ncap;
        }
        /* §6.136 **先过位图、再碰库句**：这一步只读那块 32 字节/条的位图（214 KB，能进 L2），
         * 字符集不相交的条**根本不去摸 `pairs[i]`**（那条 520 字节的结构体是这一趟的主要内存
         * 流量）。判据**精确**：一个字符都不共享 ⇒ 两边一个词都不共享 ⇒ cov=0、dice=0，
         * 而字符项按现在的实现（逐字符比）也必然是 0 ⇒ sc=(0+0+0)/100=0、cov=0 ——
         * 与逐条算出来的**完全一样**。位图碰撞只会让预筛放行（多算一次），不改分数。 */
        (void)ai_char_query_prep(ai->pool ? ai->pool->serial : 0, words, n);
        for (size_t i = 0; i < ai->pairs_n; ++i) {
            const uint64_t *cbv = ai_pair_cb_bits(ai, i);
            if (cbv && !ai_cb_any(g_charq.cbits, cbv)) {
                ai->sc_memo[i] = 0;
                ai->sc_cov[i] = 0;
                continue;
            }
            int cv = 0;
            ai->sc_memo[i] = ai_sent_score_pair(ai, words, n, i, &cv);   /* §6.140：走缓存切片 */
            ai->sc_cov[i] = cv;
        }
        ai->sc_tick = tick;
        ai->sc_pairs_n = ai->pairs_n;
        ai->sc_idf_n = ai->idf_n;
        for (size_t i = 0; i < n; ++i) ai->sc_q[i] = words[i];
        ai->sc_qn = n;
        ai->sc_valid = 1;
    }
    if (out_cov) *out_cov = ai->sc_cov;
    return ai->sc_memo;
}

static inline uint8_t ai_detect_topic(AI *ai, Token *const *words, size_t n) {
    uint8_t best = ai_detect_topic_words(ai, words, n);
    uint32_t best_v = 0;
    if (best) {
        for (size_t i = 0; i < n; ++i)
            if (ai_topic_has(ai, best, words[i])) best_v++;
    }
    if (!ai || !ai->pairs || ai->pairs_n == 0) return best;
    /* 每个话题取「库里最像的那一条」的分数，而不是把该话题所有对的相似度**累加**。
     * 累加会奖励话题规模（实测小话题「支持」49 分 输给大话题「想念」的累加 5598），
     * 取最大值才是「这个输入到底属于哪个话题」。 */
    uint32_t tv[AI_MAX_TOPICS];
    memset(tv, 0, sizeof tv);
    const int *row = ai_sent_score_row(ai, words, n, NULL);   /* §6.132 一轮算一遍 */
    for (size_t i = 0; i < ai->pairs_n; ++i) {
        const AISentPair *p = &ai->pairs[i];
        if (p->topic == ASSOC_TOPIC_NONE || p->topic >= AI_MAX_TOPICS) continue;
        int sc = row ? row[i] : ai_sent_score(ai, words, n, (Token *const *)p->in, p->in_n);
        if (sc > 0 && (uint32_t)sc > tv[p->topic]) tv[p->topic] = (uint32_t)sc;
    }
    /* 整句票和词表票尺度不同（命中词数 vs 相似度），从零重比。 */
    best_v = 0;
    for (uint8_t t = 1; t < AI_MAX_TOPICS; ++t) {
        if (ai->topics[t].n == 0) continue;
        if (tv[t] > best_v) { best_v = tv[t]; best = t; }
    }
    return best_v ? best : ASSOC_TOPIC_NONE;
}

static inline const AISentPair *ai_sent_lookup(AI *ai, Token *const *words, size_t n,
                                               uint8_t topic, int *out_score,
                                               int *out_cov) {
    if (out_cov) *out_cov = 0;
    ai->last_token_ratio = 0;
    if (!ai || !ai->pairs || ai->pairs_n == 0 || !words || n == 0) return NULL;
    int best = -1, best_cov = 0;
    size_t best_i = 0, ties = 0, best_len = 0;
    const int *cov_row = NULL;
    const int *row = ai_sent_score_row(ai, words, n, &cov_row);   /* §6.132 与 detect_topic 共用 */
    for (size_t i = 0; i < ai->pairs_n; ++i) {
        const AISentPair *p = &ai->pairs[i];
        int cv = 0;
        int sc = row ? row[i] : ai_sent_score_cov(ai, words, n, p->in, p->in_n, &cv);
        if (row) cv = cov_row[i];
        if (topic != ASSOC_TOPIC_NONE && p->topic == topic) sc += 25;
        if (sc > best) { best = sc; best_i = i; ties = 1; best_cov = cv;
                         best_len = p->in_n; }
        else if (sc == best) {
            /* 蓄水池抽样：在并列里等概率挑一个 */
            ties++;
            ai->rng ^= ai->rng << 13; ai->rng ^= ai->rng >> 17; ai->rng ^= ai->rng << 5;
            if (ai->rng % ties == 0) best_i = i;
        }
    }
    if (best < 0) return NULL;
    if (out_score) *out_score = best;
    if (out_cov) *out_cov = best_cov;
    ai->last_retrieval_len = best_len;
    ai->last_input_len = n;
    /* **词命中比例**：输入里有多少个词真的出现在命中的那条输入句里。
     * 比 IDF 覆盖率可靠 —— IDF 会被「一个少见词命中」抬高
     * （实测「太阳 离 我们 多 远」cov=43 但只有「远」一个字真出现过）。 */
    {
        const AISentPair *bp = &ai->pairs[best_i];
        size_t hit = 0;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < bp->in_n; ++j)
                if (words[i] == bp->in[j]) { hit++; break; }
        ai->last_token_ratio = n ? (int)(100 * hit / n) : 0;
    }
    return &ai->pairs[best_i];
}


/* 检索：返回最佳匹配的回复句（调用方负责把符号节点* 转成字符串）。
 * 命中话题的候选有加权；同分时用 rng 随机选一个，避免每次都同一句。 */

/* 把一串符号节点拼成可读文本（CJK 之间补空格，ASCII 字母数字之间不补） */
static inline char *ai_tokens_to_text(Token *const *toks, size_t n) {
    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!toks[i] || !toks[i]->name) continue;
        const char *nm = toks[i]->name;
        size_t nl = strlen(nm);
        while (len + nl + 2 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        if (len > 0) {
            unsigned char a = (unsigned char)buf[len-1];
            unsigned char b = (unsigned char)nm[0];
            int ascii_pair = (a < 128 && b < 128 && isalnum(a) && isalnum(b));
            if (!ascii_pair) buf[len++] = ' ';
        }
        memcpy(buf + len, nm, nl);
        len += nl; buf[len] = 0;
    }
    return buf;
}

/* ---------------- 模板合成：说出语料里没有的句子 ---------------- */

/* 把「输入 → 库里输入」的差异当成槽位替换，套用到回复上。
 *
 * 例：库里 `你 喜欢 猫 吗 → 我 喜欢 猫`。
 *     用户问 `你 喜欢 狗 吗`（语料里没有这一对）。
 *     差异在 猫↔狗，而它们同槽位 → 输出 `我 喜欢 狗` —— 这句话语料里没有。
 *
 * 骨架是学来的（所以通顺），内容是新拼的（所以是「生成」）。
 * 只在确实发生了替换时才返回；纯复述交给 ai_sent_lookup。 */
static inline char *ai_synthesize(AI *ai, Token *const *words, size_t n,
                                  uint8_t topic, int *out_score) {
    if (out_score) *out_score = 0;
    if (!ai || !ai->pairs || ai->pairs_n == 0 || !words || n == 0) return NULL;

    /* 取最像的 24 条候选 */
    enum { CAND = 24 };
    size_t cand[CAND]; int csc[CAND]; size_t nc = 0;
    for (size_t i = 0; i < ai->pairs_n; ++i) {
        const AISentPair *p = &ai->pairs[i];
        int sc = ai_sent_score(ai, words, n, (Token *const *)p->in, p->in_n);
        if (topic != ASSOC_TOPIC_NONE && p->topic == topic) sc += 25;
        if (nc < CAND) { cand[nc] = i; csc[nc] = sc; nc++; }
        else {
            size_t worst = 0;
            for (size_t k = 1; k < CAND; ++k) if (csc[k] < csc[worst]) worst = k;
            if (sc > csc[worst]) { cand[worst] = i; csc[worst] = sc; }
        }
    }
    /* 从高分到低分试 */
    for (size_t rank = 0; rank < nc; ++rank) {
        size_t bi = 0; int bv = -1;
        for (size_t k = 0; k < nc; ++k) if (csc[k] > bv) { bv = csc[k]; bi = k; }
        if (bv < 0) break;
        size_t pi = cand[bi];
        csc[bi] = -1;
        if (bv < 60) break;                      /* 太不像就别硬套 */

        const AISentPair *p = &ai->pairs[pi];
        /* 模板必须和输入**差不多长**：我们要的是「同一句话换了个词」，
         * 不是「一句长话里碰巧含几个相同的词」。实测不做这个限制，
         * 匹配器会挑长的相似句，未匹配数两边不等，合成永远不触发（0/10）。 */
        {
            size_t hi = n > p->in_n ? n : p->in_n;
            size_t lo = n < p->in_n ? n : p->in_n;
            if (hi == 0 || lo * 2 < hi) continue;
        }
        /* **干净的一换一**：输入和模板里，各自恰好有一个词是对方没有的。
         * 这样模板就是「只差一个槽位」的同构句，替换一定成立；
         * 放宽成「任意同位词都可换」会产出「不 用 我 歉」这种病句（实测过）。 */
        Token *a_only[AI_MAX_SENT], *b_only[AI_MAX_SENT];
        size_t na = 0, nb = 0;
        for (size_t i2 = 0; i2 < n; ++i2) {
            int f = 0;
            for (size_t q = 0; q < p->in_n; ++q) if (p->in[q] == words[i2]) { f = 1; break; }
            if (!f && na < AI_MAX_SENT) a_only[na++] = words[i2];
        }
        for (size_t q = 0; q < p->in_n; ++q) {
            int f = 0;
            for (size_t i2 = 0; i2 < n; ++i2) if (words[i2] == p->in[q]) { f = 1; break; }
            if (!f && nb < AI_MAX_SENT) b_only[nb++] = p->in[q];
        }
        if (na == 0 || na != nb) continue;         /* 两边未匹配数必须相等 */
        if (na > 4) continue;                      /* 换太多就不是同构句了 */
        /* 未匹配的词必须能一一配对且同位 */
        int usedb[AI_MAX_SENT] = {0};
        int pair_ok = 1;
        for (size_t x = 0; x < na && pair_ok; ++x) {
            int hit = 0;
            for (size_t y = 0; y < nb; ++y) {
                if (usedb[y]) continue;
                if (ai_same_slot(ai, a_only[x], b_only[y])) { usedb[y] = 1; hit = 1; break; }
            }
            if (!hit) pair_ok = 0;
        }
        if (!pair_ok) continue;
        {
            Token *out[AI_MAX_SENT];
            size_t on = p->out_n > AI_MAX_SENT ? AI_MAX_SENT : p->out_n;
            for (size_t j = 0; j < on; ++j) out[j] = p->out[j];
            size_t swapped = 0;
            for (size_t y = 0; y < nb; ++y) {
                /* 找出 b_only[y] 对应的 a_only[x] */
                Token *rep = NULL;
                int useda[AI_MAX_SENT] = {0};
                for (size_t x = 0; x < na; ++x) {
                    if (useda[x]) continue;
                    if (ai_same_slot(ai, a_only[x], b_only[y])) { rep = a_only[x]; useda[x] = 1; break; }
                }
                if (!rep) continue;
                for (size_t j = 0; j < on; ++j)
                    if (out[j] == b_only[y]) { out[j] = rep; swapped = 1; }
            }
            if (!swapped) continue;
            if (out_score) *out_score = bv;
            ai->n_synth++;
            return ai_tokens_to_text(out, on);
        }
    }
    return NULL;
}


static inline Token *ai_pick_next_role_pat(AI *ai, Token *cur, float energy, int do_touch,
                                           uint8_t want, uint8_t *out_used,
                                           uint8_t *out_pat) {
    if (!cur || cur->count == 0) return NULL;

    /* 角色优先级：想要的角色排第一，通用共现次之，回复角色垫底。
     * 两个关键点：
     *   ① 目标角色没有可用出边时**必须**能退到通用边，否则该词直接饿死；
     *   ② 从输入尾词出发时 want 是 NONE，也得能挑到回复角色的边 ——
     *      否则「输入尾词 → 回复首词」这条主干永远用不上。 */
    uint8_t order[4];
    size_t n_order = 0;
    if (want != ASSOC_ROLE_NONE) order[n_order++] = want;
    order[n_order++] = ASSOC_ROLE_NONE;
    if (want != ASSOC_ROLE_REPLY) order[n_order++] = ASSOC_ROLE_REPLY;
    /* ①f：缓存（`role=推论`）作为**最后一档**（默认根本不进 order ⇒ 按构造进不了联想） */
    if (ai->cache_walk && n_order < (int)(sizeof order / sizeof order[0]))
        order[n_order++] = ASSOC_ROLE_DERIVED;

    /* 分两层：内容词优先，边界标记（[对话:结束]/[原语:分割]）只能兜底。
     * 否则「吗 → [对话:结束]」会被当成一次正常续接，句子刚开头就收了。 */
    if (g_walk_trace) fprintf(stderr, "PICK cur=%s cnt=%zu want=%u e=%.4f\n",
                              cur->name ? cur->name : "?", cur->count,
                              (unsigned)want, (double)energy);
    for (int tier = 0; tier < 2; ++tier) {
        uint8_t use = ASSOC_ROLE_NONE;
        uint64_t total = 0;
        for (size_t r = 0; r < n_order && total == 0; ++r) {
            for (size_t i = 0; i < cur->count; ++i) {
                AssocEntry *e = &token_assocs(cur)[i];
                if (e->role != order[r]) continue;
                if (!ai_edge_walkable(ai, e, energy)) continue;
                if (!ai_can_continue_from(ai, cur, e->main, energy)) continue;
                int boundary = ai_is_reply_end(e->main) || ai_is_dialog_split(e->main);
                if (tier == 0 && boundary) continue;      /* 第一层只要内容词 */
                if (tier == 1 && !boundary) continue;     /* 第二层只看边界 */
                total += ai_edge_weight(ai, e);
                if (g_walk_trace) fprintf(stderr, "  C tier=%d r=%u i=%zu tok=%s w=%u tot=%llu\n",
                                          tier, (unsigned)order[r], i,
                                          e->main && e->main->name ? e->main->name : "?",
                                          ai_edge_weight(ai, e), (unsigned long long)total);
            }
            if (total) use = order[r];
        }
        if (total == 0) continue;

        ai->rng ^= ai->rng << 13; ai->rng ^= ai->rng >> 17; ai->rng ^= ai->rng << 5;
        uint64_t pick = (uint64_t)ai->rng % total, acc = 0;
        if (g_walk_trace) fprintf(stderr, "  TOTAL tier=%d use=%u total=%llu rng=%u pick=%llu\n",
                                  tier, (unsigned)use, (unsigned long long)total,
                                  ai->rng, (unsigned long long)pick);
        for (size_t i = 0; i < cur->count; ++i) {
            AssocEntry *e = &token_assocs(cur)[i];
            if (e->role != use) continue;
            if (!ai_edge_walkable(ai, e, energy)) continue;
            if (!ai_can_continue_from(ai, cur, e->main, energy)) continue;
            int boundary = ai_is_reply_end(e->main) || ai_is_dialog_split(e->main);
            if (tier == 0 && boundary) continue;
            if (tier == 1 && !boundary) continue;
            acc += ai_edge_weight(ai, e);
            if (pick < acc) {
                if (do_touch) token_touch_entry(cur, e);
                if (out_used) *out_used = use;
                if (out_pat) *out_pat = e->pattern;
                return e->main;
            }
        }
    }
    return NULL;
}

static inline Token *ai_pick_next_role(AI *ai, Token *cur, float energy, int do_touch,
                                       uint8_t want, uint8_t *out_used) {
    return ai_pick_next_role_pat(ai, cur, energy, do_touch, want, out_used, NULL);
}

static inline Token *ai_pick_next_ex(AI *ai, Token *cur, float energy, int do_touch) {
    return ai_pick_next_role(ai, cur, energy, do_touch, ASSOC_ROLE_NONE, NULL);
}

/* 带话题约束的选边：只走「本话题」或「无话题」的边。
 * 话题锁定后跨话题的边一律排除 —— 这是「按意图约束解码」的核心。 */
static inline Token *ai_pick_next_topic(AI *ai, Token *cur, float energy, int do_touch,
                                        uint8_t want, uint8_t topic, uint8_t *out_used,
                                        uint8_t *out_pat) {
    if (!cur || cur->count == 0) return NULL;
    if (topic == ASSOC_TOPIC_NONE)
        return ai_pick_next_role_pat(ai, cur, energy, do_touch, want, out_used, out_pat);

    uint8_t order[4];
    size_t n_order = 0;
    if (want != ASSOC_ROLE_NONE) order[n_order++] = want;
    order[n_order++] = ASSOC_ROLE_NONE;
    if (want != ASSOC_ROLE_REPLY) order[n_order++] = ASSOC_ROLE_REPLY;
    /* ①f：缓存（`role=推论`）作为**最后一档**（默认根本不进 order ⇒ 按构造进不了联想） */
    if (ai->cache_walk && n_order < (int)(sizeof order / sizeof order[0]))
        order[n_order++] = ASSOC_ROLE_DERIVED;

    if (g_walk_trace) fprintf(stderr, "PICKT cur=%s cnt=%zu want=%u topic=%u e=%.4f\n",
                              cur->name ? cur->name : "?", cur->count,
                              (unsigned)want, (unsigned)topic, (double)energy);
    for (int pass = 0; pass < 2; ++pass) {
        /* pass 0：只接受本话题的边；pass 1：放宽到无话题/其它话题 */
        for (int tier = 0; tier < 2; ++tier) {
            uint8_t use = ASSOC_ROLE_NONE;
            uint64_t total = 0;
            for (size_t r = 0; r < n_order && total == 0; ++r) {
                for (size_t i = 0; i < cur->count; ++i) {
                    AssocEntry *e = &token_assocs(cur)[i];
                    if (e->role != order[r]) continue;
                    if (pass == 0 && e->topic != topic) continue;
                    if (pass == 1 && e->topic == topic) continue;
                    if (!ai_edge_walkable(ai, e, energy)) continue;
                    if (!ai_can_continue_from(ai, cur, e->main, energy)) continue;
                    int boundary = ai_is_reply_end(e->main) || ai_is_dialog_split(e->main);
                    if (tier == 0 && boundary) continue;
                    if (tier == 1 && !boundary) continue;
                    total += ai_edge_weight(ai, e);
                    if (g_walk_trace) fprintf(stderr, "  CT pass=%d tier=%d r=%u i=%zu tok=%s w=%u tot=%llu\n",
                                              pass, tier, (unsigned)order[r], i,
                                              e->main && e->main->name ? e->main->name : "?",
                                              ai_edge_weight(ai, e), (unsigned long long)total);
                }
                if (total) use = order[r];
            }
            if (total == 0) continue;

            ai->rng ^= ai->rng << 13; ai->rng ^= ai->rng >> 17; ai->rng ^= ai->rng << 5;
            uint64_t pick = (uint64_t)ai->rng % total, acc = 0;
            if (g_walk_trace) fprintf(stderr, "  TOTALT pass=%d tier=%d use=%u total=%llu rng=%u pick=%llu\n",
                                      pass, tier, (unsigned)use, (unsigned long long)total,
                                      ai->rng, (unsigned long long)pick);
            for (size_t i = 0; i < cur->count; ++i) {
                AssocEntry *e = &token_assocs(cur)[i];
                if (e->role != use) continue;
                if (pass == 0 && e->topic != topic) continue;
                if (pass == 1 && e->topic == topic) continue;
                if (!ai_edge_walkable(ai, e, energy)) continue;
                if (!ai_can_continue_from(ai, cur, e->main, energy)) continue;
                int boundary = ai_is_reply_end(e->main) || ai_is_dialog_split(e->main);
                if (tier == 0 && boundary) continue;
                if (tier == 1 && !boundary) continue;
                acc += ai_edge_weight(ai, e);
                if (pick < acc) {
                    if (do_touch) token_touch_entry(cur, e);
                    if (out_used) *out_used = use;
                    if (out_pat) *out_pat = e->pattern;
                    return e->main;
                }
            }
        }
    }
    return NULL;
}
static inline char *ai_generate_scored_topic(AI *ai, Token *seed, float energy,
                                             size_t max_len, size_t *out_steps,
                                             float *out_score, uint8_t topic) {
    if (out_score) *out_score = 0;
    if (out_steps) *out_steps = 0;
    if (!ai || !seed) return NULL;

    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;

    Token *path[AI_MAX_PATH];
    size_t path_n = 0;
    path[path_n++] = seed;

    token_touch(seed);
    if (seed->name) {
        size_t nl = strlen(seed->name);
        while (len + nl + 1 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        memcpy(buf + len, seed->name, nl);
        len += nl; buf[len] = 0;
    }
    Token *cur = seed;
    float e = energy;
    size_t steps = 0;
    /* 角色寄存器：跨过「输入/回复」边界之后就锁死在 REPLY，
     * 之后每一步只在「回复内部」学到的那批边里走。 */
    uint8_t want = (seed && ai_is_dialog_split(seed)) ? ASSOC_ROLE_REPLY
                                                      : ASSOC_ROLE_NONE;
    uint8_t pat_from_cur = ASSOC_PAT_NONE;
    ai->cur_topic = topic;
    for (size_t step = 0; step < max_len; ++step) {
        if (e < ai->cutoff) break;
        uint8_t used = ASSOC_ROLE_NONE, pat = ASSOC_PAT_NONE;
        Token *next = ai_pick_next_topic(ai, cur, e, 1, want, topic, &used, &pat);
        /* 退回了通用边说明「回复语境」在这里断了：之后不再假装还在回复里，
         * 否则会用回复角色去挑词、又混着通用边，出来的句子两头不靠。 */
        if (used != want) want = used;
        if (!next || next == ai->unknowing || !next->name || next == cur) break;
        if (ai_is_dialog_split(next)) {          /* 跨过「输入/回复」边界，不输出它 */
            want = ASSOC_ROLE_REPLY;
            cur = next; e *= ai->decay; steps++;
            continue;
        }
        if (ai_is_reply_end(next)) {             /* 回复结束：收句 */
            steps++;
            break;
        }
        /* 句尾模式：这条边是在「从这个词往后还剩 N 个词」的语境里学到的。
         * TAIL0 = 它本来就是句尾 → 到了就收；
         * TAIL1 = 后面只剩一个词 → 再走一步就收。
         * 只看 [对话:结束] 是不够的：那条边经常选不中，句子就一直漂。 */
        if (pat_from_cur == ASSOC_PAT_TAIL1) { steps++; break; }
        if (pat_from_cur == ASSOC_PAT_TAIL0) { steps++; break; }
        pat_from_cur = pat;
        {   /* 环检测：撞到已经走过的词就收句。语料里的共现图有不少小环
             * （反义/什么/属 于 …），游走一旦进环就会反复念同一个词。 */
            int seen = 0;
            for (size_t k = 0; k < path_n; ++k)
                if (path[k] == next) { seen = 1; break; }
            if (seen) { steps++; break; }
        }
        size_t nl = strlen(next->name);
        while (len + nl + 2 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        /* 词之间要留空格：CJK 词如果直接拼会变成「好朋友属于」这种糊在一起的串
         * （tokenize 是按空白 + 标点切、CJK 按字切的，粘一起就读不回来了）。
         * 只有「ASCII 字母数字彼此相邻」才不加空格，避免 "abc" "def" 变成 "abc def"。 */
        if (len > 0) {
            unsigned char a = (unsigned char)buf[len-1];
            unsigned char b = (unsigned char)next->name[0];
            int ascii_pair = (a < 128 && b < 128 && isalnum(a) && isalnum(b));
            if (!ascii_pair) buf[len++] = ' ';
        }
        memcpy(buf + len, next->name, nl);
        len += nl; buf[len] = 0;
        if (path_n < AI_MAX_PATH) path[path_n++] = next;
        ai_push_trace(ai, next);
        cur = next;
        e *= ai->decay;
        steps++;
    }

    float score = 0;
    for (size_t i = 0; i < path_n; ++i)
        for (size_t j = 0; j < path[i]->count; ++j) {
            Token *m = token_assocs_ro(path[i])[j].main;
            if (m == ai->good) score += 1.0f;
            if (m == ai->bad)  score -= 1.0f;
        }
    if (out_score) *out_score = score;
    if (out_steps) *out_steps = steps;

    ai->last_path.n = path_n;
    memcpy(ai->last_path.path, path, path_n * sizeof *path);
    return buf;
}
static inline char *ai_generate_scored(AI *ai, Token *seed, float energy,
                                       size_t max_len, size_t *out_steps, float *out_score) {
    return ai_generate_scored_topic(ai, seed, energy, max_len, out_steps, out_score,
                                    ASSOC_TOPIC_NONE);
}

static inline void ai_push_skipped(AI *ai, Token *t) {
    if (!ai || !t) return;
    if (ai->last_skipped_n == ai->last_skipped_cap) {
        size_t ncap = ai->last_skipped_cap ? ai->last_skipped_cap * 2 : 8;
        Token **nt = (Token **)realloc(ai->last_skipped, ncap * sizeof *nt);
        if (!nt) return;
        ai->last_skipped = nt; ai->last_skipped_cap = ncap;
    }
    ai->last_skipped[ai->last_skipped_n++] = t;
}
static inline char *ai_plan_and_act(AI *ai, Token *seed,
                                    size_t *out_steps, float *out_conf,
                                    char **input_words, size_t n_words) {
    if (!ai || !seed) return NULL;
    int n = ai_effective_candidates(ai);
    float e = ai_effective_energy(ai);
    char *best = NULL;
    float best_score = -1e30f;
    size_t best_steps = 0;
    ai->last_skipped_n = 0;
    for (int i = 0; i < n; ++i) {
        size_t steps; float score;
        char *cand = ai_generate_scored_topic(ai, seed, e, ai->max_gen, &steps, &score,
                                              ai->cur_topic);
        if (g_walk_trace) {
            fprintf(stderr, "W i=%d steps=%zu score=%.6f n=%zu:", i, steps,
                    (double)score, ai->last_path.n);
            for (size_t z = 0; z < ai->last_path.n; ++z) {
                Token *pp = ai->last_path.path[z];
                fprintf(stderr, " %s", pp && pp->name ? pp->name : "?");
            }
            fprintf(stderr, "\n");
        }
        if (!cand) continue;
        Token *first = (ai->last_path.n >= 2) ? ai->last_path.path[1] : NULL;
        if (score > best_score) {
            if (best && first) ai_push_skipped(ai, first);
            free(best);
            best = cand; best_score = score; best_steps = steps;
            /* 记下「被选中的那条」的路径：last_path 会被后面几条候选覆盖。 */
            ai->last_walk_path = ai->last_path;
        } else {
            if (first) ai_push_skipped(ai, first);
            free(cand);
        }
    }
    if (best) {
        float conf = ai_confidence(ai, seed, NULL, 0, e,
                                   best_steps, ai->max_gen,
                                   input_words, n_words);
        if (out_conf) *out_conf = conf;
        if (out_steps) *out_steps = best_steps;
    }
    /* **N1 参与决策**（默认关）：走边没走出好东西（分数低于阈值）而**思维对象想通了**
     * （缺口 0 + 已复核 + 有结论）时，用它的结论当结果 —— 这样"想出来的"就不只是痕了。 */
    if (ai->n1_decide && ai->n1_verified && ai->n1_gaps == 0 && ai->n1_concl[0] &&
        best_score < 0.40f) {
        char *nc = (char *)malloc(strlen(ai->n1_concl) + 1);
        if (nc) {
            memcpy(nc, ai->n1_concl, strlen(ai->n1_concl) + 1);
            free(best);
            best = nc;
            best_score = 0.60f;      /* 比"没走出来"高，但不压过正常走出来的高分候选 */
            best_steps = 1;
            if (out_conf) *out_conf = 0.60f;
        }
    }
    return best;
}

/* ============================ 抽象 ============================ */

static inline size_t ai_jaccard(const Token *a, const Token *b) {
    if (!a || !b || a->count == 0 || b->count == 0) return 0;
    size_t inter = 0;
    for (size_t i = 0; i < a->count; ++i) {
        Token *m = token_assocs_ro(a)[i].main;
        if (!m || (m->name && m->name[0] == '[')) continue;
        for (size_t j = 0; j < b->count; ++j)
            if (token_assocs_ro(b)[j].main == m) { inter++; break; }
    }
    size_t uni = a->count + b->count - inter;
    return uni ? (inter * 100) / uni : 0;
}
/* §6.253 **可分片调用**：从 `from` 扫到 `to`（不含），最多再建 `max_new - *made_io` 个抽象。
 * 分成多步的目的是**把一次 ~200 ms 的步骤摊到多轮**（睡眠本来就是"每轮一步"）。
 * 等价性：外层 i 仍**升序**、每建一个就 break 内层、上限仍是 3 ⇒ 结果与一次跑完相同。 */
static inline void ai_form_abstractions_slice(AI *ai, size_t from, size_t to,
                                             size_t max_new, size_t *made_io) {
    if (!ai || !ai->pool) return;
    TokenPool *p = ai->pool;
    size_t created = made_io ? *made_io : 0;
    const size_t MAX_NEW = max_new;
    /* §6.238 **候选集倒排**（精确剪枝，不改结果）：jaccard≥70 要求两边共享**很多**邻居，
     * 至少 1 个（两侧出度都 ≥3 时其实 ≥3 个）。原来的二重循环对**每一对**都算
     * `ai_jaccard`（O(出度²)）。这里先建"谁指向 m"的倒排表（O(E)），再只对**与 a 共享邻居**
     * 的 b 算 jaccard，候选按 id 升序处理 —— 与原来的"j 从小到大、第一个满足就取"等价。 */
    size_t n = p->count;
    size_t ecount = 0;
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        for (size_t k = 0; k < t->count; ++k) {
            Token *m = token_assocs(t)[k].main;
            if (!m || (m->name && m->name[0] == '[')) continue;
            if (m->id >= n) continue;
            ecount++;
        }
    }
    uint32_t *in_off = (uint32_t *)calloc(n + 2, sizeof *in_off);
    uint32_t *in_src = (uint32_t *)malloc((ecount + 1) * sizeof *in_src);
    uint8_t  *seen   = (uint8_t *)calloc(n + 1, 1);
    uint32_t *cand   = (uint32_t *)malloc((n + 1) * sizeof *cand);
    if (!in_off || !in_src || !seen || !cand || ecount == 0) {
        free(in_off); free(in_src); free(seen); free(cand);
        if (ecount == 0) return;
    }
    for (size_t i = 0; i < n; ++i) {
        Token *t = p->tokens[i];
        for (size_t k = 0; k < t->count; ++k) {
            Token *m = token_assocs(t)[k].main;
            if (!m || (m->name && m->name[0] == '[')) continue;
            if (m->id >= n) continue;
            in_off[m->id + 1]++;
        }
    }
    for (size_t i = 0; i < n; ++i) in_off[i + 1] += in_off[i];
    {
        uint32_t *cur = (uint32_t *)malloc((n + 2) * sizeof *cur);
        if (!cur) { free(in_off); free(in_src); free(seen); free(cand); return; }
        memcpy(cur, in_off, (n + 2) * sizeof *cur);
        for (size_t i = 0; i < n; ++i) {
            Token *t = p->tokens[i];
            for (size_t k = 0; k < t->count; ++k) {
                Token *m = token_assocs(t)[k].main;
                if (!m || (m->name && m->name[0] == '[')) continue;
                if (m->id >= n) continue;
                uint32_t pos = cur[m->id]++;
                if (pos < ecount) in_src[pos] = (uint32_t)i;
            }
        }
        free(cur);
    }

    size_t lim = (to < p->count) ? to : p->count;
    for (size_t i = from; i < lim && created < MAX_NEW; ++i) {
        Token *a = p->tokens[i];
        if (a->is_garbage || !a->name || a->parent) continue;
        if (a->name[0] == '[' || a->count < 3) continue;
        /* 候选 = a 的邻居们的"入边来源"（去重、只要 id > i）*/
        size_t ncand = 0;
        for (size_t k = 0; k < a->count; ++k) {
            Token *m = token_assocs(a)[k].main;
            if (!m || (m->name && m->name[0] == '[')) continue;
            if (m->id >= n) continue;
            for (uint32_t q = in_off[m->id]; q < in_off[m->id + 1]; ++q) {
                uint32_t t = in_src[q];
                if (t <= i || t >= n) continue;
                if (seen[t]) continue;
                seen[t] = 1;
                cand[ncand++] = t;
            }
        }
        /* 按 id 升序（原来的 j 就是从小到大）*/
        for (size_t x = 1; x < ncand; ++x) {
            uint32_t v = cand[x]; size_t y = x;
            while (y > 0 && cand[y - 1] > v) { cand[y] = cand[y - 1]; --y; }
            cand[y] = v;
        }
        for (size_t x = 0; x < ncand; ++x) seen[cand[x]] = 0;   /* 清标记 */
        for (size_t ci = 0; ci < ncand && created < MAX_NEW; ++ci) {
            size_t j = cand[ci];
            Token *b = p->tokens[j];
            if (b->is_garbage || !b->name || b->parent) continue;
            if (b->name[0] == '[' || b->count < 3) continue;
            /* §6.238b 试过"度数比剪枝"（min(ca,cb) ≥ 0.7·max），**撤了**：
             * 代数上该成立，实测却**改变了选中的配对**（抽象 49 挂在别的词上）。
             * 等价性没证住的东西不许留 —— 只保留"共享邻居"那条（已验证逐字节等价）。 */
            if (ai_jaccard(a, b) < 70) continue;
            char cname[64];
            snprintf(cname, sizeof cname, "[抽象%zu]", ai->n_abstractions);
            Token *c = pool_token(p, cname, NULL);
            if (!c) continue;
            token_set_category(c, 1);
            token_set_parent(a, c);
            token_set_parent(b, c);
            for (size_t k = 0; k < a->count; ++k) {
                Token *m = token_assocs(a)[k].main;
                if (!m || m == ai->good || m == ai->bad) continue;
                token_add_assoc_unique(c, m);
            }
            created++; ai->n_abstractions++;
            break;
        }
    }
    if (made_io) *made_io = created;
    free(in_off); free(in_src); free(seen); free(cand);
}

/* 老入口（一次跑完）：probe/测试仍用它 */
static inline void ai_form_abstractions(AI *ai) {
    size_t made = 0;
    ai_form_abstractions_slice(ai, 0, ai && ai->pool ? ai->pool->count : 0, 3, &made);
}

/* ============================ 睡眠（增量） ============================ */


static inline void ai_sleep_begin(AI *ai) {
    if (!ai || ai->sleep_target > 0) return;
    size_t n = ai->trace_n < 32 ? ai->trace_n : 32;
    if (n == 0) return;
    ai->sleep_cursor = 0;
    ai->sleep_target = n;
    ai->sleep_in_abstraction = 0;
}
static inline int ai_sleep_pending(const AI *ai) { return ai && ai->sleep_target > 0; }
static inline void ai_sleep_step(AI *ai) {
    if (!ai || ai->sleep_target == 0) return;
    if (ai->sleep_cursor < ai->sleep_target) {
        Token *t = ai->trace[ai->sleep_cursor++];
        if (t && !t->is_garbage) {
            ActivateCtx act;
            activate_ctx_init(&act, 0.9f, 0.1f, 6);
            act.max_visits = ai->sleep_max_visits ? ai->sleep_max_visits : AI_SLEEP_MAX_VISITS;
            token_activate_from(t, NULL, 0, 0.9f, &act);
            ai->n_sleep_visits += act.n_visits;
            ai->n_sleep_cuts   += act.n_cut;
            activate_ctx_free(&act);
        }
        return;
    }
    /* §6.253 **抽象分片**：以前这一步一次跑完（1412 词实测 ~200 ms，某一轮特别卡）。
     * 现在每轮只扫一段（约 1/8 词表），总结果与一次跑完相同（外层 i 升序 + 上限 3）。
     * 用 `sleep_in_abstraction` 当"进入抽象阶段"的标记，用 `abs_i` 当游标。 */
    if (!ai->sleep_in_abstraction) {
        ai->sleep_in_abstraction = 1;
        ai->abs_i = 0;
        ai->abs_made = 0;
    }
    {
        size_t n = ai->pool ? ai->pool->count : 0;
        size_t step = n / 8 + 1;
        size_t to = ai->abs_i + step;
        ai_form_abstractions_slice(ai, ai->abs_i, to, 3, &ai->abs_made);
        ai->abs_i = to;
        if (ai->abs_i < n && ai->abs_made < 3) return;   /* 还没扫完/没到上限 ⇒ 下一轮继续 */
    }
    ai->sleep_target = 0;
    ai->sleep_cursor = 0;
    ai->sleep_in_abstraction = 0;
    ai->trace_n = 0;
    ai->n_sleeps++;
    ai->inner.fatigue = 0.0f;
}
static inline void ai_sleep(AI *ai) {
    ai_sleep_begin(ai);
    while (ai_sleep_pending(ai)) ai_sleep_step(ai);
}

/* ============================ GC ============================ */

static inline int ai_gc_protected(const AI *ai, const Token *t) {
    if (!t) return 1;
    if (t == ai->self || t == ai->good || t == ai->bad ||
        t == ai->unknowing || t == ai->causal ||
        t == ai->social_good || t == ai->social_bad ||
        t == ai->past_self || t == ai->future_self) return 1;
    if (t->name && t->name[0] == '[') return 1;
    if (t->name && strcmp(t->name, TOKEN_UNKNOWING_NAME) == 0) return 1;
    return 0;
}
static inline int ai_gc_has_valence(const AI *ai, const Token *t) {
    if (!t) return 0;
    for (size_t i = 0; i < t->count; ++i) {
        Token *m = token_assocs_ro(t)[i].main;
        if (m == ai->good || m == ai->bad) return 1;
    }
    return 0;
}
/* §6.208 **O(E) 重建入度**（原来的 `ai_gc_indegree` 是 O(N·E)，GC 里每个候选词调一次）。
 * 等价性：原函数跳过 `is_garbage` 的来源；GC 的第一趟**不会**改任何词的 `is_garbage`
 * （标垃圾在第三趟），所以"GC 开头重建一次"与"每次现算"逐字节等价 —— A/B 钉着这一点。
 * 维护策略刻意选"用前重建"而不是"加边/删边时 ++/--"：后者要在 6+ 处写维护代码，
 * 漏一处就是**静默**的入度偏差（决策会变、还不报错）。 */
static inline void ai_gc_rebuild_indegree(TokenPool *p) {
    if (!p) return;
    for (size_t i = 0; i < p->count; ++i)
        if (p->tokens[i]) { p->tokens[i]->in_degree = 0; p->tokens[i]->rel_in_degree = 0; }
    for (size_t i = 0; i < p->count; ++i) {
        Token *from = p->tokens[i];
        if (!from || from->is_garbage) continue;
        for (size_t j = 0; j < from->count; ++j) {
            AssocEntry *e = &token_assocs(from)[j];
            if (!e->main) continue;
            e->main->in_degree++;
            if (edge_is_knowledge(e)) e->main->rel_in_degree++;   /* §6.588 */
        }
    }
}

static inline size_t ai_gc_indegree(const TokenPool *p, const Token *t) {
    if (!t) return 0;
    /* 校验钩子（默认关）：`AIP_INCHECK=1` 时现算一遍比对缓存值。 */
    static int incheck = -1;
    if (incheck < 0) { const char *v = getenv("AIP_INCHECK"); incheck = (v && v[0] == '1') ? 1 : 0; }
    if (!incheck) return t->in_degree;
    size_t n = 0;
    for (size_t i = 0; i < p->count; ++i) {
        const Token *from = p->tokens[i];
        if (!from || from->is_garbage) continue;
        for (size_t j = 0; j < from->count; ++j)
            if (token_assocs_ro(from)[j].main == t) n++;
    }
    if (n != t->in_degree)
        fprintf(stderr, "[incheck] %s: 缓存=%u 现算=%zu（不一致！）\n",
                t->name ? t->name : "?", t->in_degree, n);
    return t->in_degree;
}
static inline float ai_gc_keep_prob(const AI *ai, uint64_t age, uint64_t recency,
                                    uint32_t hits, int valence, size_t indegree) {
    if (age < ai->gc.gc_min_age) return 1.0f;
    float r = 1.0f / (1.0f + (float)recency / (float)ai->gc.gc_time_scale);
    float s = (float)(hits + 1) / (float)(hits + 3);
    float p = r * s;
    if (valence) p = p * 0.5f + 0.5f;
    if (indegree >= 5) p = p * 0.7f + 0.3f;
    return p > 1.0f ? 1.0f : p;
}
static inline float ai_gc_rand(AI *ai) {
    ai->gc_rng ^= ai->gc_rng << 13;
    ai->gc_rng ^= ai->gc_rng >> 17;
    ai->gc_rng ^= ai->gc_rng << 5;
    return (float)(ai->gc_rng & 0xFFFFFF) / (float)0x1000000;
}
static inline void ai_gc(AI *ai) {
    if (!ai || !ai->pool) return;
    TokenPool *p = ai->pool;
    /* §6.570 局部加载：GC 要**全图的入度**才能判"这条边还有没有人指向"。
     * 图有一部分在磁盘上时**宁可跳过这一轮**，也不要靠半张图去删东西
     * （少删一次不会有损；删错一次就真丢知识）。跳到 `::parts detach` 之后仍然成立。 */
    if (p->part_load) {
        ai->n_gc_skipped_partial++;
        if (getenv("AIP_PARTLOG")) fprintf(stderr, "[gc] 跳过：图有一部分在磁盘上（不做全图 GC）\n");
        return;
    }
    uint64_t now = p->current_tick;
    uint8_t del_state = ai->gc.gc_delete_state;
    float rate = ai->gc.gc_upgrade_rate;

    /* ⚠ §6.127 试过把「晋级态 / 挑出该埋的」并成一趟（省一趟全扫），**撤了**：
     * 两趟不是纯后缀 —— 第一趟里的 `ai_gc_indegree(p, t)` 会**跳过已经标成垃圾的词**，
     * 于是"边涨边埋"会让后面那些词算出的入度与原来不同（决策跟着变）。
     * 收益本来也可忽略（GC 每 100 轮才走一趟，约占每轮开销 1%），不值得拿等价性去换。 */
    ai_gc_rebuild_indegree(p);        /* §6.208：一次 O(E)，之后每个候选词 O(1) 读 */
    ai->n_scan_tokens += p->count;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t->is_garbage || t->mem_state >= del_state) continue;
        if (ai_gc_protected(ai, t)) continue;
        /* §6.588 **知识节点不许被"淡忘"掉**：它自己是某条关系的主语，或者被某条关系指着
         * （`麒麟 属于 神兽` 里的 神兽）。以前的 GC 只看 age/recency/hits ⇒ 用户教的事实
         * 在长会话里会被当成"不常用的共现"删掉（实测：60 轮 GC 之后 10 条教过的事实掉到 7 条）。 */
        if (t->rel_in_degree > 0 || token_has_knowledge_out(t)) continue;
        uint64_t age = now - t->birth_tick, rec = now - t->last_touch_tick;
        int val = ai_gc_has_valence(ai, t);
        size_t ind = (t->mem_state != MEM_ACTIVE) ? ai_gc_indegree(p, t) : 0;
        float keep = ai_gc_keep_prob(ai, age, rec, token_hits_total(t), val, ind);
        if (ai_gc_rand(ai) < (1.0f - keep) * rate) t->mem_state++;
    }
    ai->n_scan_tokens += p->count;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t->is_garbage) continue;
        for (size_t j = 0; j < t->count; ++j) {
            AssocEntry *e = &token_assocs(t)[j];
            ai->n_scan_edges++;
            if (e->mem_state >= del_state) continue;
            if (edge_is_knowledge(e)) continue;   /* §6.588 知识边不参与"淡忘"晋级 */
            uint64_t age = now - e->birth_tick, rec = now - e->last_touch_tick;
            int val = (e->main == ai->good || e->main == ai->bad);
            float keep = ai_gc_keep_prob(ai, age, rec, e->hits, val, 0);
            if (ai_gc_rand(ai) < (1.0f - keep) * rate) e->mem_state++;
        }
    }
    size_t deleted = 0;
    ai->n_scan_tokens += p->count;
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t->is_garbage || t->mem_state < del_state) continue;
        if (ai_gc_protected(ai, t)) continue;
        t->is_garbage = 1; deleted++;
    }
    ai->n_scan_tokens += p->count;   /* 清边那一趟同样逐词走 */
    for (size_t i = 0; i < p->count; ++i) {
        Token *t = p->tokens[i];
        if (t->is_garbage) {
            ai->n_gc_deleted_edges += t->count;
            t->count = 0;
            continue;
        }
        ai->n_scan_edges += t->count;
        for (size_t j = 0; j < t->count; ) {
            AssocEntry *e = &token_assocs(t)[j];
            int remove = 0;
            if (e->main && e->main->is_garbage) remove = 1;
            if (!remove && e->action && e->action->is_garbage) remove = 1;
            if (!remove && e->mem_state >= del_state) remove = 1;
            if (remove) {
                memmove(e, e + 1, (t->count - j - 1) * sizeof *e);
                t->count--; ai->n_gc_deleted_edges++;
            } else j++;
        }
    }
    ai->n_gc_deleted_tokens += deleted;
}

/* ============================ 世界模型 ============================ */

static inline void ai_set_last_output(AI *ai, Token **toks, size_t n) {
    if (!ai) return;
    if (n > ai->last_output_cap) {
        size_t ncap = n < 8 ? 8 : n * 2;
        Token **nt = (Token **)realloc(ai->last_output, ncap * sizeof *nt);
        if (!nt) return;
        ai->last_output = nt; ai->last_output_cap = ncap;
    }
    memcpy(ai->last_output, toks, n * sizeof *toks);
    ai->last_output_n = n;
}
/* 找一条**因果猜想**边：`from -[因果]→ to`（动作槽上挂 `[因果]` 标记的那条）。
 * 单独一个查询函数，是因为「学因果」和「削弱因果」必须按同一个判据找 ——
 * 削弱原来走 ai_weaken_edge（它只认"普通知识边"），于是**永远匹配不上**，
 * 反事实成了空转（见 ai_learn_counterfactual 的注释）。
 * v10 起判据在**动作槽**上（语境字段已删）：edge_is_causal_guess()。 */
static inline AssocEntry *ai_causal_edge(Token *from, Token *to, const Token *causal) {
    (void)causal;   /* 判据只有一处（按标记名），不再需要调用方传标记对象 */
    if (!from || !to) return NULL;
    /* §6.570 没有因果边的词**不必读它的边**（局部加载下这一步是省下的整段读取） */
    if (!from->has_causal) return NULL;
    for (size_t i = 0; i < from->count; ++i) {
        AssocEntry *e = &token_assocs(from)[i];
        if (e->main != to || !edge_is_causal_guess(e)) continue;
        return e;
    }
    return NULL;
}

/* 因果猜想：**一次巧合不算规律**（反迷信），反复协变才升格。
 *
 * 人脑原理（采用）：因果/预测关系来自**协变** —— X 之后 Y 反复出现才算数；
 * 而且**反例**（我没做 X，Y 照样发生）会削弱它。这是人真的在做的事，
 * 也是这个内核本来就想要的（`[因果]` 语境 + 反事实那一节）。
 * 糟粕（不学）：一次时序相邻就当成规律（post hoc ergo propter hoc），
 * 以及「出现一百次和出现一次没有区别」（从不更新这条猜想有多可信）。
 *
 * 三条护栏，保证「见得多」**不会**把它变成知识：
 *   ① 动作槽上永远挂着 `[因果]` 标记 → 关系层/知识查询（rel_direct_*）认不出它；
 *   ② 永远不参与走边（ai_edge_walkable 直接挡掉因果猜想边）；
 *   ③ 升格只写 hits/mem_state（参数层），**不建新边、不改结构**。 */
static inline void ai_learn_causal(AI *ai, char **words, size_t n) {
    if (!ai || ai->last_output_n == 0 || n == 0) return;
    for (size_t i = 0; i < ai->last_output_n; ++i) {
        Token *did = ai->last_output[i];
        if (!did || !did->name || did->name[0] == '[') continue;
        /* §6.570 局部加载：因果是**背景统计**，不是知识 ⇒ 边不在内存里的词**跳过**，
         * 不为它把整段读回来（实测：不跳过时一句问答会把 1265 段全读回）。
         * 代价照实写：被卸下的词这一轮不参与因果协变计数；读回来之后自然会再参与。 */
        if (!token_assocs_resident(did)) continue;
        for (size_t j = 0; j < n; ++j) {
            Token *observed = pool_token_intern(ai->pool, words[j]);
            if (observed == did) continue;
            if (!observed->name || observed->name[0] == '[') continue;
            AssocEntry *e = ai_causal_edge(did, observed, ai->causal);
            if (e) {
                /* 又见了一次：这是**协变证据**（不是新知识，也不新建边） */
                if (e->hits < AI_CAUSAL_HITS_CAP) e->hits++;
                if (e->hits >= AI_CAUSAL_MIN_HITS && e->mem_state == MEM_FADING) {
                    e->mem_state = MEM_ACTIVE;
                    ai->n_causal_promoted++;
                }
                continue;
            }
            /* v10：因果猜想边 = **动作槽上挂 `[因果]` 的边**（语境字段已删）。
             * 落边用 (main, action) 去重：普通观测边可以同时存在，两条互不覆盖。 */
            AssocEntry *ne = token_add_assoc_action_unique(did, observed, ai->causal);
            if (!ne) continue;
            did->has_causal = 1;            /* §6.570 缓存这一位：下次不必为"有没有"读边 */
            /* 一次巧合 = 暂时（会淡、可被反例雪藏） */
            ne->hits = 1;
            ne->mem_state = MEM_FADING;
            ai->n_causal_seen++;
        }
    }
}

/* 削弱一条因果猜想（**只动带 [因果] 标记的边**）：
 *   hits>0        → hits--（掉到 0 退回「暂时」）
 *   hits==0 暂时  → 雪藏（DORMANT：在「暂时」之上再来一个反例就不信了）
 * 返回 1 = 真的削弱了。计数器据此 +1 —— 以前不管削没削到都 +1，那是在**谎报**。 */
static inline int ai_weaken_causal(Token *from, Token *to, const Token *causal) {
    AssocEntry *e = ai_causal_edge(from, to, causal);
    if (!e) return 0;
    if (e->hits > 0) {
        e->hits--;
        if (e->hits == 0) e->mem_state = MEM_FADING;
        return 1;
    }
    if (e->mem_state == MEM_FADING) { e->mem_state = MEM_DORMANT; return 1; }
    return 0;
}

static inline void ai_learn_counterfactual(AI *ai, char **words, size_t n) {
    if (!ai || ai->last_skipped_n == 0 || n == 0) return;
    for (size_t i = 0; i < ai->last_skipped_n; ++i) {
        Token *didnt_do = ai->last_skipped[i];
        if (!didnt_do || didnt_do->is_garbage) continue;
        for (size_t j = 0; j < n; ++j) {
            Token *observed = pool_token_intern(ai->pool, words[j]);
            if (!observed || observed == didnt_do) continue;
            /* 「我没做 X，可 Y 还是发生了」⟹ X 不是 Y 的原因 —— 削弱那条猜想。
             * 踩过的坑：这里原来调 ai_weaken_edge，而它只认"普通知识边"，
             * 因果猜想边（当时靠语境字段标出来）→ **永远匹配不上**：
             * 反事实从来没生效过，计数器却每次 +1（机制在，行为不在）。 */
            if (ai_weaken_causal(didnt_do, observed, ai->causal))
                ai->n_counterfactual++;
        }
    }
    ai->last_skipped_n = 0;
}
static inline void ai_credit_from_feedback(AI *ai, float valence) {
    if (!ai) return;
    AIPath *p = &ai->last_path;
    if (p->n < 2) return;
    if (valence > 0.0f) {
        for (size_t i = 0; i + 1 < p->n; ++i)
            token_observe_pair(p->path[i], p->path[i + 1]);
    } else {
        float credit = -valence;
        for (size_t i = p->n; i > 1; --i) {
            ai_weaken_edge(p->path[i - 2], p->path[i - 1]);
            credit *= 0.7f;
            if (credit < 0.15f) break;
        }
    }
    ai->n_credit_events++;
}
static inline void ai_credit_from_surprise(AI *ai, float surprise) {
    if (!ai || surprise < 0.3f) return;
    ai_credit_from_feedback(ai, -surprise);
}

/* ============================ 他者 ============================ */

static inline OtherModel *ai_meet(AI *ai, const char *name) {
    if (!ai || !name) return NULL;
    for (size_t i = 0; i < ai->others_n; ++i)
        if (ai->others[i].who && ai->others[i].who->name &&
            strcmp(ai->others[i].who->name, name) == 0)
            return &ai->others[i];
    if (ai->others_n >= AI_MAX_OTHERS) return NULL;
    Token *who = pool_token_intern(ai->pool, name);
    OtherModel *om = &ai->others[ai->others_n++];
    om->who = who; om->belief_of_self = NULL;
    om->trust = 0.5f; om->n_interactions = 0;
    token_add_assoc_unique(ai->self, who);
    token_add_assoc_unique(who, ai->self);
    char bname[64];
    snprintf(bname, sizeof bname, "[%s:认为的我]", name);
    om->belief_of_self = pool_token_intern(ai->pool, bname);
    token_add_assoc_unique(who, om->belief_of_self);
    return om;
}
/* ---- 他者信念表：他说的「X R Y」存在**他**名下 ----
 * 这是「归属」从标注变成真东西的那一步。三条规矩：
 *   ① 他的信念**不进我的图**（跟假设同一条铁律）—— 他信什么不影响我知道什么；
 *   ② 问「他 觉得 … 吗」只查他的表（+ 归他的假设层），查不到就说不知道他怎么看，
 *      **不许**拿我的知识冒充他的想法（原来的实现就是拿我的图答他的问题）；
 *   ③ 他的信念写进模型文件（v8），存盘重载还在 —— 关系是持久的，归属也该持久。 */
static inline OtherModel *other_find(const AI *ai, const Token *who) {
    if (!ai || !who) return NULL;
    for (size_t i = 0; i < ai->others_n; ++i)
        if (ai->others[i].who == who) return (OtherModel *)&ai->others[i];
    return NULL;
}
static inline OtherModel *other_intern(AI *ai, Token *who) {
    OtherModel *om = other_find(ai, who);
    if (om || !who) return om;
    if (ai->others_n >= AI_MAX_OTHERS) return NULL;
    om = &ai->others[ai->others_n++];
    memset(om, 0, sizeof *om);
    om->who = who;
    om->trust = 0.5f;
    return om;
}
/* 记一条他的信念。同一对词、同一关系 → 覆盖极性（人会说反话）。 */
static inline int other_belief_learn(AI *ai, OtherModel *om,
                                     Token *a, Token *b, uint8_t rid, int neg) {
    if (!ai || !om || !a || !b || rid == 0 || a == b) return 0;
    /* **指代消解最小版**：谁刚说过话，就记下来 —— 「他/她/它」随后指的就是这个人
     * （只在 `pron_last` 打开时用；关着时这个字段没人读，行为逐字节相同）。 */
    if (om->who) ai->last_who = om->who;
    for (size_t i = 0; i < om->n_bel; ++i) {
        if (om->bel[i].a == a && om->bel[i].b == b && om->bel[i].rid == rid) {
            uint8_t nv = (uint8_t)(neg ? 1 : 0);
            /* **真改了**才记一笔（同一句话重复说 = 没改口 ⇒ 历史不该长） */
            if (om->bel[i].neg != nv) {
                BelHist *h = &om->hist[i];
                if (h->n < AI_MAX_BELIEF_REV) {
                    h->old_neg[h->n] = om->bel[i].neg;
                    h->tick[h->n] = (uint32_t)(ai->pool ? ai->pool->current_tick : 0);
                    h->n++;
                } else {                        /* 满了：滚最新（最初 2 次 + 最近 1 次） */
                    h->old_neg[AI_MAX_BELIEF_REV - 1] = om->bel[i].neg;
                    h->tick[AI_MAX_BELIEF_REV - 1] = (uint32_t)(ai->pool ? ai->pool->current_tick : 0);
                }
                h->total++;
            }
            om->bel[i].neg = nv;
            return 1;
        }
    }
    if (om->n_bel >= AI_MAX_BELIEF) return 0;
    HypoFact *f = &om->bel[om->n_bel];
    f->a = a; f->b = b; f->rid = rid; f->neg = (uint8_t)(neg ? 1 : 0);
    memset(&om->hist[om->n_bel], 0, sizeof om->hist[om->n_bel]);   /* 新信念：历史清零 */
    om->n_bel++;
    om->n_bel_learned++;
    return 1;
}
static inline const HypoFact *other_belief_find(const OtherModel *om,
                                                const Token *a, uint8_t rid,
                                                const Token *b) {
    if (!om || !a || !b || rid == 0) return NULL;
    for (size_t i = 0; i < om->n_bel; ++i)
        if (om->bel[i].a == a && om->bel[i].b == b && om->bel[i].rid == rid)
            return &om->bel[i];
    return NULL;
}
/* 以 a 为主语、关系 rid、极性 neg_want 的信念（他可能信好几条） */
static inline size_t other_belief_edges(const OtherModel *om, const Token *a,
                                        uint8_t rid, int neg_want,
                                        const HypoFact **out, size_t cap) {
    size_t n = 0;
    if (!om || !a) return 0;
    for (size_t i = 0; i < om->n_bel && n < cap; ++i) {
        const HypoFact *x = &om->bel[i];
        if (x->a != a || x->rid != rid) continue;
        if ((int)x->neg != (neg_want ? 1 : 0)) continue;
        out[n++] = x;
    }
    return n;
}
/* 他名下**有没有以 a 为主语**的说法 —— 用来区分「他这么想」和「他没说过这个」。
 * 只看主语：问「他 觉得 猫 属于 鸟 吗」时，他要是只说过「猫 会 飞」，
 * 那关于「猫 属于 鸟」他其实什么都没说，只能答「我没听他说过这个」。 */
static inline int other_has_subject(const OtherModel *om, const Token *a) {
    if (!om || !a) return 0;
    for (size_t i = 0; i < om->n_bel; ++i)
        if (om->bel[i].a == a) return 1;
    return 0;
}
static inline void ai_social_feedback(AI *ai, OtherModel *om, int feedback) {
    if (!ai || !om || feedback == 0) return;
    float target = (feedback > 0) ? 1.0f : 0.0f;
    om->trust = om->trust * 0.9f + target * 0.1f;
    om->n_interactions++;
    /* §6.299 无名节点**删掉**：原来这里造一个无名节点只为挂 `trust` 的两条边，
     * 没有任何消费者读它。`trust` / `n_interactions` 照常更新。 */
}
static inline OtherModel *ai_identify_speaker(AI *ai, char **words, size_t n) {
    if (!ai || !words || n == 0) return NULL;
    /* 「他/你/她 + 名字」才是在指出说话人。第一版只看前一个词，于是
     * `他 以为 猫 会 飞` 会给「以为」建一个「他者模型」、`他 说 …` 再给「说」
     * 建一个 —— 一次对话下来冒出好几个假人（实测）。所以后面那个词
     * **必须不像谓语**，否则不算人名。 */
    static const char *not_name[] = {
        "以为", "认为", "觉得", "说", "想", "知道", "是", "的", "了", "在", "有",
        "会", "能", "要", "属于", "包含", "喜欢", "爱", "吃", "怕", "不", "吗",
        "呢", "吧", "过", "着", "和", "跟", "与", NULL };
    for (size_t i = 0; i + 1 < n; ++i) {
        if (strcmp(words[i], "你") != 0 && strcmp(words[i], "他") != 0 &&
            strcmp(words[i], "她") != 0) continue;
        int bad = 0;
        for (size_t k = 0; not_name[k]; ++k)
            if (strcmp(words[i + 1], not_name[k]) == 0) { bad = 1; break; }
        if (bad) continue;
        return ai_meet(ai, words[i + 1]);
    }
    return NULL;
}

/* ============================ 时间 ============================ */

static inline void ai_timeline_advance(AI *ai) {
    if (!ai) return;
    char tname[64];
    snprintf(tname, sizeof tname, "[时刻:%llu]",
             (unsigned long long)ai->pool->current_tick);
    Token *now = pool_token_intern(ai->pool, tname);
    if (ai->timeline_n > 0) {
        size_t last = (ai->timeline_head + ai->timeline_n - 1) % AI_MAX_HISTORY;
        Token *prev = ai->timeline[last];
        if (prev && prev != now) {
            if (!prev->next_in_time) prev->next_in_time = now;
            Token *rel = pool_token_intern(ai->pool, "[关系:之后]");
            token_add_assoc_unique(prev, rel);
            token_add_assoc_unique(prev, now);
            token_touch(ai->past_self);
        }
    }
    ai->timeline[ai->timeline_head] = now;
    ai->timeline_head = (ai->timeline_head + 1) % AI_MAX_HISTORY;
    if (ai->timeline_n < AI_MAX_HISTORY) ai->timeline_n++;
    if (ai->focus) token_add_assoc_unique(now, ai->focus);
    token_add_assoc_unique(ai->past_self, now);
    token_add_assoc_unique(now, ai->self);
    token_add_assoc_unique(ai->future_self, ai->self);
}
static inline size_t ai_timeline_recent(const AI *ai, Token **out, size_t max) {
    if (!ai || ai->timeline_n == 0) return 0;
    size_t k = ai->timeline_n < max ? ai->timeline_n : max;
    for (size_t i = 0; i < k; ++i) {
        size_t idx = (ai->timeline_head + AI_MAX_HISTORY - 1 - i) % AI_MAX_HISTORY;
        out[i] = ai->timeline[idx];
    }
    return k;
}
static inline Token *ai_when_did_i(AI *ai, Token *what) {
    if (!ai || !what) return NULL;
    size_t k = ai->timeline_n;
    for (size_t i = 0; i < k; ++i) {
        size_t idx = (ai->timeline_head + AI_MAX_HISTORY - 1 - i) % AI_MAX_HISTORY;
        Token *t = ai->timeline[idx];
        if (!t) continue;
        for (size_t j = 0; j < t->count; ++j)
            if (token_assocs(t)[j].main == what) return t;
    }
    return NULL;
}

/* ============================ 想象 ============================ */

/* 前向声明：`ai_write_assoc` 定义在后面（它要 `micro_of` → 必须等 micro.h 进来）——
 * 但"想象/内部对话"这些**自撰写入**要用它。风格同 think.h 里那些前向声明。 */
static inline int ai_write_assoc(AI *ai, Token *a, Token *b, AiWriteSrc s);

static inline void ai_imagine(AI *ai) {
    if (!ai || ai->focus_n < 2) return;
    float ec = ai_comfort_error(ai), ea = ai_arousal_error(ai);
    float dev = ec * ec + ea * ea;
    float rate = ai->imagine_rate * (1.0f + dev * 5.0f);
    if (rate > 0.5f) rate = 0.5f;
    ai->rng ^= ai->rng << 13; ai->rng ^= ai->rng >> 17; ai->rng ^= ai->rng << 5;
    float roll = (float)(ai->rng & 0xFFFFFF) / (float)0x1000000;
    if (roll > rate) return;

    size_t n = ai->focus_n;
    size_t i = (ai->rng >> 8) % n;
    size_t j = (ai->rng >> 16) % n;
    if (i == j) return;
    Token *a = ai->focus_ring[i];
    Token *b = ai->focus_ring[j];
    if (!a || !b || a->is_garbage || b->is_garbage) return;

    /* §6.299 **想象的载体换了**（外部评审拍板的方案 1）：不造节点、不绑向量，
     * 直接把两个焦点词**连起来**，边上标 `ASSOC_ROLE_IMAGINED`。
     * 好处：零新字段（role 字节本来就在存档里）、边可存盘可衰减、
     * 一眼看得出"这条不是听到的，是它自己想出来的"。
     * 也不设 `rel` 位 ⇒ 不污染关系知识（rel_direct 看不到它）。 */
    (void)ai_write_assoc(ai, a, b, AI_WSRC_IMAGINE);
    (void)ai_write_assoc(ai, b, a, AI_WSRC_IMAGINE);
    {
        AssocEntry *e = token_find_assoc_action(a, b, NULL);
        if (e) e->role = ASSOC_ROLE_IMAGINED;
        e = token_find_assoc_action(b, a, NULL);
        if (e) e->role = ASSOC_ROLE_IMAGINED;
    }
    ai->n_imaginations++;
}

/* ============================ 类比 ============================ */

/* 关系类比：A : B :: C : ?
 *
 * **为什么不用 bind/unbind 了**：那个版本走的是 hv（名字的随机投影），
 * 于是 unbind 出来的「关系向量」和答案都是随机的，最近邻查出来的东西没有意义 ——
 * 这就是它一直是死代码的原因（没人敢调）。
 *
 * 改成在**学出来的 emb 空间**里做向量算术（word2vec 那套）：
 *
 *     answer ≈ emb(C) + (emb(B) − emb(A))
 *
 * emb 是从 PPMI 邻居投影出来的，所以这个偏移方向真的对应「A 到 B 的那种关系」。
 * 实测 `猫 动物 狗` 能落在「同类词」上（见 ::analogy）。
 *
 * 返回值是最近的那个真实词（不新建符号节点），相似度写进 out_sim。 */
static inline Token *ai_analogy(AI *ai, Token *item_a, Token *pair_ab,
                               Token *item_c, float *out_sim) {
    if (!ai || !item_a || !pair_ab || !item_c) return NULL;
    if (!item_a->emb || !pair_ab->emb || !item_c->emb) return NULL;
    /* 查询向量用 float 累加（emb 是 int8，直接相加会溢出） */
    int16_t q[EMB_DIM];
    for (size_t d = 0; d < EMB_DIM; ++d) {
        int v = (int)item_c->emb[d] + (int)pair_ab->emb[d] - (int)item_a->emb[d];
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        q[d] = (int16_t)v;
    }
    Token *best = NULL;
    double best_sim = -2.0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t->emb || t->is_garbage) continue;
        if (!t->name || t->name[0] == '[') continue;
        if (t == item_a || t == pair_ab || t == item_c) continue;
        double dot = 0, nq = 0, nt = 0;
        for (size_t d = 0; d < EMB_DIM; ++d) {
            dot += (double)q[d] * (double)t->emb[d];
            nq  += (double)q[d] * (double)q[d];
            nt  += (double)t->emb[d] * (double)t->emb[d];
        }
        if (nq == 0 || nt == 0) continue;
        double c = dot / (sqrt(nq) * sqrt(nt));
        if (c > best_sim) { best_sim = c; best = t; }
    }
    if (out_sim) *out_sim = best ? (float)best_sim : 0.0f;
    return best;
}

/* 按 emb 找最近的词（语义邻居，不要求有边） */
static inline Token *ai_nearest_emb(AI *ai, const Token *t, float *out_sim) {
    if (!ai || !t || !t->emb) return NULL;
    Token *best = NULL;
    double best_sim = -2.0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *u = ai->pool->tokens[i];
        if (!u->emb || u->is_garbage || u == t) continue;
        if (!u->name || u->name[0] == '[') continue;
        double c = (double)emb_cosine(t->emb, u->emb);
        if (c > best_sim) { best_sim = c; best = u; }
    }
    if (out_sim) *out_sim = best ? (float)best_sim : 0.0f;
    return best;
}

/* ============================ 内部对话 ============================ */

static inline void ai_inner_dialogue(AI *ai) {
    if (!ai || ai->focus_n < 2) return;
    for (size_t i = 0; i < ai->focus_n; ++i) {
        Token *ti = ai->focus_ring[i];
        if (!ti) continue;
        for (size_t j = 0; j < ai->focus_n; ++j) {
            if (i == j) continue;
            Token *tj = ai->focus_ring[j];
            if (!tj) continue;
            for (size_t k = 0; k < ti->count; ++k) {
                if (token_assocs(ti)[k].main == tj) {
                    ai->focus_weight[j] += ai->inner_dialogue_gain *
                                           (float)(token_assocs(ti)[k].hits + 1);
                    if (ai->focus_weight[j] > 3.0f) ai->focus_weight[j] = 3.0f;
                    break;
                }
            }
        }
    }
    ai->n_inner_dialogues++;
}

/* ============================ 求值层（样板：数值比较） ============================
 *
 * ② 档第一件：**"条件"要有真判据**。语言层现在所有判据都是"图内判据"
 * （提到过/够得着/有据）—— 数值比较这类**可计算**的东西判不了。
 * 这一层只做一个样板谓语，配方写清楚，后面加别的谓语照抄：
 *   ① 认出形状（这里是 `<数字> 算符 <数字>`）② 真算一遍 ③ 算得出来就用它，
 *   算不出来就**明说判不了**（回退到图内判据，绝不默认成立）。
 * 默认关（`AIP_EVALNUM=1` / `::proc eval on`）：关着时行为与今天逐字节相同。 */
static inline int ai_eval_parse_num(const char *s, long *out) {
    if (!s || !*s || !out) return 0;
    char buf[32];
    size_t k = 0;
    const char *p2 = s;
    if (*p2 == '-' || *p2 == '+') buf[k++] = *p2++;
    while (*p2 && k + 1 < sizeof buf) {
        if (*p2 == ' ') { p2++; continue; }
        if (*p2 < '0' || *p2 > '9') break;
        buf[k++] = *p2++;
    }
    while (*p2 == ' ') p2++;
    if (*p2) return 0;                    /* 后面还有别的东西 ⇒ 不是纯数字 */
    if (k == 0 || (k == 1 && (buf[0] == '-' || buf[0] == '+'))) return 0;
    buf[k] = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (!end || *end) return 0;
    *out = v;
    return 1;
}

/* 认 `<数字> <算符> <数字>` 并真算一遍。返回 1 = 算出来了（*out 是真值），0 = 判不了。 */
/* §6.142 **算术表达式**（默认关的求值层里那一步"浮点/算术"）：只认 数、+ - * / 和括号。
 * 认不出就返回 0（判不了）—— 求值层的老规矩：**绝不默认成立**。
 * 有意**不认**科学计数法（`1e3` 仍旧判不了，看护里钉着）—— 那会把「1e3」这种
 * 图上真出现过的名字当成数，形状判断反而变糊。 */
typedef struct { const char *p; int ok; } AiEvalCur;
static inline void ai_eval_skip(AiEvalCur *c) { while (*c->p == ' ') c->p++; }
static inline double ai_eval_expr(AiEvalCur *c);
static inline double ai_eval_factor(AiEvalCur *c) {
    ai_eval_skip(c);
    int neg = 0;
    while (*c->p == '+' || *c->p == '-') { if (*c->p == '-') neg = !neg; c->p++; ai_eval_skip(c); }
    if (*c->p == '(') {
        c->p++;
        double v = ai_eval_expr(c);
        ai_eval_skip(c);
        if (*c->p != ')') { c->ok = 0; return 0; }
        c->p++;
        return neg ? -v : v;
    }
    const char *st = c->p;
    int any = 0;
    while (*c->p >= '0' && *c->p <= '9') { c->p++; any = 1; }
    if (*c->p == '.') { c->p++; while (*c->p >= '0' && *c->p <= '9') { c->p++; any = 1; } }
    if (!any) { c->ok = 0; return 0; }
    size_t n = (size_t)(c->p - st);
    char buf[40];
    if (n >= sizeof buf) { c->ok = 0; return 0; }
    memcpy(buf, st, n); buf[n] = 0;
    double v = strtod(buf, NULL);
    return neg ? -v : v;
}
static inline double ai_eval_term(AiEvalCur *c) {
    double v = ai_eval_factor(c);
    for (;;) {
        ai_eval_skip(c);
        char op = *c->p;
        if (op != '*' && op != '/') break;
        c->p++;
        double r = ai_eval_factor(c);
        if (!c->ok) return 0;
        if (op == '*') v *= r;
        else { if (r == 0.0) { c->ok = 0; return 0; } v /= r; }   /* 除以 0 ⇒ 判不了 */
    }
    return v;
}
static inline double ai_eval_expr(AiEvalCur *c) {
    double v = ai_eval_term(c);
    for (;;) {
        ai_eval_skip(c);
        char op = *c->p;
        if (op != '+' && op != '-') break;
        c->p++;
        double r = ai_eval_term(c);
        if (!c->ok) return 0;
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}
static inline int ai_eval_parse_expr(const char *s, double *out) {
    if (!s || !*s || !out) return 0;
    AiEvalCur c = { s, 1 };
    double v = ai_eval_expr(&c);
    ai_eval_skip(&c);
    if (!c.ok || *c.p) return 0;          /* 尾巴还有东西 ⇒ 不是纯表达式 */
    *out = v;
    return 1;
}

static inline int ai_eval_cmp(const char *expr, int *out) {
    if (!expr || !expr[0] || !out) return 0;
    static const char *const ops[] = { "<=", ">=", "==", "!=", "<", ">", NULL };
    for (size_t i = 0; ops[i]; ++i) {
        const char *at = strstr(expr, ops[i]);
        if (!at) continue;
        size_t l = (size_t)(at - expr);
        if (l == 0 || l >= 48) continue;
        char lhs[48];
        memcpy(lhs, expr, l); lhs[l] = 0;
        const char *rhs = at + strlen(ops[i]);
        /* 两边**都是纯整数**时走整数那条路（逐位精确，大整数也不会被 double 吃掉）；
         * 否则走 §6.142 的算术表达式（浮点 + + - * / 括号）。两条都认不出 ⇒ 判不了。 */
        long ia = 0, ib = 0;
        double da = 0, db = 0;
        int is_int = 0;
        if (ai_eval_parse_num(lhs, &ia) && ai_eval_parse_num(rhs, &ib)) {
            is_int = 1; da = (double)ia; db = (double)ib;
        } else if (ai_eval_parse_expr(lhs, &da) && ai_eval_parse_expr(rhs, &db)) {
            is_int = 0;
        } else {
            continue;
        }
        if (strcmp(ops[i], "<")  == 0) *out = is_int ? (ia <  ib) : (da <  db);
        else if (strcmp(ops[i], ">")  == 0) *out = is_int ? (ia >  ib) : (da >  db);
        else if (strcmp(ops[i], "<=") == 0) *out = is_int ? (ia <= ib) : (da <= db);
        else if (strcmp(ops[i], ">=") == 0) *out = is_int ? (ia >= ib) : (da >= db);
        else {
            /* 浮点相等用**相对容差**（判的是"这个说法成立吗"，不是逐位相等）：
             * 于是 `0.1+0.2==0.3` 成立（IEEE 下逐位比会得到假）。 */
            int eq = is_int ? (ia == ib)
                            : (fabs(da - db) <= 1e-9 * (1.0 + fabs(da) + fabs(db)));
            *out = (strcmp(ops[i], "==") == 0) ? eq : !eq;
        }
        return 1;
    }
    return 0;                             /* 认不出形状 ⇒ 判不了（绝不默认成立） */
}

/* ============================ 漂移（§6.299 删除） ============================
 * 原来比较 hv 与 hv_prev 的余弦来量"漂移"。实测 1198/1198 个符号节点的两份向量
 * 完全相同（hv 建成后只有 memcpy）⇒ 漂移恒 0，是假信号；唯一非零来自想象造的
 * 无名节点（被 vsa_bind 覆盖）。hv 删掉后它没有载体，**整个采样器删除**。
 * `drift_sample_every` / `n_drifts_detected` 两个字段仍在存档里（为了 v13 兼容
 * 读取不重排），但已无写入方 —— 当作历史残留，`::drift` 命令一并删除。 */

/* ============================ 叙事 ============================ */

/* §6.141 跑一次叙事并**落地**（写进图 + 记账 + 留最近一次给 `::narrate` 看）。
 * 周期性那条（`agent_step` 维护段）与 `::narrate now` 共用这一处 —— 一份判据两处用。 */
static inline int ai_narrate_apply(AI *ai);

static inline char *ai_narrate(AI *ai, size_t n_recent) {
    if (!ai || ai->timeline_n == 0) return NULL;
    size_t cap = 512, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;
    size_t k = ai->timeline_n < n_recent ? ai->timeline_n : n_recent;
    int first = 1;
    for (size_t i = 0; i < k; ++i) {
        size_t idx = (ai->timeline_head + AI_MAX_HISTORY - 1 - i) % AI_MAX_HISTORY;
        Token *t = ai->timeline[idx];
        if (!t) continue;
        Token *focus = NULL;
        uint32_t best_h = 0;
        for (size_t j = 0; j < t->count; ++j) {
            Token *m = token_assocs(t)[j].main;
            if (!m || !m->name || m->name[0] == '[') continue;
            /* §6.141 **别把叙事说成"我最近在想 我"**：每个 `[时刻:]` 都挂着「我」（自指边），
             * 而它命中数最高 ⇒ 以前叙事几乎永远只输出「我」。跳过自指与自环，
             * 让叙事说**它那一刻在想什么**（焦点词）。 */
            if (m == ai->self || m == t) continue;
            uint32_t h = token_assocs(t)[j].hits;
            if (h >= best_h) { best_h = h; focus = m; }
        }
        if (!focus) continue;
        const char *sep = first ? "我最近在想 " : "，然后 ";
        size_t sl = strlen(sep), nl = strlen(focus->name);
        while (len + sl + nl + 2 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        memcpy(buf + len, sep, sl); len += sl;
        memcpy(buf + len, focus->name, nl); len += nl;
        buf[len] = 0;
        first = 0;
    }
    if (!first) {
        while (len + 4 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
        strcat(buf, "。");
    }
    return buf;
}

/* §6.141 跑一次叙事并落地（写进图 + 记账 + 留最近一次）。返回 1 = 真的说了一段。 */
static inline int ai_narrate_apply(AI *ai) {
    if (!ai) return 0;
    char *narr = ai_narrate(ai, 5);
    if (!narr) { ai->narr_last[0] = 0; return 0; }
    snprintf(ai->narr_last, sizeof ai->narr_last, "%s", narr);   /* 有界拷贝（别用 strcpy）*/
    ai->narr_last_turn = ai->turns;
    size_t nn = 0;
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
    return 1;
}

/* ============================ 诊断 ============================ */

static inline float ai_lexical_diversity(AI *ai) {
    if (!ai || ai->trace_n == 0) return 1.0f;
    size_t k = ai->trace_n < 64 ? ai->trace_n : 64;
    size_t distinct = 0;
    for (size_t i = 0; i < k; ++i) {
        Token *t = ai->trace[ai->trace_n - 1 - i];
        if (!t) continue;
        int seen = 0;
        for (size_t j = 0; j < i; ++j)
            if (ai->trace[ai->trace_n - 1 - j] == t) { seen = 1; break; }
        if (!seen) distinct++;
    }
    return (float)distinct / (float)k;
}
static inline void ai_diagnose(AI *ai) {
    if (!ai) return;
    float div = ai_lexical_diversity(ai);
    size_t p_n = ai->pool->perspective_n;
    size_t abs_n = ai->n_abstractions;
    size_t imag_n = ai->n_imaginations;   /* `n_imagine_useful` 恒 0（全仓没有 ++），不再参与判定 */

    if (ai->diag.prev_lexical_diversity > 0 &&
        div < ai->diag.prev_lexical_diversity - 0.15f) {
        Token *d = pool_token_intern(ai->pool, "[诊断:自举退化]");
        if (d) token_add_assoc_unique(ai->self, d);
        ai->n_diagnoses++;
    }
    if (p_n > ai->diag.prev_perspective_n + 50) {
        Token *d = pool_token_intern(ai->pool, "[诊断:视角爆炸]");
        if (d) token_add_assoc_unique(ai->self, d);
        ai->n_diagnoses++;
    }
    if (abs_n > ai->diag.prev_abstractions + 10) {
        Token *d = pool_token_intern(ai->pool, "[诊断:抽象过度]");
        if (d) token_add_assoc_unique(ai->self, d);
        ai->n_diagnoses++;
    }
    /* `n_imagine_useful` 恒 0（全仓没有 ++）⇒ 原来这个条件等价于"想象超过 20 次就报噪声"。
     * 现在只剩前半条，语义仍是"想象次数异常增长"（§6.299 指出那个计数器是摆设）。 */
    if (imag_n > ai->diag.prev_imaginations + 20) {
        Token *d = pool_token_intern(ai->pool, "[诊断:想象噪声]");
        if (d) token_add_assoc_unique(ai->self, d);
        ai->n_diagnoses++;
    }
    ai->diag.prev_lexical_diversity = div;
    ai->diag.prev_perspective_n = p_n;
    ai->diag.prev_abstractions = abs_n;
    ai->diag.prev_imaginations = imag_n;
    ai->diag.last_diag_tick = ai->pool->current_tick;
}

/* ============================ 校准 ============================ */

static inline void ai_calibrate(AI *ai, int feedback) {
    if (!ai || feedback == 0) return;
    if (ai->last_confidence_at_output > 0.8f) {
        ai->calib.n_confident++;
        if (feedback < 0) ai->calib.n_confident_wrong++;
    }
    if (ai->calib.n_confident > 0) {
        float wrong_rate = (float)ai->calib.n_confident_wrong
                         / (float)ai->calib.n_confident;
        ai->calib.calibration_bias = wrong_rate - 0.2f;
    }
    if (ai->calib.n_confident > 20) {
        if (ai->calib.calibration_bias > 0.15f) {
            ai->uncertain.certain_threshold += 0.01f;
            if (ai->uncertain.certain_threshold > 0.90f)
                ai->uncertain.certain_threshold = 0.90f;
        } else if (ai->calib.calibration_bias < -0.10f) {
            ai->uncertain.certain_threshold -= 0.01f;
            if (ai->uncertain.certain_threshold < 0.50f)
                ai->uncertain.certain_threshold = 0.50f;
        }
    }
}

/* ============================ 统计 ============================ */

static inline void ai_stats(const AI *ai) {
    /* 来源档可信度（Beta 后验，见 ai_source_posterior）：**学出来的**，不是写死的档 */
    if (ai) {
        printf("       来源档可信度（后验）：基准 %.2f 观测 %.2f 推论 %.2f（改口 %zu 次 / 缓存撤 %zu 次）\n",
               ai_source_posterior(ai, ASSOC_ROLE_INNATE),
               ai_source_posterior(ai, ASSOC_ROLE_NONE),
               ai_source_posterior(ai, ASSOC_ROLE_DERIVED),
               ai->n_revised, ai->n_retracted);
    }
    if (!ai) return;
    size_t edges = 0, garbage = 0;
    size_t active = 0, fading = 0, dormant = 0, forgotten = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (t->is_garbage) { garbage++; continue; }
        edges += t->count;
        switch (t->mem_state) {
        case MEM_ACTIVE: active++; break;
        case MEM_FADING: fading++; break;
        case MEM_DORMANT: dormant++; break;
        case MEM_FORGOTTEN: forgotten++; break;
        }
    }
    printf("[统计] 符号节点=%zu (活跃%zu 淡化%zu 休眠%zu 遗忘%zu 垃圾%zu)  边=%zu\n",
           ai->pool->count, active, fading, dormant, forgotten, garbage, edges);
    printf("       已学=%zu  轮次=%zu  已删符号节点=%zu  已删边=%zu\n",
           ai->n_learned, ai->turns,
           ai->n_gc_deleted_tokens, ai->n_gc_deleted_edges);
    printf("       惊讶=%zu  抽象=%zu  睡眠=%zu  轨迹=%zu  睡眠中=%d  睡眠激活访问=%zu 截断=%zu（上限 %d）\n",
           ai->n_surprises, ai->n_abstractions, ai->n_sleeps,
           ai->trace_n, ai_sleep_pending(ai),
           ai->n_sleep_visits, ai->n_sleep_cuts, AI_SLEEP_MAX_VISITS);
    printf("       信用=%zu  表达自举=%zu  反事实=%zu  因果=%zu(升格%zu)\n",
           ai->n_credit_events, ai->n_expr_learned, ai->n_counterfactual,
           ai->n_causal_seen, ai->n_causal_promoted);
    printf("       舒适=%.2f 唤醒=%.2f 疲劳=%.2f 候选=%d 置信=%.2f 知道感=%.2f\n",
           ai->inner.comfort, ai->inner.arousal, ai->inner.fatigue,
           ai_effective_candidates(ai), ai->last_confidence, ai->meta.total);
    printf("       帧生成=%zu (关联链放行%zu 现场造词%zu 拆解%zu 合并%zu 中转%zu 降级%zu)\n",
           ai->n_frames, ai->n_frame_assoc, ai->n_coined,
           ai->n_split, ai->n_merge, ai->n_relay, ai->n_hedge);
    printf("       思维链步数=%zu\n", ai->n_cot);
    printf("       关系知识：现场学到 %zu 条（其中改口 %zu 次：后说的观测替掉了先说的）  "
           "推理答是 %zu / 答不是 %zu / 答不确定 %zu（多跳 %zu 类比 %zu）\n",
           ai->n_rel_learned, ai->n_revised, ai->n_rel_yes, ai->n_rel_no, ai->n_rel_unknown,
           ai->n_rel_deep, ai->n_rel_guess);
    printf("       动态关系 %zu 个\n", ai->dynrel_n);
    {
        size_t nh = 0;
        for (size_t i = 0; i < ai->hypo.depth; ++i) nh += ai->hypo.fr[i].n;
        printf("       假设层：累计设过 %zu 次  当前 %zu 层 / %zu 条%s\n",
               ai->n_hypo, ai->hypo.depth, nh,
               ai->hypo.depth ? "（只在推理时用，未写进知识库）" : "");
    }
    printf("       世界假设：%s\n", ai->closed_world ? "闭合（没写就是假）" : "开放（没写就是不知道）");
    {
        printf("       关系独占表：");
        for (uint8_t r = 1; r <= REL_CORE_MAX; ++r) {
            const char *nm = NULL;
            switch (r) {
                case REL_IS_A: nm = "属于"; break;
                case REL_HAS:  nm = "有";   break;
                case REL_CAN:  nm = "会";   break;
                case REL_FEAR: nm = "怕";   break;
                case REL_LIKE: nm = "喜欢"; break;
                case REL_EAT:  nm = "吃";   break;
                case REL_AT:   nm = "在";   break;
                case REL_NEED: nm = "需要"; break;
                default: break;
            }
            if (!nm) continue;
            /* 带上证据：单宾语/多宾语的主语数。判独占的完整数字在 ::excl 里。 */
            printf("%s=%s(单%zu/多%zu) ", nm, ai->rel_excl[r] ? "独占" : "多值",
                   ai->rel_excl_single[r], ai->rel_excl_multi[r]);
        }
        printf("（有数据支持的 %zu 项，其中改口 %zu 项；参照多宾语率 %.1f%%）\n",
               ai->n_rel_excl_evidenced, ai->n_rel_excl_learned,
               ai->rel_excl_base / 10.0);
    }
    printf("       逐词游走=%zu\n", ai->n_walks);
    printf("       他者=%zu  时间点=%zu  视角深度=%zu\n",
           ai->others_n, ai->timeline_n, perspective_depth(ai->pool));
    printf("       想象=%zu (有用%zu)  内部对话=%zu  叙事=%zu\n",
           ai->n_imaginations, ai->n_imagine_useful,
           ai->n_inner_dialogues, ai->n_narrations);
    printf("       诊断=%zu  漂移=%zu  校准偏差=%.2f  置信门槛=%.2f\n",
           ai->n_diagnoses, ai->n_drifts_detected,
           ai->calib.calibration_bias, ai->uncertain.certain_threshold);
}

/* 微内核（安全内核 + 自修改意图队列 + 补丁注册表 + 元认知）。
 * 放在最后包含：它要用完整的 AI 结构，也要用上面这些函数
 * （ai_tokenize / ai_focus_push / ai_free_words）。 */
/* §6.564 记一笔"我改了什么"。有界环形：满了覆盖最旧的一条（`n` 继续涨，能看出溢出）。 */
/* 记一条账（只给 id）。§6.580：**冲突但没写**的那种账，宾语可能在本脑里还没有词
 * （判定冲突时我们刻意不建词）⇒ 这里允许 `b_id == 0`，`::mods` 显示成 "?"。
 * 有 id 这一层，账就不依赖"两端都是本池里活着的词"。 */
static inline void ai_modlog_note_raw(AI *ai, uint32_t a_id, uint32_t b_id, uint8_t rid, int neg,
                                      AiWriteSrc s, AIModKind kind) {
    if (!ai || !a_id) return;
    AIModLog *L = &ai->modlog;
    AIModEntry *e = &L->ent[L->head];
    L->head = (L->head + 1u) % AI_MODLOG_MAX;
    L->n++;
    e->a_id = a_id;
    e->b_id = b_id;
    e->seq  = (uint32_t)L->n;
    e->rid  = rid;
    e->neg  = (uint8_t)(neg ? 1 : 0);
    e->src  = (uint8_t)((unsigned)s < AI_WSRC_N ? (unsigned)s : 0u);
    e->kind = (uint8_t)kind;
}
static inline void ai_modlog_note(AI *ai, Token *a, Token *b, uint8_t rid, int neg,
                                  AiWriteSrc s, AIModKind kind) {
    if (!ai || !a || !b) return;
    ai_modlog_note_raw(ai, a->id, b->id, rid, neg, s, kind);
}
static inline size_t ai_modlog_count(const AI *ai) { return ai ? ai->modlog.n : 0; }
/* 第 i 条（0 = 最新）。越界返回 NULL。 */
static inline const AIModEntry *ai_modlog_at(const AI *ai, size_t i) {
    if (!ai || i >= ai->modlog.n) return NULL;
    size_t n = ai->modlog.n < AI_MODLOG_MAX ? ai->modlog.n : AI_MODLOG_MAX;
    if (i >= n) return NULL;                     /* 更老的已经被覆盖 */
    size_t slot = (ai->modlog.head + AI_MODLOG_MAX - 1u - i) % AI_MODLOG_MAX;
    return &ai->modlog.ent[slot];
}

/* §6.568 记"删掉了一条边"（B 腿：账里不能只有"加"，没有"删/改"） */
static inline void ai_modlog_note_del(AI *ai, Token *a, Token *b, AiWriteSrc s, AIModKind kind) {
    ai_modlog_note(ai, a, b, 0, 0, s, kind);
}
/* §6.585 撤掉一条**关系位**（不是删整条边）：要带上关系 id，否则 `::mods` 只能显示 "?" */
static inline void ai_modlog_note_retract(AI *ai, Token *a, Token *b, uint8_t rid, AiWriteSrc s) {
    ai_modlog_note(ai, a, b, rid, 0, s, AIMOD_REL_DEL);
}


#include "micro.h"

/* ============================ 统一写入通道 ============================
 *
 * `microkernel.md` 一直记着一条局限：**老写入者绕过安全内核**
 * （感知学习、内部对话、想象、关系层现场学都直接改图）。这一轮把它变成
 * "**有账 + 可路由**"：
 *
 *   ① **有账**：谁写的都从这两个口子过，`ai->writes[来源]++`；
 *   ② **可路由**：`ai->route_writes` 打开时，**知识类**写入（关系事实）
 *      改成提意图 → 同一条「审查 → 预算 → 应用 → 审计」的路；
 *   ③ **刻意不路由的**：感知共现（`ai_learn_pair`）。它带 hits/pattern/topic
 *      这些观测细节，转成意图会**丢信息**；而且观测是知识的来源，
 *      直写本来就是它的正常路径。这条区别写在 `::safe` 里，不藏着。
 *
 * 两个口子都必须**在 micro.h 之后**（要用 micro_of / micro_try）。 */
static inline void ai_write_count(AI *ai, AiWriteSrc s) {
    if (ai && (unsigned)s < AI_WSRC_N) ai->writes[s]++;
}

/* 共现边（**非观测**那一侧）：记一笔；`route_writes` 打开时走**同一套**受审通道（K6 补齐）。
 *
 * 谁走这里：`[时刻:]` 时间链、内部对话、想象 —— 它们都是"机器自己写下来的"，
 * 不是观测。谁**不**走这里：感知共现（`ai_learn_pair`）**保持直写** ——
 * 它带 hits/pattern/topic 这些观测细节，转成意图会丢信息；而且观测是知识的来源，
 * 直写本来就是它的正常路径（§6.55 K6 的决定，`::safe` 里也这么写着）。
 *
 * 两条路的语义相同（`MOD_ADD_ASSOC` 内部就是 `token_add_assoc_unique`），差别只在
 * 谁来审查、什么时候落地、有没有账 —— 所以这是**可测量**的开关，不是改行为。
 * 返回值的口径两条路一致：1 = 新加了一条边 / 0 = 边已经有了 / -1 = 被受审通道拒了。 */
static inline int ai_write_assoc(AI *ai, Token *a, Token *b, AiWriteSrc s) {
    if (!ai || !a || !b) return 0;
    ai_write_count(ai, s);
    /* `route_writes` 关着（默认）时**一个字都不多做** —— 与今天逐字节相同。
     * 打开时才去碰微内核状态，并且先看那条**静态例外**（见上面那段注释）。 */
    if (ai->route_writes) {
        Micro *mk = micro_of(ai);
        /* §6.175 **无名端点的边不走受审通道**：没有名字的符号节点是"临时叠加"
         * （`ai_imagine` 用 `pool_token(pool,NULL,NULL)` 造的），它**不会存盘**
         * （`mio_edge_saved` 跳过无名端点）⇒ 进审查只是白记预算/限额。
         * 实测症状：`我` 的边数 674 > 上限 512 之后，每轮都多一条
         * 「加边 我→(无名) 拒绝：边数触顶」—— 被拒的是本来就不存盘的东西，
         * 却把"自我修改被拒"的噪声堆进 `::safe`。 */
        int named = (a->name && a->name[0]) && (b->name && b->name[0]);
        if (named && mk && !mk->safe.mod_off &&
            !micro_endpoint_reserved(a) && !micro_endpoint_reserved(b)) {
            int had = micro_has_edge(a, b);  /* 口径要和直写那条路一致（1=新加 / 0=已有） */
            ModIntent m;
            memset(&m, 0, sizeof m);
            m.type = MOD_ADD_ASSOC;
            m.target = a; m.main = b;
            {   /* source 只有 24 字节：有界拼接（截断是故意的，同 ai_write_rel） */
                size_t off = 0;
                const char *head = "统一通道:";
                for (const char *q = head; *q && off + 1 < sizeof m.source; ++q) m.source[off++] = *q;
                for (const char *q = ai_write_src_name(s); *q && off + 1 < sizeof m.source; ++q)
                    m.source[off++] = *q;
                m.source[off] = 0;
            }
            char note[96];
            if (micro_try(ai, &m, note, sizeof note) != SAFE_OK) return -1;
            return had ? 0 : 1;
        }
    }
    return token_add_assoc_unique(a, b) == 0;
}

/* 关系事实（知识）：记一笔；`route_writes` 打开时**走受审通道**。
 * 两条路的语义相同（MOD_TAG_REL 内部就是 token_tag_rel），差别只在
 * 谁来审查、什么时候落地 —— 所以这是一次**可测量**的开关，不是改行为。 */
static inline int ai_write_rel_raw(AI *ai, Token *a, Token *b, uint8_t rid, int neg, AiWriteSrc s);

static inline int ai_write_rel(AI *ai, Token *a, Token *b, uint8_t rid, int neg, AiWriteSrc s);

/* §6.580 同一条写入通道，但**账里记成哪种动作**由调用方定。
 * 为什么要这个参数：照抄/覆盖和"现场学"写的是**同一张图**（必须同一个通道，
 * 否则又会有第二条写入路径偷偷绕过受审），但账上必须能分开 —— 用户要能看见
 * "这 3 条是外来带进来的""这 1 条明知冲突还是写了"。 */
static inline int ai_write_rel_k(AI *ai, Token *a, Token *b, uint8_t rid, int neg,
                                 AiWriteSrc s, unsigned kind) {
    if (!ai || !a || !b || rid == 0) return -1;
    ai_write_count(ai, s);
    int rc = ai_write_rel_raw(ai, a, b, rid, neg, s);
    /* §6.248 **写后读回校验**（可选，`::verifywrites on` / `AIP_VERIFYWRITES=1`，默认关）：
     * 声称写成功就立刻用读路径查一遍。这是"报了没学进去"那类错的**机制**兜底
     * （人工清单只能保证这一版）。默认关的理由：每次学习多一次查图，且生产路径不需要。 */
    if (rc == 0) ai_modlog_note(ai, a, b, rid, neg, s, (AIModKind)kind);   /* §6.564 */
    if (rc == 0 && ai->verify_writes && ai->rel_verify_hook) {
        /* 读路径在 rel.h（ai.h 之后才 include）⇒ 走**函数指针**，由 main.c 装上 */
        if (!ai->rel_verify_hook(a, b, rid, neg)) ai->n_verify_miss++;
        ai->n_verify_ok++;
    }
    return rc;
}

static inline int ai_write_rel(AI *ai, Token *a, Token *b, uint8_t rid, int neg, AiWriteSrc s) {
    return ai_write_rel_k(ai, a, b, rid, neg, s, AIMOD_REL);
}

static inline int ai_write_rel_raw(AI *ai, Token *a, Token *b, uint8_t rid, int neg, AiWriteSrc s) {
    (void)s;
    if (!a || !b || a == b) return -1;   /* §6.584 自环不是事实（含动态谓语）*/
    Micro *mk = micro_of(ai);
    if (ai->route_writes && mk && !mk->safe.mod_off) {
        ModIntent m;
        memset(&m, 0, sizeof m);
        m.type = MOD_TAG_REL;
        m.target = a; m.main = b; m.rel_id = rid;
        m.neg = (uint8_t)(neg ? 1 : 0);      /* 否定也要带过去（否则改口/否定语义会丢） */
        {   /* source 只有 24 字节：有界拼接（"统一通道:关系学习" 正好 26 字节，
             * snprintf 会被 -Werror=format-truncation 拦下；截断是故意的） */
            size_t off = 0;
            const char *head = "统一通道:";
            for (const char *q = head; *q && off + 1 < sizeof m.source; ++q) m.source[off++] = *q;
            for (const char *q = ai_write_src_name(s); *q && off + 1 < sizeof m.source; ++q)
                m.source[off++] = *q;
            m.source[off] = 0;
        }
        char note[96];
        return micro_try(ai, &m, note, sizeof note) == SAFE_OK ? 0 : -1;
    }
    /* **动态关系**（清单外的谓语）：核心关系走 rel/nrel 位，动态关系走 `dyn` + `nrel` 第 15 位
     * （§6.80）—— 以前这里直接落到 `token_tag_nrel()`，而它只认 `rid < REL_DYN_BASE`
     * ⇒ 对话里说「A 不 <动态谓语> B」整句掉降级答复（实测）。 */
    if (rid >= REL_DYN_BASE)
        return token_tag_dyn_neg(a, b, (uint8_t)(rid - REL_DYN_BASE + 1), neg, REL_BOOST);
    return neg ? token_tag_nrel(a, b, rid, REL_BOOST) : token_tag_rel(a, b, rid, REL_BOOST);
}

#endif /* AI_H */

/* 结构化回复层（依赖 ai.h 里的 AI / ai_tokenize / ai_tokens_to_text） */
#include "frame.h"

/* 语义关系层（L2）：类型化关系 + 传递推理 + 对话中动态学知识 */
#include "rel.h"

/* 推导层：把「链」做成第一等公民 —— 可验证、可渲染（文字是最后一步的事）。
 * 必须排在 rel.h 之后：它用 Proof / rel_solve 做验证。 */
#include "derive.h"

/* 思维循环（议程 + 分步 + 跨轮未解问题 + 元认知）。
 * 排在最后：它要用 rel.h 的求解器、derive.h 的验证器、micro.h 的意图队列。 */
#include "think.h"
