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

/* ==================== ② 触发接求解：**开放式关系问句**（§6.86） ====================
 *
 * 量出来的缺口（先量后改）：`猫 属于 什么` / `什么 属于 动物` / `谁 属于 动物` 这类
 * **缺一端**的关系问句全掉"我不太懂" —— 可图里明明答得出来（正向查邻居 / 反向扫一遍）。
 * 关系层此前只答**闭合**问句（`A R B 吗`）。这一段把"表面说法"接到**求解器**上：
 *   · `A R 什么/谁`     ⇒ 正向查 `rel_neighbors(a, rid)`；
 *   · `什么/谁 R B`     ⇒ **反向**扫一遍（谁有 `R` 边指向 B），上限 64 个符号节点。
 * 默认关（`AIP_RELOPEN=1` / `::rel open on`）：关着时这些话照旧走流水线（逐字节相同）。 */
/* 把 `w[0..take)` **连起来**（不加空格）写进 `dst` —— 分词器把「到底」切成「到」「底」
 * 的时候，只有这样比才认得出填充词（同一个坑：合写否定 §6.69 / 情态 §6.74③ 都踩过）。 */
static inline size_t agent_open_join(char *const *w, size_t take, char *dst, size_t cap) {
    size_t L = 0;
    if (cap) dst[0] = 0;
    for (size_t i = 0; i < take; ++i) {
        for (const char *s = w[i]; s && *s; ++s) {
            if (L + 1 >= cap) return L;
            dst[L++] = *s;
        }
    }
    if (cap) dst[L < cap ? L : cap - 1] = 0;
    return L;
}

static inline int agent_open_slot_word(const char *w) {
    return w && (strcmp(w, "什么") == 0 || strcmp(w, "谁") == 0 || strcmp(w, "哪些") == 0 ||
                 strcmp(w, "啥") == 0);
}

/* §6.148 **占位词也可能是两个词**：分词器会把「哪些」「哪种」「哪个」切开
 * （实测「哪些 属于 动物」整句掉流水线，而「谁 属于 动物」能答）。
 * 所以跟填充词一样按**连写**比 —— 返回占位短语占几个词（1 或 2），0 = 这儿不是占位词。 */
static inline size_t agent_open_slot_len(char *const *w, size_t n) {
    static const char *const slots[] = { "什么", "谁", "哪些", "啥", "哪种", "哪个",
                                         "哪一个", "哪一位", NULL };
    for (size_t take = (n < 2 ? n : 2); take >= 1; --take) {
        char j[32];
        agent_open_join(w, take, j, sizeof j);
        for (size_t k = 0; slots[k]; ++k)
            if (strcmp(j, slots[k]) == 0) return take;
    }
    return 0;
}

/* §6.145 **先量后改**：把 12 种"开放式关系问句"喂进去，只有 3 种接上了（`A R 什么` /
 * `什么 R B` / 尾巴挂"吗"）；漏掉的里**四成是填充词/问法外壳**：
 *   「猫 **到底** 属于 什么」「**请问** 猫 属于 什么」「猫 **究竟** 属于 什么 呢」
 *   「**其实** 猫 属于 什么」「**我 想 知道** 猫 属于 什么」
 * 这一层只干一件事：把这些**外壳**剥掉再走原来的形状判定。剥完还是原来的形状才接，
 * 所以"剥错了"最多是**不接**（退回流水线），不会答错。
 *
 * 有意**不**把 `我`/`想` 当通用填充词：`我 喜欢 什么` 里 `我` 是**主语**，
 * 一滤就把可答的问句滤成不可答 —— 所以只剥**写死的几个外壳**，不做通用过滤。 */
/* 前缀外壳：`我 想 知道` / `我 想知道` / `请问` / `问 一下` / `麻烦` / `你 知道`。
 * 返回该剥掉几个词（0 = 不是外壳）。 */
static inline size_t agent_open_shell_len(char *const *w, size_t n) {
    static const char *const shells[] = { "我想知道", "请问", "问一下", "麻烦你", "麻烦",
                                          "你知道", NULL };
    for (size_t take = (n < 3 ? n : 3); take >= 1; --take) {
        char j[32];
        agent_open_join(w, take, j, sizeof j);
        for (size_t k = 0; shells[k]; ++k)
            if (strcmp(j, shells[k]) == 0) return take;
    }
    return 0;
}

/* 填充词：`到底/究竟/其实/那么/大概/也许/一下/倒是`（同样按**连写**比）。
 * 返回该跳过几个词（0 = 不是填充词）。 */
static inline size_t agent_open_filler_len(char *const *w, size_t n) {
    static const char *const fills[] = { "到底", "究竟", "其实", "那么", "大概", "也许",
                                         "一下", "倒是", NULL };
    for (size_t take = (n < 2 ? n : 2); take >= 1; --take) {
        char j[32];
        agent_open_join(w, take, j, sizeof j);
        for (size_t k = 0; fills[k]; ++k)
            if (strcmp(j, fills[k]) == 0) return take;
    }
    return 0;
}

static inline size_t agent_open_strip_shell(char *const *words, size_t n,
                                            char **out, size_t cap) {
    if (!words || !out) return 0;
    size_t i = 0;
    for (int round = 0; round < 2; ++round) {          /* 最多剥两层外壳 */
        size_t got = agent_open_shell_len(words + i, n - i);
        if (got == 0) break;
        i += got;
    }
    size_t k = 0;
    while (i < n) {
        size_t skip = agent_open_filler_len(words + i, n - i);
        if (skip) { i += skip; continue; }
        if (k >= cap) return 0;                        /* 装不下就不接（不截断着猜）*/
        out[k++] = words[i++];
    }
    return k;
}

/* §6.212 **名字类问法的形状归一**：这些问法都该落到「谁 叫 X / A 叫 什么」上 ——
 * 关系层早就会答，只是没认这几种说法（实测：`我 的 名字 是 什么` 掉流水线，
 * 答「我 的 愿 望 很 小」；`你 知道 我 的 名字 吗` 答「忘 了 别 的」）。
 * 只在**问句**上改（`A 的 名字 是 B` 那种陈述走学习侧，另有一处）。返回 1 = 改了。 */
static inline int agent_norm_name_q(char *const *w, size_t n, char **nw, size_t *out_n) {
    if (!w || !nw || !out_n || n < 3) return 0;
    size_t last = n;
    while (last > 0 && (!strcmp(w[last - 1], "吗") || !strcmp(w[last - 1], "呢"))) last--;
    /* ① `X 是 谁` ⇒ `谁 叫 X`（反查）*/
    if (last == 3 && !strcmp(w[1], "是") && !strcmp(w[2], "谁")) {
        nw[0] = (char *)"谁"; nw[1] = (char *)"叫"; nw[2] = w[0];
        *out_n = 3; return 1;
    }
    /* ② `A 的 名字 是/叫 什么` ⇒ `A 叫 什么`（正查）；分词把「名字」切成 名/字 也认 */
    {
        size_t mi = 0; int split = 0;
        for (size_t i = 0; i + 1 <= last; ++i) {
            if (i < last && !strcmp(w[i], "名字")) { mi = i; break; }
            if (i + 1 < last && !strcmp(w[i], "名") && !strcmp(w[i + 1], "字")) { mi = i; split = 1; break; }
        }
        size_t after = split ? mi + 2 : mi + 1;
        if (mi == 2 && last == after + 2 && !strcmp(w[1], "的") &&
            (!strcmp(w[after], "是") || !strcmp(w[after], "叫")) &&
            !strcmp(w[after + 1], "什么")) {
            nw[0] = w[0]; nw[1] = (char *)"叫"; nw[2] = (char *)"什么";
            *out_n = 3; return 1;
        }
    }
    /* ②b §6.234 `A 是 一 种/只/个 什么 [吗]` ⇒ `A 属于 什么`（量词问句）*/
    {
        size_t last2 = last;
        if (last2 >= 5 && (!strcmp(w[1], "是")) && (!strcmp(w[last2 - 1], "什么"))) {
            size_t q = last2 - 1;                    /* 什么 的位置 */
            size_t c = q - 1;                        /* 量词/数词的位置 */
            if (c >= 2 && (!strcmp(w[c], "种") || !strcmp(w[c], "只") ||
                           !strcmp(w[c], "个") || !strcmp(w[c], "类") ||
                           !strcmp(w[c], "种 类"))) {
                nw[0] = w[0]; nw[1] = (char *)"属于"; nw[2] = (char *)"什么";
                *out_n = 3; return 1;
            }
        }
    }
    /* ③ `你/您 知道 A 的 名字 [吗]` ⇒ `A 叫 什么` */
    if (last == 5 && (!strcmp(w[0], "你") || !strcmp(w[0], "您")) && !strcmp(w[1], "知道") &&
        !strcmp(w[3], "的") && !strcmp(w[4], "名字")) {
        nw[0] = w[2]; nw[1] = (char *)"叫"; nw[2] = (char *)"什么";
        *out_n = 3; return 1;
    }
    /* ④ `你/您 知道 A 的 名 字 [吗]`（名字被切开）*/
    if (last == 6 && (!strcmp(w[0], "你") || !strcmp(w[0], "您")) && !strcmp(w[1], "知道") &&
        !strcmp(w[3], "的") && !strcmp(w[4], "名") && !strcmp(w[5], "字")) {
        nw[0] = w[2]; nw[1] = (char *)"叫"; nw[2] = (char *)"什么";
        *out_n = 3; return 1;
    }
    return 0;
}

/* §6.269 分词会把「不 喜欢」**合成一个词**「不喜欢」：占位问句里 `words[1]` 因此不是
 * `不` 也不是关系词 ⇒ `A 不 R 什么` / `谁 不 R B` 整句掉闲聊（实测「我 不 喜欢 什么」
 * 答「你 想 说 的 时候 再 说」）。这里统一剥掉词首的「不」再查关系 id，并带出否定标志。 */
static inline uint8_t agent_rid_maybe_neg(AI *ai, const char *w, int *neg) {
    uint8_t rid = w ? rel_id_of(ai, w, 0) : 0;
    if (!rid && w) {
        const char *no = "不";
        size_t nl = strlen(no);                 /* UTF-8：3 字节 */
        if (strncmp(w, no, nl) == 0) {
            rid = rel_id_of(ai, w + nl, 0);
            if (rid && neg) *neg = 1;
        }
    }
    return rid;
}

/* §6.273 **多字生词的拼法**：分词把没见过的词按单字切（`石家庄` → 石/家/庄，
 * `笔记本` → 笔/记/本）。形状 = 前面若干**单字实词**（最多 4 个）+ 末词（单字，或已在池里的多字词）。
 * 为什么末词放开：`笔记本电脑` = 笔/记/本 + 电脑。为什么不无限放开：`开猫 真的` 这种
 * （两字词 + 填充词）必须挡住 —— optest 有一条「不是写死的填充词 ⇒ 不接」钉着。 */
static inline int agent_loc_char(const char *w) {
    static const char *loc[] = {"上", "下", "里", "中", "外", NULL};
    for (size_t i = 0; loc[i]; ++i) if (w && strcmp(w, loc[i]) == 0) return 1;
    return 0;
}

static inline int agent_compound_ok(AI *ai, char *const *w, size_t k) {
    if (!ai || !w || k < 2 || k > 4) return 0;
    size_t bytes = 0;
    for (size_t i = 0; i < k; ++i) {
        size_t l = strlen(w[i]);
        /* 地名首字（`上`海/`中`国）本身在虚词表里，但作为复合词首字是合法的
         * （与 §6.272 学习侧那条例外同一份名单）。 */
        if (i == 0 && agent_loc_char(w[i])) { bytes += l; continue; }
        if (i + 1 == k) {
            if (!rel_one_cjk(w[i]) && !pool_find(ai->pool, w[i])) return 0;
        } else if (!rel_one_cjk(w[i])) return 0;
        bytes += l;
    }
    return bytes >= 6 && bytes <= 12;
}

static inline char *agent_open_relation(AI *ai, char *const *words, size_t n, float *out_conf) {
    /* 开关在**函数里**也要把一道（不只是调用点）——"关着就一定不接"这种保证，
     * 写在唯一入口上比写在每个调用点上可靠（第一版只写在调用点，测试直接调它就漏了）。 */
    if (!ai || !ai->rel_open || ai->rel_off || !words || n < 3 || n > 8) return NULL;
    /* §6.212 名字类问法先归一到关系层认得的形状（`谁 叫 X` / `A 叫 什么`）。
     * ⚠ `nw` 必须声明在**函数作用域**：写进块里再 `words = nw` 会留下悬垂指针（实测崩）。 */
    char *nw[5]; size_t nn = 0;
    if (agent_norm_name_q(words, n, nw, &nn)) { words = nw; n = nn; }
    /* §6.215 `A 在 哪` = `A 在 什么`（问**地点**）。关系层只认「什么」这一种占位词，
     * 实测 `我 在 家里` 学得进、`我 在 什么` 答得出，但 **`我 在 哪` 掉兜底**。
     * ⚠ 只对**地点关系（在）**开这个口子：`开猫 属于 哪` 这种"哪 + 别的关系"仍然不接 ——
     * `optest` 有一条断言钉着"光一个「哪」不算占位词（不接，不乱猜）"，那是**有意的保守**。
     * ⚠ 分词会把「哪里/哪儿」切成 哪/里、哪/儿 ⇒ 两种形状都要认，关系词位置也跟着挪。 */
    {
        size_t qpos = n;          /* 占位词在哪个下标 */
        if (n >= 3 && words[n - 1] &&
            (!strcmp(words[n - 1], "哪") || !strcmp(words[n - 1], "哪里") ||
             !strcmp(words[n - 1], "哪儿"))) {
            qpos = n - 1;
        } else if (n >= 4 && words[n - 2] && !strcmp(words[n - 2], "哪") && words[n - 1] &&
                   (!strcmp(words[n - 1], "里") || !strcmp(words[n - 1], "儿"))) {
            qpos = n - 2;
        }
        if (qpos < n && qpos >= 1 && words[qpos - 1] &&
            rel_id_of(ai, words[qpos - 1], 0) == REL_AT) {
            for (size_t i = 0; i < qpos; ++i) nw[i] = words[i];
            nw[qpos] = (char *)"什么";
            words = nw; n = qpos + 1;
        } else if (qpos < n && qpos == 1 && words[0]) {
            /* §6.262 **关系词被分词吞进了前一个 token**：不带空格写「我在哪」「我住哪」时，
             * 分词器给出 [我在][哪] / [我住][哪]（`我在` 在词池里是最大匹配）⇒ 上面那条判据
             * `rel_id_of("我在")` 不成立、整句掉兜底。这里把后缀 在/住 剥出来当关系词。
             * ⚠ 就地改 `words[0]`（截断成主语）—— 它是 ai_tokenize 分配的、后面统一 free，安全。 */
            const char *w = words[0];
            size_t wl = strlen(w);
            static const char *suf[] = {"在", "住", NULL};
            for (size_t i = 0; suf[i]; ++i) {
                size_t sl = strlen(suf[i]);
                if (wl <= sl || strcmp(w + wl - sl, suf[i]) != 0) continue;
                ((char *)w)[wl - sl] = 0;
                if (!w[0]) return NULL;
                nw[0] = (char *)w;
                nw[1] = (char *)"在";      /* 住 也问"在哪"（住的地方就是地点）*/
                nw[2] = (char *)"什么";
                words = nw; n = 3;
                break;
            }
        }
    }
    /* §6.145 剥掉问法外壳（填充词/前缀）——剥完再按原来的形状判定 */
    char *stripped[16];
    {
        size_t sn = agent_open_strip_shell(words, n, stripped, sizeof stripped / sizeof stripped[0]);
        if (sn == 0 || sn > 5) return NULL;
        words = stripped; n = sn;
    }
    if (ai_has_question_word(words, n)) { /* 问句里除了占位词还有别的疑问词 ⇒ 不是这条路 */ }
    /* 形状一：`A R 什么`（3 词；4/5 词时允许尾巴上挂"呢/吗"）
     * §6.147 先量后加：`A R 什么 <名词>`（「猫 属于 什么 动物」）以前**整句掉流水线** ——
     * 尾巴上那个名词是**类别提示**，不影响该问什么 ⇒ 照形状一答，提示词原样带回话里。 */
    size_t last = n;
    while (last > 0 && (strcmp(words[last - 1], "呢") == 0 || strcmp(words[last - 1], "吗") == 0))
        last--;
    /* §6.269 `A 不 R 什么`（「我 不 喜欢 什么」）—— shape 一 以前只认 `A R 什么`，
     * `words[1]` 是 `不`（不是关系词）⇒ 整句掉闲聊（实测答「你 想 说 的 时候 再 说」，
     * 明明图里有「我 不 喜欢 猫」）。`不` 只认紧跟主语的那一个，别的位置不动。 */
    size_t qn = (last >= 4 && strcmp(words[1], "不") == 0) ? 1 : 0;
    size_t rpos = 1 + qn;
    size_t sl1 = (last >= rpos + 2) ? agent_open_slot_len(words + rpos + 1, last - rpos - 1) : 0;
    if (sl1 && (last == rpos + 1 + sl1 || last == rpos + 1 + sl1 + 1)) {
        const char *hint = (last == rpos + 1 + sl1 + 1) ? words[rpos + 1 + sl1] : NULL;   /* 类别提示（可为空）*/
        Token *a = pool_find(ai->pool, words[0]);
        int rneg = 0;
        uint8_t rid = agent_rid_maybe_neg(ai, words[rpos], &rneg);
        if (qn) rneg = 1;
        /* §6.270 试过但**证不出正例**：分词会把生词切成单字（`上海` → 上/海），
         * 短语形状根本进不到这里，所以「主语不认识」这条分支一直是死代码 ⇒ 撤掉
         * （纪律：证不出正例的改动不留）。真正要修的是分词/生词那一层。 */
        if (!a || rid == 0) return NULL;
        Token *out[8];
        uint8_t from_hyp[8];
        size_t got = rel_next_cands(ai, a, rid, rneg, out, from_hyp,
                                    (size_t)(sizeof out / sizeof out[0]));
        char buf[256];
        size_t off = 0;
        const char *rd1 = words[rpos];
        if (rneg && strncmp(rd1, "不", strlen("不")) == 0) rd1 += strlen("不");
        off += (size_t)snprintf(buf + off, sizeof buf - off, "我 记 的 是 ： %s %s%s ",
                                a->name ? a->name : "?", rneg ? "不 " : "", rd1);
        if (got == 0) {
            /* §6.269 第一人称"地点没记"时给一句怎么教：实测「我 在 哪」答成
             * 「我 记 的 是 ： 我 在 什么 ， 图里 没 记 过」—— 像在复述问题，用户拿不到下一步。 */
            if (!rneg && a->name && strcmp(a->name, "我") == 0 && rid == REL_AT)
                snprintf(buf + off, sizeof buf - off,
                         "什么 ， 你 还 没 告诉 我 你 在 哪 —— 说 一 句「我 在 上海」我 就 记 住（不 编）");
            /* §6.147：带类别提示时把提示原样带回这句里（「… 属于 什么 动物 ， 图里 没 记 过」）*/
            else if (hint) snprintf(buf + off, sizeof buf - off, "什么 %s ， 图里 没 记 过（不 编）", hint);
            else      snprintf(buf + off, sizeof buf - off, "什么 ， 图里 没 记 过（不 编）");
        } else {
            for (size_t i = 0; i < got && i < 4; ++i)
                off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s",
                                        i ? " 、 " : "", out[i]->name ? out[i]->name : "?");
            if (got > 4) snprintf(buf + off, sizeof buf - off, " … 共 %zu 个", got);
        }
        if (out_conf) *out_conf = 0.8f;
        char *r = (char *)malloc(strlen(buf) + 1);
        if (r) memcpy(r, buf, strlen(buf) + 1);
        return r;
    }
    /* §6.272 **多字主语**（生词被切成单字的安全网）：分词对没见过的词按单字切
     * （`上海` → 上/海，`夸克` → 夸/克），严格形状 `[A][R][什么]` 于是认不出。
     * 放宽成：在关系词位置找**第一个已知关系词**，它前面的词拼成主语。
     * 主语在池里就照常查；**不在池里就明说"还不认识"**（这才是用户能接着做的动作）。
     * 只在严格形状失败后才走这里，所以已认识的词行为一点不变。 */
    if (last >= 3 && last <= 6) {
        size_t rj = 0;
        uint8_t rjd = 0;
        int rjneg = 0;
        for (size_t i = 1; i + 1 < last; ++i) {
            int ng = 0;
            uint8_t id = agent_rid_maybe_neg(ai, words[i], &ng);
            if (id) { rj = i; rjd = id; rjneg = ng; break; }
        }
        size_t sj = (rj >= 2) ? agent_open_slot_len(words + rj + 1, last - rj - 1) : 0;
        if (rj >= 2 && sj && (last == rj + 1 + sj || last == rj + 1 + sj + 1)) {
            char subj[64];
            size_t so = 0;
            for (size_t i = 0; i < rj; ++i) {
                size_t l = strlen(words[i]);
                if (so + l + 1 >= sizeof subj) { so = 0; break; }
                memcpy(subj + so, words[i], l);
                so += l;
            }
            subj[so] = 0;
            /* §6.272/§6.273 拼法要合法（前面全是单字、末词在池里、2~4 字）。
             * 不放宽到任意长度：`开猫 真的 属于 什么` 这类会被接住（optest 当场红）。 */
            if (!agent_compound_ok(ai, words, rj)) return NULL;
            Token *a2 = pool_find(ai->pool, subj);
            char buf[256];
            if (!a2) {
                snprintf(buf, sizeof buf,
                         "我 还 不 认识 「%s」（图里 没 这 个 词）—— 你 教 一 句「%s %s …」"
                         "我 就 记 住（不 编）。", subj, subj,
                         rel_name_of(ai, rjd) ? rel_name_of(ai, rjd) : "属于");
            } else {
                Token *out2[8];
                uint8_t hyp2[8];
                size_t got2 = rel_next_cands(ai, a2, rjd, rjneg, out2, hyp2,
                                             (size_t)(sizeof out2 / sizeof out2[0]));
                size_t o2 = 0;
                o2 += (size_t)snprintf(buf + o2, sizeof buf - o2, "我 记 的 是 ： %s %s%s ",
                                       subj, rjneg ? "不 " : "",
                                       rel_name_of(ai, rjd) ? rel_name_of(ai, rjd) : "?");
                if (got2 == 0)
                    snprintf(buf + o2, sizeof buf - o2, "什么 ， 图里 没 记 过（不 编）");
                else {
                    for (size_t i = 0; i < got2 && i < 4; ++i)
                        o2 += (size_t)snprintf(buf + o2, sizeof buf - o2, "%s%s",
                                               i ? " 、 " : "",
                                               out2[i]->name ? out2[i]->name : "?");
                    if (got2 > 4) snprintf(buf + o2, sizeof buf - o2, " … 共 %zu 个", got2);
                }
            }
            if (out_conf) *out_conf = a2 ? 0.8f : 0.6f;
            char *r2 = (char *)malloc(strlen(buf) + 1);
            if (r2) memcpy(r2, buf, strlen(buf) + 1);
            return r2;
        }
    }
    /* 形状二：`什么/谁 R B`（反向扫；上限 64 个候选，避免大池子上太慢） */
    size_t sl2 = agent_open_slot_len(words, last);
    /* §6.166 **否定反查**：`谁 不 R B`（「谁 不 会 唱歌」）。以前 `不` 会被当成**类别词**
     * （`哪些 动物 ...` 那一档的形状刚好也合），于是回答变成「… （只算 不） ： 图里 没 记 过」。
     * 这里把 `不` 认成否定，再用**否定边**去扫。 */
    int oq_neg = 0;
    if (sl2 && sl2 < last && strcmp(words[sl2], "不") == 0) { oq_neg = 1; }
    /* §6.151 **带类别的反查**：`哪些 <类别> R B`（「哪些 动物 在 天上」）——
     * 先量到出厂模型里**有正例**（是动物的：鸟；不是的：太阳…共 4 个）⇒ 值得做。
     * 做法：候选必须是 `t 属于 <类别>`（直边）。**不做**"理解成 `什么 R B` 再全列"——
     * 那会把不是动物的东西也列出来，是**误导**（宁可少答、不可答错）。 */
    size_t base = sl2 + (size_t)oq_neg;                /* 关系词的位置（跳过 不）*/
    int has_cat = 0;
    if (sl2 && last == base + 2) has_cat = 0;
    else if (sl2 && last == base + 3) has_cat = 1;
    if (sl2 && (last == base + 2 || has_cat)) {
        const char *catw = has_cat ? words[base] : NULL;
        Token *cat = catw ? pool_find(ai->pool, catw) : NULL;
        if (has_cat && !cat) return NULL;               /* 类别词不在图里 ⇒ 不接（不猜）*/
        uint8_t rid = agent_rid_maybe_neg(ai, words[base + (has_cat ? 1 : 0)], &oq_neg);
        Token *b = pool_find(ai->pool, words[base + (has_cat ? 2 : 1)]);
        if (rid == 0 || !b) return NULL;
        char buf[256];
        size_t off = 0, hits = 0;
        /* §6.269 关系词可能**自带**「不」（单 token「不喜欢」）⇒ 文案里别再补一个「不」 */
        const char *rdisp = words[base + (has_cat ? 1 : 0)];
        if (oq_neg && strncmp(rdisp, "不", strlen("不")) == 0) rdisp += strlen("不");
        const char *bdisp = words[base + (has_cat ? 2 : 1)];
        if (cat) off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s %s 的 有 （只算 %s） ： ",
                                         oq_neg ? "不 " : "", rdisp, bdisp, catw);
        else     off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s %s 的 有 ： ",
                                         oq_neg ? "不 " : "", rdisp, bdisp);
        /* §6.149 **先量后改**：`scanned < 64` 会让"第 64 个内容词之后"的答案**永远查不到**。
         * 实测（出厂模型）：`什么 属于 车` 漏掉 `火车`/`汽车` —— 它俩在第 **1045** 个内容词上，
         * 旧上限直接回"图里 没 记 过"（**假否定**）。合成小脑子里池子小、一直没抓到，
         * 所以这条看护是这一轮才补的（见 optest ㊲ 的"垫 80 个词再查"）。
         * §6.150 再把那个"防超大池子"的 4096 **也拿掉**：扫**整个池子**，扫了多少**记进
         * `n_scan_tokens`**（§6.128 的规矩：全图扫描必须记账 —— 这样"反查是不是变慢了"
         * 有数字可查，而不是靠一个拍的上限去赌）。输出仍只列前 4 个（够了就提前停，
         * 记账记的是**真走过的**词数）。 */
        /* §6.214 **走反查表**（原来全池扫）：只取 `b` 的桶，逐条 `rel_direct*` **校验**
         * （陈旧项——GC 删边/改口/猜升格——自然被过滤），再按 **词 id 升序**排一遍：
         * 池序就是 id 序，所以输出顺序与"全池扫描"逐字一致。
         * 表是"只追加"的：漏写一条的后果是**漏知识**（不是慢），所以有 `::revcheck` 全量对拍。 */
        uint32_t cand[512];
        size_t ncd = 0;
        for (uint32_t ent = b->rev_head;
             ent && ncd < (sizeof cand / sizeof cand[0]);
             ent = ai->pool->rev_next[ent - 1]) {
            uint32_t meta = ai->pool->rev_meta[ent - 1];
            if ((uint8_t)(meta & 0x7FFFu) != rid) continue;
            if (((meta & 0x8000u) != 0) != (oq_neg != 0)) continue;
            uint32_t sid = ai->pool->rev_src[ent - 1];
            if (sid >= ai->pool->count) continue;
            cand[ncd++] = sid;
        }
        for (size_t i = 1; i < ncd; ++i) {          /* 插入排序（桶很小）*/
            uint32_t v = cand[i]; size_t j = i;
            while (j > 0 && cand[j - 1] > v) { cand[j] = cand[j - 1]; --j; }
            cand[j] = v;
        }
        size_t scanned = 0;
        for (size_t ci = 0; ci < ncd && hits < 4; ++ci) {
            if (ci && cand[ci] == cand[ci - 1]) continue;   /* 同一条边可能被重复追加 */
            Token *t = ai->pool->tokens[cand[ci]];
            if (!t || t->is_garbage || !t->name || t->name[0] == '[') continue;
            scanned++;
            /* §6.166：`谁 不 R B` 扫**否定边**；其余扫肯定那档 */
            if (oq_neg) { if (rel_direct_neg(t, rid, b) == 0) continue; }
            else        { if (rel_direct(t, rid, b) == 0) continue; }
            if (cat && rel_direct(t, REL_IS_A, cat) == 0) continue;   /* §6.151 类别过滤 */
            off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s",
                                    hits ? " 、 " : "", t->name);
            hits++;
        }
        if (hits == 0) snprintf(buf + off, sizeof buf - off, "图里 没 记 过（不 编）");
        else if (hits >= 4) snprintf(buf + off, sizeof buf - off, " 等（只列前几个）");
        ai->n_scan_tokens += scanned;          /* §6.150：全图扫描记账（§6.126/§6.128 的规矩）*/
        if (out_conf) *out_conf = hits ? 0.8f : 0.4f;
        char *r = (char *)malloc(strlen(buf) + 1);
        if (r) memcpy(r, buf, strlen(buf) + 1);
        return r;
    }
    return NULL;
}

/* ============ ② 类比兜底（§6.90，默认关）：规则不行时，用"上位/同类"的例子答**大概** ============
 *
 * 缺口：求解器只在**规则（关系边）**上走，找不到就说不知道 —— 但它手上其实还有两类"近似依据"：
 *   ① **上位**：`A 属于 P` 且 `P —R→ B` ⇒ "A 大概也 R B"（类别继承的类比）；
 *   ② **同类**：A 的**兄弟** A2（同一 parent）有 `A2 —R→ B` ⇒ "同类里 A2 也是 R B"。
 * 这一版把它们当**兜底**（只在规则答不出时用），并且：
 *   · 措辞一定是「**大概**」+ 说清依据是谁（绝不硬说"对"）；
 *   · 置信度**压低**（0.45~0.50），进 `out_conf`；
 *   · 上游"有没有据"的账照旧（这条不进图、不改知识，只影响这一次回答）。 */
/* §6.125 **记住一条猜出来的结论**（默认关，`ai->rel_remember`）。
 * 边上打 `[类比:依据词]`（自带出处）+ `role = 推论`：
 *   · 下一个问题可以拿它**当中转**（R0 在"记忆可见"那一趟里才认它）；
 *   · 但 `rel_direct` 一律跳过它 ⇒ "图里有没有这条**知识**"仍然是"没有"
 *     ⇒ 验证器的自查查不到它，**永远不会把"猜的"变成"知道的"**。
 * 幂等 + 有界（同一个 A-关系-B 只记一条；上限 `AI_GUESS_MEM_MAX` 条防长会话无界增长）。 */
#define AI_GUESS_MEM_MAX 512
static inline void agent_guess_remember(AI *ai, Token *A, uint8_t rid, Token *B,
                                       const Token *sib) {
    if (!ai || !ai->rel_remember || !A || !B || rid == 0 || rid >= REL_DYN_BASE) return;
    if (!sib || !sib->name) return;
    if (rel_direct(A, rid, B)) return;              /* 观测/知识里已经有了：不必记，也绝不动它 */
    if (rel_direct_guess(A, rid, B, 0)) return;      /* 记过了：幂等 */
    if (ai->n_rel_guess_mem >= AI_GUESS_MEM_MAX) return;
    /* **别用 `token_tag_rel` 写这条边**：① 它是**观测**写法（"观测压过缓存"会把已有的推论边
     * 清成观测 ⇒ 猜测当场变成知识，实测中毒过一次）；② 它的返回值是 **0 = 成功** 的约定
     * （我按"非 0 才成功"写，于是一建边就 return，留下一条没打标记的观测边 ✗）。
     * 照 `micro.h` 的 `MOD_TAG_DERIVED`：纯建边 → 置位 → **角色最后打** → 拒绝降级观测。 */
    for (size_t i = 0; i < A->count; ++i)
        if (edge_premise_polarity(&A->assocs[i])) return;   /* 这条边上挂着前提（条件缓存）：不掺和 */
    char mark[80];
    snprintf(mark, sizeof mark, AI_GUESS_MARK_PREFIX ":%s]", sib->name);
    Token *m = pool_token_intern(ai->pool, mark);
    if (!m) return;
    if (token_add_assoc_unique(A, B) != 0) return;   /* 0 = 成功（这个家族都是这个约定） */
    AssocEntry *e = NULL;
    for (size_t i = 0; i < A->count; ++i)
        if (A->assocs[i].main == B && edge_is_plain(&A->assocs[i])) { e = &A->assocs[i]; break; }
    if (!e) return;
    uint16_t bit = REL_BIT(rid);
    if ((e->rel & bit) && e->role != ASSOC_ROLE_DERIVED) return;   /* 已是观测：拒绝降级 */
    int fresh_bit = !(e->rel & bit);
    e->rel |= bit;
    /* §6.223 反查表也要追加（漏了这一处 ⇒ `::revcheck` 报"缺 1 条：狗 -[会]-> 唱歌"，
     * 反查会**查不到**这条猜出来的知识 —— 又是"只追加必须每个写入点都写到"的活教材）。 */
    if (fresh_bit && A->owner) pool_rev_add(A->owner, B, A->id, rid, 0);
    e->action = m;                              /* 出处写在动作槽上（就像 [因果]/[前提]） */
    e->role = ASSOC_ROLE_DERIVED;               /* 角色最后打（观测写法会清掉它） */
    token_cache_enqueue(A);                     /* §6.127：新落的推论边要进复核队列 */
    ai->n_rel_guess_mem++;
}

/* §6.133 **「猜」只有一条路**：找依据 → 建推导链 → **过验证器** → 渲染 → 记账。
 *
 * 以前是两套各自实现的"类比"：
 *   · 答复层那条（`agent_answer_relation_inner` 的 `rel_guess` 分支）：有验证器、有链、走 `der_say`；
 *   · 兜底那条（`agent_analogy_fallback`）：只看"兄弟"，**不过验证器**、措辞手写、置信度手写。
 * 两条路的判据、门槛、记账各写一遍 —— §6.123 原因①留下的就是这个不一致（谁先跑谁说了算）。
 * 现在合成一个函数，两边都调它：判据一处、验证一处、把握一处（按猜测环数衰减）。
 *
 * 两种依据（措辞不同是**信息**，不是重复）：
 *   ① `rel_guess`：最近邻 / 结构相似（`emb` 余弦 + 关系骨架）—— 走 `der_say`，
 *      链上写「依据词 ≈（n% 像）≈> 宾语」；
 *   ② `rel_sibling_with`：**同类互推**（同一个上位下的兄弟有这条关系）—— 结构相似度当 `sim`
 *      喂给验证器（不够近就会被拒），措辞明说「同类里谁 也 R B（类比，不是直接知识）」。
 * 返回 1 = 答了（`resp`/`out_conf` 已填，cot 与账都已记）；0 = 没猜出来（调用方照旧说不知道）。 */
static inline void agent_hypo_cache(AI *ai, Token *A, uint8_t rid, Token *B, int neg, int truth);

#define AGENT_GUESS_NEIGHBOR 1   /* 允许：最近邻 / 结构相似（rel_guess） */
#define AGENT_GUESS_SIBLING  2   /* 允许：同类互推（兄弟有这条关系） */

static inline int agent_guess_answer(AI *ai, Token *A, uint8_t rid, Token *B, int want_neg,
                                     int kinds,
                                     Derivation *d, char *line, size_t linecap,
                                     char *resp, size_t respcap, float *out_conf) {
    if (!ai || !A || !B || rid == 0 || !d || !resp || respcap == 0) return 0;
    if (!(kinds & (AGENT_GUESS_NEIGHBOR | AGENT_GUESS_SIBLING))) return 0;
    /* ---- ① 最近邻 / 结构相似 ---- */
    Proof gpf;
    memset(&gpf, 0, sizeof gpf);
    if ((kinds & AGENT_GUESS_NEIGHBOR) && rel_guess(ai, A, rid, B, &gpf)) {
        Token *src = (gpf.n >= 2 && gpf.step[1].tok) ? gpf.step[1].tok : A;
        int pol = (gpf.n >= 1) ? (gpf.step[gpf.n - 1].neg ? -1 : +1) : +1;
        int sim = (gpf.n >= 2) ? gpf.step[1].sim : 0;
        der_reset(d);
        d->has_claim = 1;
        der_goal(d, A, rid, B);
        der_default(d, A, src, rid, B, sim, pol);
        der_fact(d, src, rid, B, pol < 0 ? 1 : 0, pol);
        der_conclude(d, pol, (int)(ai_guess_conf(1) * 100.0f + 0.5f));
        ai->n_der++;
        if (der_verify(ai, d) == 0) {
            der_chain_text(ai, d, line, linecap);
            cot_push(ai, COT_INFER, 1, 45,
                     "类比推测（知识库里没有，借最像的那个词；已验证推测合法）", line);
            ai->n_cot++;
            der_say(ai, d, ai->der_style, resp, respcap);
            /* §6.221 **答复里要点到被问的那个词**：类比链的底是"最像的那个词"（鱼），
             * 实测 `考拉 吃 草 吗` 答成「鱼 ≈（60% 像）≈> 草」——完全没提考拉，读起来像答错了对象。
             * 用户问的是哪个词，答复就得以它开头（白箱：先说清"我在拿谁比"）。 */
            if (A->name && A->name[0] && !strstr(resp, A->name)) {
                char tmp[512];
                snprintf(tmp, sizeof tmp, "（问的是 %s）%s", A->name, resp);
                snprintf(resp, respcap, "%s", tmp);
            }
            agent_hypo_cache(ai, A, rid, B, want_neg, d->truth);
            agent_guess_remember(ai, A, rid, B, src);      /* 猜出来的结论记进"记忆"（一处） */
            if (out_conf) *out_conf = ai_guess_conf(1);
            return 1;
        }
    }
    /* ---- ② 同类互推（兄弟有这条关系）---- */
    if (!(kinds & AGENT_GUESS_SIBLING)) return 0;
    const Token *sup = NULL;
    const Token *sib = rel_sibling_with(ai, A, rid, B, &sup);
    if (!sib) return 0;
    size_t common = 0;
    int sim = rel_struct_sim(A, sib, &common);
    /* ⚠ **兄弟这条证据直接给到结构门槛分** —— 与 `rel_guess` 里对 `sib0` 的处理**同一个约定**
     * （rel.h：「兄弟这条证据：直接给到结构门槛分，不再要求共同邻居 ≥3」）。
     * 不这么做的话，刚够得上"同一个上位下的兄弟"的词会卡在相似度上被拒，
     * 而那条证据本来就不靠相似度 —— 实测：卡掉之后 ㊵ 那条兜底整条失效（4 项红）。
     * 取 max 是为了不把真的算出来的更高分压下去。 */
    if (sim < REL_STRUCT_MIN_PCT) sim = REL_STRUCT_MIN_PCT;
    int pol = rel_direct(sib, rid, B) ? +1 : -1;
    der_reset(d);
    d->has_claim = 1;
    der_goal(d, A, rid, B);
    der_default(d, A, (Token *)sib, rid, B, sim, pol);
    der_fact(d, (Token *)sib, rid, B, pol < 0 ? 1 : 0, pol);
    der_conclude(d, pol, (int)(ai_guess_conf(1) * 100.0f + 0.5f));
    ai->n_der++;
    if (der_verify(ai, d) != 0) return 0;                  /* ⚠ 这一条以前**没有**验证器 */
    der_chain_text(ai, d, line, linecap);
    cot_push(ai, COT_INFER, 1, 45,
             "同类互推（同一个上位下的兄弟有这条关系；类比，不是直接知识）", line);
    ai->n_cot++;
    snprintf(resp, respcap,
             "大概 —— 同类里 %.48s 也 %.12s %.48s（%.48s；**类比**，不是直接知识）%s%s",
             sib->name ? sib->name : "?",
             rel_name_of(ai, rid) ? rel_name_of(ai, rid) : "?",
             B->name ? B->name : "?",
             sup && sup->name ? sup->name : "?",
             "", "");
    if (A->name && A->name[0] && !strstr(resp, A->name)) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "（问的是 %s）%s", A->name, resp);
        snprintf(resp, respcap, "%s", tmp);
    }
    agent_hypo_cache(ai, A, rid, B, want_neg, d->truth);
    agent_guess_remember(ai, A, rid, B, sib);
    if (out_conf) *out_conf = ai_guess_conf(1);
    return 1;
}

static inline char *agent_analogy_fallback(AI *ai, char *const *words, size_t n, float *out_conf) {
    if (!ai || !ai->rel_analogy || !words || n < 3 || n > 6) return NULL;
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    if (!rel_parse(ai, words, n, jbuf, sizeof jbuf, &aw, &rid, &is_neg, &bw, &is_q, &rw, 0))
        return NULL;

    if (!is_q || rid == 0 || is_neg) return NULL;          /* 只兜肯定的疑问句（否定另说） */
    Token *A = pool_find(ai->pool, aw);
    Token *B = pool_find(ai->pool, bw);
    if (!A || !B) return NULL;
    if (rel_direct(A, rid, B) || rel_direct_neg(A, rid, B)) return NULL;   /* 已经知道就不猜 */
    /* §6.133：**和答复层那条走同一个函数**。这一侧的**依据集合**保持原样（只同类互推），
     * 变的是：① 现在也**过验证器**（以前不过）；② 把握按猜测环数算；③ 记账只有一处。 */
    Derivation d;
    der_reset(&d);
    char line[THINK_WHY], buf[THINK_CONCL];
    line[0] = 0; buf[0] = 0;
    float conf = 0.0f;
    if (!agent_guess_answer(ai, A, rid, B, 0, AGENT_GUESS_SIBLING,
                            &d, line, sizeof line, buf, sizeof buf, &conf))
        return NULL;
    if (out_conf) *out_conf = conf;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

/* 步骤一：用户**陈述**了一条关系事实吗？是就当场学进图里（并写进模型）。
 * 支持肯定（`猫 属于 动物`）和否定（`猫 不 会 飞`）两种陈述。
 * 返回要说的确认话（调用方 free），不是陈述就返回 NULL。 */
/* §6.229 **主语位置的虚词守卫**：代词/指示词可以当主语，**别的虚词不行**。
 * 实测漏洞：`很 属于 猫` 学进去了（`什么 属于 猫` 会答「很」）、`了 在 家里` 也学进去了。
 * 为什么不能直接 `fr_is_func(主语)`：代词和指示词本来就在虚词表里（§6.228 的教训）。 */
static inline int agent_subj_ok(const char *w) {
    if (!w || !w[0]) return 0;
    static const char *ok_subj[] = {
        "我", "你", "您", "他", "她", "它", "我们", "你们", "他们",
        "这", "那", "这个", "那个", "这些", "那些", "这里", "那里", "大家", "自己",
        NULL
    };
    for (size_t i = 0; ok_subj[i]; ++i) if (strcmp(w, ok_subj[i]) == 0) return 1;
    return !fr_is_func(w);
}

/* §6.229b **宾语位置的虚词守卫**（与 agent_subj_ok 一对）。
 * 实测漏洞：`猫 属于 很`、`我 在 很`（3 词走核心关系那条）会被学进去 ——
 * §6.226 的宾语守卫只加在**动态谓语**分支里，核心关系那条没查。
 * 允许表里放"本身就是地点/方位"的虚词（`猫 在 这里` 是正常的）。 */
static inline int agent_obj_ok(const char *w) {
    if (!w || !w[0]) return 0;
    static const char *ok_obj[] = {
        "这里", "那里", "这儿", "那儿", "上面", "下面", "里面", "外面",
        "前面", "后面", "旁边", "附近", NULL
    };
    for (size_t i = 0; ok_obj[i]; ++i) if (strcmp(w, ok_obj[i]) == 0) return 1;
    return !fr_is_func(w);
}

/* §6.218 **「A V 在 B」/「A 在 B V」里的地点事实**：
 * 实测 `我 住 北京` 学得进（动态关系 住），但 `我 住 在 北京`、`我 在 学校 学习` **整句掉兜底**
 * （答出「学习 小 的 那 我 空 吧」这种词串）。地点是 `在 B` 这一块，先把**它**学下来：
 * 学 `A 在 B`（`在` 是核心关系 REL_AT），动词/多出来的词**如实说没记**。
 * 守卫：疑问词/否定/多词主语/地点后缀（上/里/下/中/外/前/后/旁边）一律不接。 */
static inline char *agent_learn_locative(AI *ai, char *const *words, size_t n, float *out_conf) {
    if (!ai || ai->rel_off || !ai->locative || !words || n < 4 || n > 6) return NULL;
    if (ai_has_question_word(words, n)) return NULL;
    for (size_t i = 0; i < n; ++i) {
        if (!words[i]) return NULL;
        if (!strcmp(words[i], "不") || !strcmp(words[i], "没") || !strcmp(words[i], "没有")) return NULL;
    }
    size_t at = n;
    for (size_t i = 1; i + 1 < n; ++i) if (!strcmp(words[i], "在")) { at = i; break; }
    if (at == n || at + 1 >= n) return NULL;
    /* 地点后缀（「桌子 上」）说明宾语不是一个完整地点 ⇒ 不猜 */
    static const char *suf[] = {"上", "里", "下", "中", "外", "前", "后", "旁边", NULL};
    for (size_t k = 0; suf[k]; ++k) if (!strcmp(words[n - 1], suf[k])) return NULL;
    /* 地点边界怎么定（§6.219）：
     * ① `A V 在 B`（`在` 前有谓词、地点在句尾）⇒ 地点 = 后面所有词**拼起来**
     *    （分词常把「北京」切成 北/京）；
     * ② `A 在 B V`（`在` 紧跟在主语后面）⇒ 没有词典，改用**图里认不认得这个地点**：
     *    从长到短试「B = 后面 k 个词拼起来」，第一个**在图里、且反查桶里有「在」条目**
     *    的就当地点（`我 在 学校 学习` ⇒ 学校；「学校学习」图里没有）。认不出就**不猜**。 */
    char bl[32]; size_t bl_n = 0;
    size_t b_end = n;               /* 地点最后一个 token 的下标 + 1 */
    if (at >= 2) {
        b_end = n;
    } else {
        b_end = 0;
        for (size_t k = n; k > at + 1; --k) {
            char cand[32]; size_t cl = 0; int ok = 1;
            for (size_t i = at + 1; i < k; ++i) {
                size_t wl = strlen(words[i]);
                if (cl + wl + 1 >= sizeof cand) { ok = 0; break; }
                memcpy(cand + cl, words[i], wl); cl += wl;
            }
            if (!ok || cl == 0) continue;
            cand[cl] = 0;
            Token *ct = pool_find(ai->pool, cand);
            if (!ct) continue;
            if (pool_rev_any(ai->pool, ct, REL_AT, 0)) { b_end = k; break; }
        }
        if (b_end == 0) return NULL;      /* 边界认不出 ⇒ 不猜 */
    }
    for (size_t i = at + 1; i < b_end; ++i) {
        size_t wl = strlen(words[i]);
        if (bl_n + wl + 1 >= sizeof bl) return NULL;
        memcpy(bl + bl_n, words[i], wl); bl_n += wl;
    }
    bl[bl_n] = 0;
    if (bl_n == 0 || bl_n > 12) return NULL;
    /* §6.227 **地点不能是虚词**：实测 `我 住 在 很` 会学成 `我 在 很`（"很"当地点）。
     * 虚词表只有一处（frame.h 的 FR_FUNC + FR_PARTICLES），与 §6.226 同源。 */
    /* §6.258c **占位地名不算地名**：`我 住 在 一个 地方` 以前会学成「我 在 一个地方」
     * （"一个地方"是占位说法，不是地名）。 */
    if (strstr(bl, "地方") || strstr(bl, "某") || strstr(bl, "一个") || strstr(bl, "哪里"))
        return NULL;
    if (fr_is_func(bl)) return NULL;
    if (!agent_subj_ok(words[0])) return NULL;     /* §6.229 `了 在 家里` 不该学 */
    Token *A = pool_token_intern(ai->pool, words[0]);
    Token *B = pool_token_intern(ai->pool, bl);
    if (!A || !B || A == B) return NULL;
    /* §6.218 **地点是独占的**（`::stats` 的测量：在=独占 单24/多0）⇒ 新地点**替掉**旧地点：
     * 人不会同时在两个地方。只清**关系位**（共现边留着），并如实说出撤掉了哪条。
     * 注意：这条"后说的地点替换先说的"只在**地点**上做 —— `属于` 有嵌套（动物/哺乳动物），
     * 不能照搬。 */
    size_t retracted = 0;
    char old_place[48]; old_place[0] = 0;
    {
        uint16_t bit = REL_BIT(REL_AT);
        for (size_t i = 0; i < A->count; ++i) {
            AssocEntry *e = &A->assocs[i];
            if (!e->main || e->main == B) continue;
            if (!(e->rel & bit)) continue;
            e->rel &= (uint16_t)~bit;
            if (!old_place[0] && e->main->name)
                snprintf(old_place, sizeof old_place, "%s", e->main->name);
            retracted++;
        }
    }
    if (ai_write_rel(ai, A, B, REL_AT, 0, AI_WSRC_RELATION) != 0) return NULL;
    ai->n_rel_learned++;
    /* 把"没记"的那部分如实说出来（A、`在`、地点之外还有词就说） */
    char dropped[64]; size_t dl = 0; dropped[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == 0 || i == at) continue;
        if (i >= at + 1 && i < b_end) continue;      /* 地点那一段不算"没记" */
        size_t wl = strlen(words[i]);
        if (dl + wl + 2 >= sizeof dropped) break;
        if (dl) dropped[dl++] = ' ';
        memcpy(dropped + dl, words[i], wl); dl += wl; dropped[dl] = 0;
    }
    char buf[224];
    if (retracted && old_place[0])
        snprintf(buf, sizeof buf, "记下 了 ： %s 在 %s（先前 记 的 %s 在 %s 撤 掉 了 ——"
                 " 一个人 不 会 同时 在 两 个 地方）%s", words[0], bl, words[0], old_place,
                 dropped[0] ? "；动词 那 半 句 我 没 记 关系" : "");
    else if (dropped[0])
        snprintf(buf, sizeof buf, "记下 了 ： %s 在 %s（「%s」这 半 句 我 没 记 关系）",
                 words[0], bl, dropped);
    else
        snprintf(buf, sizeof buf, "记下 了 ： %s 在 %s", words[0], bl);
    if (out_conf) *out_conf = 0.7f;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

/* §6.244 **并列词归一**：分词器把 `以及` 切成 以/及、`还有` 切成 还/有 ⇒ 先合成一个 token，
 * 后面的并列识别只认单 token（`以及`）。不分配内存：指向原数组或字面量。 */
static inline size_t agent_norm_conj(char *const *words, size_t n, char **out, size_t cap) {
    size_t m = 0;
    for (size_t i = 0; i < n && m + 1 < cap; ++i) {
        if (!strcmp(words[i], "以") && i + 1 < n && !strcmp(words[i + 1], "及")) {
            out[m++] = (char *)"以及"; i++;
        } else if (!strcmp(words[i], "还") && i + 1 < n && !strcmp(words[i + 1], "有") &&
                   !(i + 2 < n && !strcmp(words[i + 2], "的"))) {   /* 「还有的」不算并列 */
            out[m++] = (char *)"还有"; i++;
        } else {
            out[m++] = words[i];
        }
    }
    return m;
}

/* §6.256 **天气/预报类问题**：没有数据就**不编**。
 * 实测 `明天 会 下雨 吗` 答出「应该，明天 可能 会 下雨」—— 这是**编**（没有天气数据、也没有联网）。
 * 白箱原则：宁可说"不知道"，也不能给出听起来像知识的话。 */
/* 去掉空格的小写副本（判据要同时认「你 叫 什么」和「你叫什么」两种写法 —— 实测混着来）*/
static inline int agent_nosp(const char *in, char *buf, size_t cap) {
    if (!in || !buf || cap == 0) return 0;
    size_t o = 0;
    for (const char *q = in; *q && o + 1 < cap; ++q) {
        if (*q == ' ' || *q == '\t') continue;
        buf[o++] = *q;
    }
    buf[o] = 0;
    return (int)o;
}

static inline int agent_weather_unknown(const char *input) {
    if (!input) return 0;
    char b[256];
    if (!agent_nosp(input, b, sizeof b)) return 0;
    int has_weather = strstr(b, "天气") || strstr(b, "下雨") || strstr(b, "下雪") ||
                      strstr(b, "气温") || strstr(b, "温度") || strstr(b, "晴");
    if (!has_weather) return 0;
    /* §6.269 请求式问法（「帮你 查 一下 北京 天气」）也要认：以前只认时间词/吗，
     * 实测这句掉进「这个我 不 太 懂」。 */
    return strstr(b, "明天") || strstr(b, "今天") || strstr(b, "现在") ||
           strstr(b, "未来") || strstr(b, "外面") || strstr(b, "会不会") || strstr(b, "吗") ||
           strstr(b, "查") || strstr(b, "看看") || strstr(b, "怎么样") ||
           strstr(b, "如何") || strstr(b, "预报");
}

/* §6.258 **自述身份**：统一成一句，别再各说各的。
 * 实测 `你 叫 什么 名字` ⇒「我 是 一 直 在 听 你 说 的 人」、`你 是 谁` ⇒「我 是 你 的 伙伴」、
 * `你 叫 什么` ⇒「我 是 一 直 在 学 的 东西」—— 三句三个说法，且都没说**它是什么**。 */
/* §6.259 **"你能做什么"** —— 用户最常问的一句，也是"可用"的门面。
 * 以前回「我 一直 在 这里 …」（闲聊模板）。现在给一份**短、准、带例子**的能力清单，
 * 并且明说做不到的（不联网/不会算术/不知道天气）。 */
static inline int agent_self_capability(const char *input) {
    if (!input) return 0;
    char b[256];
    if (!agent_nosp(input, b, sizeof b)) return 0;
    return strstr(b, "你能做什么") || strstr(b, "你会什么") || strstr(b, "你能干什么") ||
           strstr(b, "你能干嘛") || strstr(b, "你有什么用") || strstr(b, "你能帮我做什么") ||
           strstr(b, "你能做什么事");
}

/* §6.260 **关于"我"的几个具体问题**：语言/年龄/位置/出处/感情。
 * 这些都不是图里的知识，而是**实现事实**；以前分别答成
 * 「就三件事…」（语言被打成了"能做什么"）、「我 没 有 生 日 但 我 可 以 有」（年龄）、
 * 「这个我 不 太 懂」（住哪/谁做的）、「你 有 感情」（感情，还把主语搞反了）。 */
/* §6.266 **"为什么 A R B"**：白箱项目的看家本事应该能在自然语言里用 ——
 * 以前 `为什么猫属于动物` ⇒「这个我 不 太 懂」（只有 `::why` 命令能看依据）。
 * 做法：剥掉"为什么"、按 A/R/B 解析、用 `rel_solve` 找证明链（正反都试），再用 `proof_text` 说出来。 */
static inline char *agent_why_question(AI *ai, char *const *words, size_t n,
                                       float *out_conf, int *handled) {
    if (handled) *handled = 0;
    if (!ai || !words || n < 4 || n > 8) return NULL;
    size_t wi = n;
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(words[i], "为什么") || !strcmp(words[i], "为啥") ||
            !strcmp(words[i], "为何")) { wi = i; break; }
    if (wi == n) return NULL;
    /* 拼出 A R B（去掉"为什么"这个 token；"不"留在关系词前会被 rel_parse 认）*/
    char *rest[8]; size_t rn = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == wi) continue;
        rest[rn++] = words[i];
    }
    if (rn < 3) return NULL;
    /* 去掉尾巴上的"呢/吗" */
    while (rn > 3 && (!strcmp(rest[rn - 1], "呢") || !strcmp(rest[rn - 1], "吗"))) rn--;
    char jb[64];
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int neg = 0, q = 0;
    if (!rel_parse(ai, rest, rn, jb, sizeof jb, &aw, &rid, &neg, &bw, &q, &rw, 0)) return NULL;
    if (!aw || !bw || !rid) return NULL;
    Token *A = pool_find(ai->pool, aw), *B = pool_find(ai->pool, bw);
    Proof pf, pf2;
    char line[320], buf[640];
    /* ⚠ `Proof.status` 的语义（实测确认）：它是**求解出来的那条断言的极性** ——
     *   +1 = 「A R B」成立；-1 = 「A 不 R B」成立；0 = 推不出（开放世界）。
     * 不是"问句真假"。踩过的两个坑：① 把 `status != 0` 当"成立"⇒「为什么猫属于植物」答成
     * "因为猫属于植物"；② 只问正极性 ⇒ 多值关系（会）里"已知 A 不 R B"时正极性给 0，
     * 必须再问一次 `want_neg=1` 才拿到 -1。 */
    const char *neg_txt = neg ? "不 " : "";
    const char *opp_txt = neg ? "" : "不 ";
    int answered = 0;
    int got_pol = 0;
    if (A && B) {
        rel_solve(ai, A, rid, B, 0, &pf);
        if (pf.status != 0) got_pol = pf.status;
        else {
            rel_solve(ai, A, rid, B, 1, &pf2);
            if (pf2.status != 0) { pf = pf2; got_pol = pf2.status; }
        }
        if (got_pol != 0) {
            int want = neg ? -1 : 1;
            proof_text(ai, &pf, line, sizeof line);
            if (got_pol == want)
                snprintf(buf, sizeof buf, "因为 %s %s%s %s —— %s：%s", aw, neg_txt, rw, bw,
                         pf.n > 2 ? "多 跳 推 导" : "直接 记 着 的 事实", line);
            else
                snprintf(buf, sizeof buf, "其实 是 反 的：%s %s%s %s —— %s：%s", aw, opp_txt, rw, bw,
                         pf.n > 2 ? "多 跳 推 导" : "直接 记 着 的 事实", line);
            answered = 1;
        }
    }
    if (!answered) {
        if (A && B && rel_guess(ai, A, rid, B, &pf)) {
            proof_text(ai, &pf, line, sizeof line);
            snprintf(buf, sizeof buf, "我 没有 直接 依据，只 能 猜：%s（推测，不 是 知识）", line);
        } else {
            snprintf(buf, sizeof buf, "「%s %s%s %s」这 条 我 图 里 没 有 依据 —— 推 不 出 来。",
                     aw, neg_txt, rw, bw);
        }
    }
    if (out_conf) *out_conf = 0.7f;
    if (handled) *handled = 1;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

/* §6.268 **省略问句「那狗呢」/「狗呢」**：把上一轮问句的（关系, 极性, 宾语）搬过来，
 * 只换主语 —— 实测 `猫不会飞吗` 之后 `那狗呢` ⇒「应该，我 还 没 学 过 这 个」（没接住）。
 * 返回 1 表示重写过（out 里是新的输入串）。 */
static inline int agent_rewrite_ellipsis(const AI *ai, const char *in, char *out, size_t cap) {
    if (!ai || !in || !out || cap == 0) return 0;
    if (!ai->last_q_rid || !ai->last_q_b[0]) return 0;
    char c[128];
    if (!agent_nosp(in, c, sizeof c)) return 0;
    size_t cl = strlen(c);
    if (cl < 5 || strcmp(c + cl - 3, "呢") != 0) return 0;     /* 以「呢」结尾（3 字节）*/
    cl -= 3;
    if (cl == 0 || cl > 36) return 0;
    char subj[40];
    size_t sl = 0;
    const char *p = c;
    if (cl >= 6 && strncmp(p, "那", 3) == 0) { p += 3; cl -= 3; }   /* 「那狗呢」去掉「那」*/
    if (cl == 0 || cl > sizeof subj - 1) return 0;
    memcpy(subj, p, cl); subj[cl] = 0; sl = cl;
    (void)sl;
    const char *rn = rel_name_of(ai, ai->last_q_rid);
    if (!rn) return 0;
    snprintf(out, cap, "%s %s%s %s 吗", subj, ai->last_q_neg ? "不 " : "", rn, ai->last_q_b);
    return 1;
}

/* §6.262 **不带空格的"在哪"问句要在分词前拆开**：`我在哪` 分词成 [我在][哪]（`我在` 是
 * 词池里的最大匹配），关系层只认 `[A][在][什么]` ⇒ 整句掉兜底。这里在**字符串层**重写成
 * `A 在 什么`（带空格），不动 token 数组（往 token 数组里插指针会破坏 free 的所有权）。
 * `我住哪` 同理（住的地方就是地点）。 */
static inline int agent_rewrite_where(const char *in, char *out, size_t cap) {
    if (!in || !out || cap == 0) return 0;
    char c[192];
    if (!agent_nosp(in, c, sizeof c)) return 0;
    size_t cl = strlen(c);
    int has_ma = 0;
    if (cl >= 3 && strcmp(c + cl - 3, "吗") == 0) { has_ma = 1; cl -= 3; }
    size_t pos = (size_t)-1;
    static const char *pat[] = {"住在哪里", "住在哪儿", "住在哪", "在哪里", "在哪儿", "在哪",
                                "住哪里", "住哪儿", "住哪", NULL};
    for (size_t i = 0; pat[i]; ++i) {
        size_t pl = strlen(pat[i]);
        if (cl < pl) continue;
        if (strcmp(c + cl - pl, pat[i]) != 0) continue;
        pos = cl - pl;
        break;
    }
    if (pos == (size_t)-1 || pos == 0) return 0;
    c[pos] = 0;
    snprintf(out, cap, "%s 在 什么%s", c, has_ma ? " 吗" : "");
    return 1;
}

/* §6.267 **做不到的"功能请求"要明说**：`说点别的` / `讲个笑话` / `唱首歌` 这类，
 * 以前会掉进生成流水线出词串（实测「的 想 出来 在 哭 会」「话 有 每 在 上 人 我们」）。 */
static inline const char *agent_unsupported_request(const char *input) {
    if (!input) return NULL;
    char b[256];
    if (!agent_nosp(input, b, sizeof b)) return NULL;
    if (strstr(b, "讲个笑话") || strstr(b, "说个笑话") || strstr(b, "讲笑话") ||
        strstr(b, "说笑话") || strstr(b, "讲个故事") || strstr(b, "唱首歌") ||
        strstr(b, "唱个歌") || strstr(b, "说点别的") || strstr(b, "换个话题") ||
        strstr(b, "聊点别的"))
        return "这 个 我 真 不 会 —— 我 只 能 回答 图 里 记 着 的 关系、记 你 教 我 的 东西，"
               "不 会 讲 笑 话/唱 歌。你 想 聊 知识 的话 我 在。";
    /* §6.269 其它**生成类**请求（写诗/画画/写代码/翻译）：实测「你 会 写 诗 吗」⇒
     * 「我 会 试 着 写诗」（把"你会写诗"当关系猜了）。只在**请求口气**里认，
     * 免得把知识句（「画画 属于 艺术」）当成请求。 */
    {
        int ask = strstr(b, "你") || strstr(b, "帮我") || strstr(b, "给 我") ||
                  strstr(b, "吗") || strstr(b, "吧") || strstr(b, "？") || strstr(b, "?");
        if (ask && (strstr(b, "写诗") || strstr(b, "作诗") || strstr(b, "写歌") ||
                    strstr(b, "写 首") || strstr(b, "画画") || strstr(b, "画张") ||
                    strstr(b, "画 个") || strstr(b, "写代码") || strstr(b, "编程") ||
                    strstr(b, "翻译") || strstr(b, "写作文")))
            return "这 个 我 真 不 会 —— 我 只 能 回答 图 里 记 着 的 关系、记 你 教 我 的 东西，"
                   "不 会 写 诗/画 画/翻 译 这 类 生 成。你 想 聊 知识 的话 我 在。";
    }
    return NULL;
}

/* §6.269 **规则句不是事实**：`教你 会飞 的 动物 是 鸟` / `教你 猫 的 主人 是 小明` 这类
 * 「…的…是…」是**规则/定义**，本项目的学习链只认「A 关系 B」这一种。以前它掉进生成流水线，
 * 答出词串（实测「鸟 睡觉 吗 只 你 开」）。这里在**剥掉引导语之后**认出来并明说学不进，
 * 同时给出可行的教法（先教两条事实，多跳推理会用上）。只对**带引导语的教法**生效，
 * 不碰普通陈述句。`名字`句（「我 的 名字 是 小明」）由名字那条路管，先放过。 */
static inline int agent_rule_teach(const char *s) {
    if (!s) return 0;
    char b[200];
    if (!agent_nosp(s, b, sizeof b)) return 0;
    if (!strstr(b, "的") || !strstr(b, "是")) return 0;
    if (strstr(b, "名字") || strstr(b, "叫")) return 0;
    if (strstr(b, "吗") || strstr(b, "呢") || strstr(b, "？") || strstr(b, "?")) return 0;
    return 1;
}

/* §6.269 **「你 确定 吗」**：用户对我刚给的说法要一句保证。以前掉闲聊
 * （实测答「我 没 什么 事 我 有 空」，紧接着「再 说 一 遍」就把这句废话复读一遍）。
 * 这里给的是**可核查**的保证（能追、能改），不吹"绝对对"。 */
static inline const char *agent_confirm_request(const char *input) {
    if (!input) return NULL;
    char b[200];
    if (!agent_nosp(input, b, sizeof b)) return NULL;
    int ask = strstr(b, "你确定") || strstr(b, "你肯定") || strstr(b, "真的吗") ||
              strstr(b, "真的假的") || strstr(b, "没骗我") || strstr(b, "确定吗");
    if (!ask) return NULL;
    return "我 说 的 都 是 图 里 记 着 的，每 条 都 能 追 —— 想 看 依据 就 问「为什么 …」；"
           "我 说 错 了 你 说「不 对」，我 就 改。";
}

/* §6.270 **量/尺度问题**（「太阳 有 多大」「这 条 路 多 远」）：分词把「多大」切成两个单字，
 * 疑问词那关看不出它是问句（逐词检查只认整词）⇒ 不走 `ai_has_question_word` 那条拒答，
 * 掉进逐词游走（实测答「大 了」）。这里按**原始字符串**认这一类，并明说记不了数量。 */
static inline const char *agent_measure_question(const char *input) {
    if (!input) return NULL;
    char b[200];
    if (!agent_nosp(input, b, sizeof b)) return NULL;
    static const char *mk[] = {"有多大", "多高", "多重", "多远", "多久", "多长",
                               "多快", "多贵", "多深", "多宽", "多厚", NULL};
    for (size_t i = 0; mk[i]; ++i)
        if (strstr(b, mk[i]))
            return "具 体 数 量 我 不 会 量 —— 我 只 记「A 关系 B」的 事实，不 记 尺 度 和 数 字（不 编）。";
    return NULL;
}

/* §6.270 **对话胶水**（不好意思 / 你觉得呢 / 然后呢 / 你记得我吗…）：
 * 这些不是知识问题，以前大多掉生成流水线出词串（实测「意思 别 上 谁 有 样 时候」、
 * 「你 当 时 也 是 没 办 法」）。原则：**能答实的就答**（能记住/记得住），
 * 答不了的**明说不编**（没有看法、不记人）—— 比闲聊模板有用，也不假装理解。 */
static inline const char *agent_social_glue(const char *input) {
    if (!input) return NULL;
    char b[200];
    if (!agent_nosp(input, b, sizeof b)) return NULL;
    if (strstr(b, "不好意思") || strstr(b, "对不起") || strstr(b, "抱歉"))
        return "没 关系 —— 我 不 记 这 个，只 记 你 教 我 的 知识。";
    if (strstr(b, "你觉得呢") || strstr(b, "你说呢") || strstr(b, "你怎么看") ||
        strstr(b, "你的看法"))
        return "我 没有 看法 —— 我 只 会 说 图 里 记 着 的。你 把 问 题 说 清，我 就 查。";
    if (strstr(b, "你记得我吗") || strstr(b, "你还记得我"))
        return "我 不 记 得 你 —— 我 只 记 图 里 的 知识，不 记 人（不 编）。";
    if (strstr(b, "你能记住吗") || strstr(b, "你记得住吗") || strstr(b, "你能记吗"))
        return "能 —— 你 说 一 句「考拉 属于 动物」，我 就 记 住；存盘 之后 下次 还 在。";
    if (strstr(b, "我不知道") || strcmp(b, "不知道") == 0 || strstr(b, "我也不确定") ||
        strcmp(b, "不确定") == 0)
        return "没 事 —— 你 问 的 我 也 不 都 知道。你 说 个 词 或 一 句 话，我 查 图，或者 记 下 你 教 的。";
    if (strstr(b, "你听懂了吗") || strstr(b, "你听明白了吗") || strstr(b, "你理解了吗"))
        return "我 只 能 按 图 里 的 关系 理解 —— 你 再 说 一 遍，或者 换 个 说 法，我 再 试。";
    if (strstr(b, "这个怎么说") || strstr(b, "这怎么说") || strstr(b, "怎么念"))
        return "你 是 想 问 某 个 词 怎么 说 吧 —— 把 那个 词 说 出 来，我 查 图，或者 你 教 我 一 句。";
    if (strstr(b, "你好吗") || strstr(b, "你还好吗") || strstr(b, "最近怎么样") ||
        strcmp(b, "在吗") == 0 || strstr(b, "在不在"))
        return "我 挺 好 的 —— 一直 在 这儿。你 想 查 什 么，或者 教 我 一 句 都 行。";
    if (strstr(b, "然后呢") || strstr(b, "还有呢") || strstr(b, "再来一个") ||
        strstr(b, "换个问题") || strstr(b, "换个说法") || strstr(b, "接下来呢"))
        return "你 想 问 哪 个 词 / 哪 件 事 ？ 我 记 得 的 都 能 追（也 可 以 先 教 我 一 句）。";
    return NULL;
}

static inline const char *agent_self_fact(const char *input) {
    if (!input) return NULL;
    char b[256];
    if (!agent_nosp(input, b, sizeof b)) return NULL;
    if (strstr(b, "什么语言") || strstr(b, "哪国语言") || strstr(b, "哪些语言"))
        return "现在 只 有 中文（分词 + 关系 图 都 是 中文 的）。";
    if (strstr(b, "你多大") || strstr(b, "你几岁") || strstr(b, "你的年龄"))
        return "我 没 有 年龄 —— 我 是 一 个 程序，不 会 长 大；"
               "「多 大」对 我 只 能 理解 成「模型 里 有 多少 东西」，那 个 你 可 以 让 我 报 统计。";
    if (strstr(b, "你住在哪") || strstr(b, "你在哪住") || strstr(b, "你家在哪"))
        return "我 没有 物理 位置 —— 我 跑 在 这 个 进程 里。";
    if (strstr(b, "谁做的你") || strstr(b, "谁写的你") || strstr(b, "谁创造了你") ||
        strstr(b, "谁开发你"))
        return "我 是 一 个 C11 单 头 文件 的 实现（JainkAI）：知识 在 图 里，回答 全 部 可 追。";
    if (strstr(b, "你有感情吗") || strstr(b, "你有情绪吗") || strstr(b, "你会难过吗"))
        return "没有 —— 我 没有 感情。不过 我 会 记 住 你 说 过 的 事，也 会 在 你 难过 的 时候 少 说 废话。";
    return NULL;
}

static inline int agent_self_identity(const char *input) {
    if (!input) return 0;
    char b[256];
    if (!agent_nosp(input, b, sizeof b)) return 0;
    return strstr(b, "你叫什么") || strstr(b, "你的名字") || strstr(b, "你是谁") ||
           strstr(b, "你是什么") || strstr(b, "你叫啥");
}

/* §6.258b **日期/时刻**：没有时钟就明说（与天气同一条原则）*/
static inline int agent_clock_unknown(const char *input) {
    if (!input) return 0;
    char b[256];
    if (!agent_nosp(input, b, sizeof b)) return 0;
    int ask = strstr(b, "几号") || strstr(b, "几点") || strstr(b, "星期几") ||
              strstr(b, "礼拜几") || strstr(b, "哪一年") || strstr(b, "几月");
    return ask && (strstr(b, "今天") || strstr(b, "现在") || strstr(b, "明天") ||
                   strstr(b, "昨天") || strstr(b, "今年") || strstr(b, "当前"));
}

/* §6.257 **能力边界要如实说**（与 §6.256 同一条原则：不会就说不会，别用闲聊模板糊过去）。
 * 实测：`你 上 网 吗` ⇒「这个我 不 太 懂」；`一 加 一 等于 几` ⇒「别 硬 撑 我 陪 你」——
 * 都不是"不会"的明说。这两件事是**确定没有**的能力，明说比含糊更诚实。 */
static inline const char *agent_capability_unknown(const char *input) {
    if (!input) return NULL;
    if (strstr(input, "上 网") || strstr(input, "上网") || strstr(input, "联网") ||
        strstr(input, "联 网") || strstr(input, "搜索") || strstr(input, "百度") ||
        strstr(input, "谷歌") || strstr(input, "查 一下 网"))
        return "我 不 联 网，也 没 有 实时 数据 —— 只 能 用 我 自己 图 里 的 东西 回答。";
    if (strstr(input, "等于") &&
        (strstr(input, "加") || strstr(input, "减") || strstr(input, "乘") ||
         strstr(input, "除") || strstr(input, "＋") || strstr(input, "＋") ||
         strstr(input, "+") || strstr(input, "-")))
        return "算 术 我 不 会（没 有 算术 能力）—— 这 个数 你 自己 算 更 快，也 不 会 错。";
    /* §6.269 「你 会 算术 吗」以前**只靠 `等于` 认**，所以这句掉进猜测 ⇒ 答
     * 「你 应该 会 算术」（还把主语当成知识问题猜了）。能力问句带 你/吗 时明说不会；
     * `算术 属于 数学` 这种知识句（没有 你/吗）不走这儿。 */
    if ((strstr(input, "算术") || strstr(input, "算数") || strstr(input, "算 术")) &&
        (strstr(input, "你") || strstr(input, "吗") || strstr(input, "帮我") ||
         strstr(input, "能给 我")))
        return "算 术 我 不 会（没 有 算术 能力）—— 这 个数 你 自己 算 更 快，也 不 会 错。";
    return NULL;
}

/* §6.254 **「忘 掉 X R Y」/「删 掉 X R Y」**：认出"要求删除"并**如实说明删不了**。
 * 为什么不做删除：`micro.h` 的 `MOD_REMOVE_ASSOC` 里有一条**硬底线** ——
 * 观测到的关系边不许被剪（`micro_edge_removable()` 只放行 DERIVED 缓存）。
 * 所以正确行为是：说清规则 + 给出可行的替代（改口 `X 不 R Y` 或 `::rel X` 查看）。 */
static inline char *agent_forget_request(AI *ai, char *const *words, size_t n, float *out_conf) {
    if (!ai || !words || n < 4) return NULL;
    static const char *mk[] = {"忘", "忘掉", "忘记", "删", "删掉", "删除", "别记", "别",
                               "清楚", "清掉", NULL};
    size_t off = 0;
    int hit = 0;
    for (; off < n && off < 3; ++off) {
        for (size_t i = 0; mk[i]; ++i)
            if (!strcmp(words[off], mk[i])) { hit = 1; break; }
        if (hit) break;
    }
    if (!hit) return NULL;
    size_t rest = off + 1;
    {   /* 「忘 掉 …」/「删 掉 …」/「别 记 …」里的"掉/记"要跳过 */
        static const char *skip[] = {"掉", "记", "了", "掉 了", NULL};
        for (size_t i = 0; skip[i] && rest < n; ++i)
            if (!strcmp(words[rest], skip[i])) { rest++; break; }
    }
    if (n - rest < 3) return NULL;
    char jb[64];
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int neg = 0, q = 0;
    if (!rel_parse(ai, words + rest, n - rest, jb, sizeof jb, &aw, &rid, &neg, &bw, &q, &rw, 0))
        return NULL;
    if (q || !aw || !bw || !rid) return NULL;
    Token *A = pool_find(ai->pool, aw), *B = pool_find(ai->pool, bw);
    int have = A && B && (neg ? rel_direct_neg(A, rid, B) : rel_direct(A, rid, B));
    char buf[320];
    if (have)
        snprintf(buf, sizeof buf,
                 "这 条 我 删 不 了 —— 观测 到 的 知识 不 许 剪（硬 底线）。"
                 "如果 你 的 意思 是「%s 不 %s %s」，就 这么 说，我 会 当「改口」记 下；"
                 "想 看 我 到底 记 了 什么 可以 查 一下。",
                 aw, rw, bw);
    else
        snprintf(buf, sizeof buf, "我 图 里 没 有「%s %s %s」，不 用 删。", aw, rw, bw);
    if (out_conf) *out_conf = 0.6f;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

/* §6.239 **「A 和 B 都 R C」**（并列主语）：学的时候拆成两条，问的时候两边都查。
 * 实测 `猫 和 狗 都 是 动物 吗` 以前掉兜底（「这个我 不 太 懂」）—— 并列主语没接住。 */
static inline char *agent_conj_both(AI *ai, char *const *w0, size_t n0, int *out_is_q,
                                    float *out_conf) {
    if (!ai || ai->rel_off || !w0 || n0 < 5) return NULL;
    char *nwb[24];
    size_t n = agent_norm_conj(w0, n0, nwb, sizeof nwb / sizeof nwb[0]);
    char *const *words = nwb;
    size_t last = n;
    int is_q = 0;
    while (last > 0 && (!strcmp(words[last - 1], "吗") || !strcmp(words[last - 1], "呢"))) {
        last--; is_q = 1;
    }
    /* 形状：`<A…> 和 <B…> [都] R <C…>`（三段都允许被分词切碎后拼回来）*/
    if (last < 4) return NULL;
    size_t p = 0;
    for (size_t i = 1; i < last; ++i)
        if (!strcmp(words[i], "和") || !strcmp(words[i], "跟") || !strcmp(words[i], "与") ||
            !strcmp(words[i], "以及") || !strcmp(words[i], "还有") ||
            !strcmp(words[i], "、")) { p = i; break; }
    if (!p) return NULL;
    size_t d = 0;
    for (size_t i = p + 1; i + 1 < last; ++i) if (!strcmp(words[i], "都")) { d = i; break; }
    size_t ri = (d ? d + 1 : p + 1);
    if (ri + 1 > last) return NULL;
    char aw[32], bw[32], cw[32];
    size_t al = 0, bl = 0, cl = 0;
    for (size_t i = 0; i < p; ++i) {
        size_t wl = strlen(words[i]);
        if (al + wl + 1 >= sizeof aw) return NULL;
        memcpy(aw + al, words[i], wl); al += wl;
    }
    aw[al] = 0;
    for (size_t i = p + 1; i < (d ? d : p + 1); ++i) {
        size_t wl = strlen(words[i]);
        if (bl + wl + 1 >= sizeof bw) return NULL;
        memcpy(bw + bl, words[i], wl); bl += wl;
    }
    bw[bl] = 0;
    for (size_t i = ri + 1; i < last; ++i) {
        size_t wl = strlen(words[i]);
        if (cl + wl + 1 >= sizeof cw) return NULL;
        memcpy(cw + cl, words[i], wl); cl += wl;
    }
    cw[cl] = 0;
    if (!al || !bl || !cl) return NULL;
    const char *rw = words[ri];
    if (!agent_subj_ok(aw) || !agent_subj_ok(bw)) return NULL;
    if (!agent_obj_ok(cw)) return NULL;
    uint8_t rid = rel_id_of(ai, rw, 0);
    if (!rid && strcmp(rw, "是") == 0) rid = REL_IS_A;
    if (!rid) return NULL;
    Token *A = pool_find(ai->pool, aw), *B = pool_find(ai->pool, bw), *C = pool_find(ai->pool, cw);
    char buf[224];
    if (!is_q) {
        if (!C) return NULL;
        if (!A && !B) return NULL;
        /* §6.247 **只报真的写进去的**（与 §6.247 的 conj_obj 同一条纪律）：
         * 词池里没有的那个主语不能算"记下了"。 */
        int wa = 0, wb = 0;
        if (A) wa = (ai_write_rel(ai, A, C, rid, 0, AI_WSRC_RELATION) == 0);
        if (B) wb = (ai_write_rel(ai, B, C, rid, 0, AI_WSRC_RELATION) == 0);
        if (!wa && !wb) return NULL;
        ai->n_rel_learned++;
        if (wa && wb)
            snprintf(buf, sizeof buf, "记下 了 ： %s 和 %s 都 %s %s", aw, bw, rw, cw);
        else if (wa)
            snprintf(buf, sizeof buf, "记下 了 ： %s %s %s（%s 我 还 不 认识）", aw, rw, cw, bw);
        else
            snprintf(buf, sizeof buf, "记下 了 ： %s %s %s（%s 我 还 不 认识）", bw, rw, cw, aw);
        if (out_conf) *out_conf = 0.75f;
    } else {
        if (!C) return NULL;
        int na = A ? (rel_direct(A, rid, C) ? 1 : (rel_direct_neg(A, rid, C) ? -1 : 0)) : 0;
        int nb = B ? (rel_direct(B, rid, C) ? 1 : (rel_direct_neg(B, rid, C) ? -1 : 0)) : 0;
        if (na == 1 && nb == 1)
            snprintf(buf, sizeof buf, "对 ， %s 和 %s 都 %s %s", aw, bw, rw, cw);
        else if (na == -1 || nb == -1)
            snprintf(buf, sizeof buf, "不 对 ： %s 不 %s %s", (na == -1) ? aw : bw, rw, cw);
        else
            snprintf(buf, sizeof buf, "我 不 确定 ： %s 和 %s 里 我 只 知道 一边（%s %s）",
                     aw, bw, rw, cw);
        if (out_conf) *out_conf = (na == 1 && nb == 1) ? 0.85f : 0.5f;
    }
    if (out_is_q) *out_is_q = is_q;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

/* §6.240 **并列宾语「A R B1 和 B2」**：实测 `我 喜欢 猫 和 狗` 以前掉流水线
 * （答出「狗 睡觉 吗 喜欢 地 事情 点」）。与 §6.239 对称：学的时候拆两条、问的时候两边查。 */
/* §6.245 **选择问句「A R B1 或 B2 吗」**：选择不是并列，不能拆成两条学；
 * 但问的时候可以**分别查**：只要有一边成立就答「对」并说清是哪边。 */
static inline char *agent_or_ask(AI *ai, char *const *words, size_t n, int *out_is_q,
                                 float *out_conf) {
    if (!ai || ai->rel_off || !words || n < 5) return NULL;
    size_t last = n;
    int is_q = 0;
    while (last > 0 && (!strcmp(words[last - 1], "吗") || !strcmp(words[last - 1], "呢"))) {
        last--; is_q = 1;
    }
    if (last < 5) return NULL;
    /* 选择问句的判据：带「吗/呢」或用了「还是」。**「或」不带「吗」不一定是问句**
     * （`我 喜欢 猫 或 狗` 是陈述，不能按问句答 —— 实测就是这么错的）。 */
    uint8_t rid = rel_id_of(ai, words[1], 0);
    if (!rid) return NULL;
    size_t p = 0;
    for (size_t i = 2; i + 1 < last; ++i) {
        if (!strcmp(words[i], "或") || !strcmp(words[i], "或者") ||
            !strcmp(words[i], "还是")) {
            p = i;
            if (!strcmp(words[i], "还是")) is_q = 1;       /* 「还是」本身即问句标记 */
            break;
        }
    }
    if (!p || !is_q) return NULL;
    const char *aw = words[0];
    if (!agent_subj_ok(aw)) return NULL;
    char bw[32], cw[32];
    size_t bl = 0, cl = 0;
    for (size_t i = 2; i < p; ++i) {
        size_t wl = strlen(words[i]);
        if (bl + wl + 1 >= sizeof bw) return NULL;
        memcpy(bw + bl, words[i], wl); bl += wl;
    }
    bw[bl] = 0;
    for (size_t i = p + 1; i < last; ++i) {
        size_t wl = strlen(words[i]);
        if (cl + wl + 1 >= sizeof cw) return NULL;
        memcpy(cw + cl, words[i], wl); cl += wl;
    }
    cw[cl] = 0;
    if (!bl || !cl) return NULL;
    if (!agent_obj_ok(bw) || !agent_obj_ok(cw)) return NULL;
    Token *A = pool_find(ai->pool, aw), *B = pool_find(ai->pool, bw), *C = pool_find(ai->pool, cw);
    int vb = B ? (rel_direct(A, rid, B) ? 1 : (rel_direct_neg(A, rid, B) ? -1 : 0)) : 0;
    int vc = C ? (rel_direct(A, rid, C) ? 1 : (rel_direct_neg(A, rid, C) ? -1 : 0)) : 0;
    char buf[200];
    if (vb == 1)
        snprintf(buf, sizeof buf, "对 ： %s %s %s（不 是 %s）", aw, words[1], bw, cw);
    else if (vc == 1)
        snprintf(buf, sizeof buf, "对 ： %s %s %s（不 是 %s）", aw, words[1], cw, bw);
    else if (vb == -1 && vc == -1)
        snprintf(buf, sizeof buf, "都 不 对 ： %s 既 不 %s %s 也 不 %s %s", aw, words[1], bw, words[1], cw);
    else
        snprintf(buf, sizeof buf, "我 不 确定 你 指 哪 个 ： %s %s %s / %s", aw, words[1], bw, cw);
    if (out_conf) *out_conf = (vb == 1 || vc == 1) ? 0.8f : 0.45f;
    if (out_is_q) *out_is_q = 1;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

static inline char *agent_conj_obj(AI *ai, char *const *w0, size_t n0, int *out_is_q,
                                   float *out_conf) {
    if (!ai || ai->rel_off || !w0 || n0 < 5) return NULL;
    char *nwb[24];
    size_t n = agent_norm_conj(w0, n0, nwb, sizeof nwb / sizeof nwb[0]);
    char *const *words = nwb;
    size_t last = n;
    int is_q = 0;
    while (last > 0 && (!strcmp(words[last - 1], "吗") || !strcmp(words[last - 1], "呢"))) {
        last--; is_q = 1;
    }
    if (last < 4) return NULL;
    uint8_t rid = rel_id_of(ai, words[1], 0);          /* 关系词固定在第二个 */
    if (!rid) return NULL;
    const char *aw = words[0];
    if (!agent_subj_ok(aw)) return NULL;
    /* §6.240c **任意个并列项**（不再是"切两半"）：`我 喜欢 猫、狗、鱼` 也要拆三条。 */
    enum { MAXI = 6 };
    char items[MAXI][32];
    size_t ni = 0, cl = 0;
    int any = 0;
    for (size_t i = 2; i < last; ++i) {
        int is_sep = (!strcmp(words[i], "和") || !strcmp(words[i], "跟") ||
                      !strcmp(words[i], "与") || !strcmp(words[i], "以及") ||
                      !strcmp(words[i], "还有") || !strcmp(words[i], "、"));
        /* §6.246 **并列谓语**：`A R B1 也 R B2`（`猫 会 爬树 也 会 抓老鼠`）——
         * 「也/都 + 同一个关系词」也算分隔符（跳过重复的那个关系词）。 */
        if (!is_sep && i + 1 < last && (!strcmp(words[i], "也") || !strcmp(words[i], "都")) &&
            !strcmp(words[i + 1], words[1])) {
            is_sep = 1;
            i++;
        }
        /* 「还 有」是**两个 token** 的并列词（分词器不合成一个）*/
        if (!is_sep && !strcmp(words[i], "还") && i + 1 < last && !strcmp(words[i + 1], "有")) {
            is_sep = 1;
            i++;                                   /* 跳过「有」*/
        }
        if (is_sep) {
            if (ni == 0) { items[ni][cl] = 0; if (!cl) return NULL; ni++; cl = 0; }
            else { items[ni][cl] = 0; if (!cl) return NULL; ni++; cl = 0; }
            if (ni >= MAXI) return NULL;
            any = 1;
            continue;
        }
        size_t wl = strlen(words[i]);
        if (!items[ni] || cl + wl + 1 >= sizeof items[0]) return NULL;
        if (cl == 0 && ni > 0) { /* 新项开始 */ }
        memcpy(items[ni] + cl, words[i], wl); cl += wl;
        if (cl + 1 < sizeof items[0]) items[ni][cl] = 0;
    }
    (void)any;
    if (ni == 0) return NULL;
    items[ni][cl] = 0;
    if (!cl) return NULL;
    ni++;
    if (ni < 2) return NULL;
    for (size_t i = 0; i < ni; ++i) if (!agent_obj_ok(items[i])) return NULL;
    Token *A = pool_find(ai->pool, aw);
    char buf[256];
    if (!is_q) {
        if (!A) return NULL;
        /* §6.240b **任一并列项就是主语本身 ⇒ 整句不接**（`猫 属于 猫、狗` 是坏句子，
         * 学一半会留下 `猫 属于 狗` 的垃圾）。 */
        for (size_t i = 0; i < ni; ++i) {
            Token *t = pool_find(ai->pool, items[i]);
            if (t && t == A) return NULL;
        }
        /* §6.247 **只报真的写进去的**：词池里没有的项（`爬树` 被分词切成 爬/树）
         * 不能算"记下了" —— 以前照样报「记下 了 ： 猫 会 爬树、抓老鼠」，再问就"没记过"，
         * 自相矛盾。现在：写进去的列出来，没认出来的单独说。 */
        size_t learned = 0, skipped = 0;
        char wrote[160] = {0}, missed[96] = {0};
        size_t wo = 0, mo = 0;
        for (size_t i = 0; i < ni; ++i) {
            Token *t = pool_find(ai->pool, items[i]);
            if (!t) {
                if (mo + strlen(items[i]) + 4 < sizeof missed)
                    mo += (size_t)snprintf(missed + mo, sizeof missed - mo, "%s%s",
                                           mo ? " 、 " : "", items[i]);
                skipped++;
                continue;
            }
            if (ai_write_rel(ai, A, t, rid, 0, AI_WSRC_RELATION) == 0) {
                if (wo + strlen(items[i]) + 4 < sizeof wrote)
                    wo += (size_t)snprintf(wrote + wo, sizeof wrote - wo, "%s%s",
                                           wo ? " 、 " : "", items[i]);
                learned++;
            }
        }
        if (!learned) return NULL;
        ai->n_rel_learned++;
        if (skipped)
            snprintf(buf, sizeof buf, "记下 了 ： %s %s %s（%s 我 还 不 认识）",
                     aw, words[1], wrote, missed);
        else
            snprintf(buf, sizeof buf, "记下 了 ： %s %s %s", aw, words[1], wrote);
        if (out_conf) *out_conf = 0.75f;
    } else {
        int nfalse = 0, nunk = 0;
        const char *first_false = NULL, *first_unk = NULL;
        for (size_t i = 0; i < ni; ++i) {
            Token *t = pool_find(ai->pool, items[i]);
            int v = t ? (rel_direct(A, rid, t) ? 1 : (rel_direct_neg(A, rid, t) ? -1 : 0)) : 0;
            if (v == 0) { nunk++; if (!first_unk) first_unk = items[i]; }
            else if (v == -1) { nfalse++; if (!first_false) first_false = items[i]; }
        }
        if (nunk == 0 && nfalse == 0)
            snprintf(buf, sizeof buf, "对 ， %s 都 %s 这 %zu 个", aw, words[1], ni);
        else if (nfalse)
            snprintf(buf, sizeof buf, "不 对 ： %s 不 %s %s", aw, words[1], first_false);
        else
            snprintf(buf, sizeof buf, "我 不 确定 ： %s 的 这几 个 里 我 没 记 过 %s %s",
                     aw, words[1], first_unk ? first_unk : "?");
        if (out_conf) *out_conf = (nunk == 0 && nfalse == 0) ? 0.85f : 0.5f;
    }
    if (out_is_q) *out_is_q = is_q;
    char *r = (char *)malloc(strlen(buf) + 1);
    if (r) memcpy(r, buf, strlen(buf) + 1);
    return r;
}

static inline char *agent_learn_relation(AI *ai, char *const *words, size_t n,
                                        float *out_conf) {
    if (!ai || ai->rel_off || !words || n < 3 || n > 6) return NULL;
    /* 「你 是 谁」也是三个词、也用 是 —— 但它显然不是事实陈述。
     * 先挡疑问词：只要句子里有疑问词，一律不当陈述。 */
    /* 疑问词表只有一张（ai.h 的 AI_QUESTION_WORDS）——
     * 以前这里抄了一份、检索和拒答各抄一份，三份还各不相同。 */
    if (ai_has_question_word(words, n)) return NULL;

    /* §6.205 **名字介绍的自然说法**：`我 的 名字 是 小明` / `我 名字 是 小明`
     * （也认「叫」）。它们是同一个意思（`我 叫 小明`，那条本来就能学），
     * 但形状不是"A 关系 B" ⇒ 以前整句掉流水线、答一句"我是你的伙伴"，名字没学进去。
     * 这里**只做形状改写**，后面的学习路径完全不变（动态关系 叫、受审、记账）。 */
    char *norm[3];
    char namebuf[32];
    {
        /* 分词器可能把「名字」切成一个词、也可能切成 名/字（§6.171 那类）；
         * 「小红/小刚」这类名字也常被切成两个单字 ⇒ 这里**按 token 拼回来**。 */
        size_t mi = n; int split_name = 0;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (!words[i]) continue;
            if (strcmp(words[i], "名字") == 0) { mi = i; break; }
            if (words[i + 1] && !strcmp(words[i], "名") && !strcmp(words[i + 1], "字")) {
                mi = i; split_name = 1; break;
            }
        }
        size_t after = (mi < n) ? (split_name ? mi + 2 : mi + 1) : n;
        int prefix_ok = (mi == 1) ||
                        (mi == 2 && words[1] && strcmp(words[1], "的") == 0);
        if (mi < n && prefix_ok && after < n && words[after] &&
            (!strcmp(words[after], "是") || !strcmp(words[after], "叫")) &&
            after + 1 < n) {
            size_t bl = 0; int ok = 1;
            for (size_t i = after + 1; i < n && ok; ++i) {
                size_t wl = words[i] ? strlen(words[i]) : 0;
                if (bl + wl + 1 >= sizeof namebuf) { ok = 0; break; }
                memcpy(namebuf + bl, words[i], wl);
                bl += wl;
            }
            if (ok && bl > 0 && bl <= 12) {
                namebuf[bl] = 0;
                norm[0] = words[0];
                norm[1] = (char *)"叫";
                norm[2] = namebuf;
                words = norm; n = 3;
            }
        }
    }

    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    char jbuf[64];
    /* 登记新谓语是**有副作用**的，所以先只验形状（不碰关系表），
     * 形状对了才允许 rel_parse 去登记 —— 否则「你 觉得 天气 怎么样」
     * 会把「觉得」污染进动态关系表，还写进模型。 */
    if (!rel_shape_ok2(words, n, ai ? ai->rel_nat : 0)) return NULL;   /* §6.163：开着才剥量词 */
    if (!rel_parse(ai, words, n, jbuf, sizeof jbuf, &aw, &rid, &is_neg,
                   &bw, &is_q, &rw, 1)) return NULL;
    if (is_q) return NULL;
    /* §6.171 分词会把「多少钱」拆成 多/少/钱，整词检查在拆开时看不出是问句；
     * `rel_parse` 会把这些单字**拼回整词**当宾语，所以拼回来之后再查一遍。
     * 实测（出厂模型）：不查这一遍，`这个 东西 多 少 钱` 会被学成
     * `这个 --[东西]--> 多少钱`（谓语是「东西」），图里就多了一条垃圾边。 */
    if (ai_token_has_question_word(aw) || ai_token_has_question_word(bw)) return NULL;
    if (!agent_subj_ok(aw)) return NULL;      /* §6.229 主语不能是别的虚词（很/了/的…）*/
    /* §6.240b/§6.244 并列走 conj 那条，别学成「猫、狗」「猫以及狗」这种 blob
     * （顿号是 UTF-8 多字节，必须用 strstr，不是 strchr）*/
    if (strstr(bw, "、") || strstr(bw, "以及")) return NULL;
    /* §6.245 **选择不是并列**：`我 喜欢 猫 或 狗` 不能学成「猫或狗」一个宾语 ——
     * 并列（都成立）与选择（二选一）是**两种语义**，混着学会答错。选择句一律不学。 */
    if (strstr(bw, "或")) return NULL;
    if (!agent_obj_ok(bw)) return NULL;       /* §6.229b 宾语同理（`猫 属于 很` 不该学）*/
    Token *A = pool_token_intern(ai->pool, aw);
    Token *B = pool_token_intern(ai->pool, bw);
    if (!A || !B || A == B) return NULL;

    /* 动态关系（清单外的谓语）：**否定也支持了**（§6.80：否定旗子在 `nrel` 第 15 位）——
     * 以前这里 `if (is_neg) return NULL`，于是「猫 不 喵 狗」整句掉降级答复。 */
    char resp0[192];
    if (rid >= REL_DYN_BASE) {          /* 边界是 REL_DYN_BASE：核心关系现在有 9 个 */
        /* §6.226 **谓语或宾语是虚词 ⇒ 这不是一条关系**。实测垃圾学习：
         * `我 忘 了` ⇒ 我 --[忘]--> 了；`举 个 例子` ⇒ 举 --[个]--> 例子；
         * `说 中文`（分词切成 中/文）⇒ 说 --[中]--> 文。虚词表只有一处（frame.h 的 FR_FUNC +
         * FR_PARTICLES，`fr_is_func`），守卫只加在**动态谓语**这条路上 ——
         * 核心关系（有/会/在）本身就是虚词，不能一视同仁。 */
        /* ⚠ 只查**谓语和宾语**：主语常是代词（我/他），代词本来就在虚词表里 ——
         * 第一版连 `aw` 一起查，把 `我 叫 小明`（名字介绍）也挡了，套件当场红。 */
        if (fr_is_func(rw) || fr_is_func(bw)) return NULL;
        if (ai_write_rel(ai, A, B, rid, is_neg, AI_WSRC_RELATION) != 0) return NULL;
        snprintf(resp0, sizeof resp0, "记下 了 ： %s %s%s %s", aw, is_neg ? "不 " : "", rw, bw);
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

/* §6.173d **一侧的词**能不能拼成一个词：最多 3 个词，其中**最多一个**是池里已有的整词，
 * 其余必须是**单字实词**（虚词/语气词由 `rel_one_cjk` 挡掉，`上/里/中` 这类也在虚词表里）。
 * 为什么允许"整词 + 单字"混排：实测 `鲸鱼 属于 哺乳动物` 分词成 `[哺][乳][动物]`、
 * `大学生 属于 人` 分词成 `[大][学生]` —— 都只差这一条。两整词（`学校`+`学习`）仍然拒，
 * 那正是会拼出垃圾节点的形状。 */
static inline int agent_nat_side_ok(AI *ai, char *const *words, size_t from, size_t cnt) {
    if (!ai || cnt < 1 || cnt > 4) return 0;   /* §6.273：4 字词（`电子产品`）也要教得进 */
    size_t whole = 0;
    for (size_t i = 0; i < cnt; ++i) {
        const char *w = words[from + i];
        if (!w || !w[0]) return 0;
        if (rel_one_cjk(w)) continue;
        if (!pool_find(ai->pool, w)) return 0;
        if (++whole > 1) return 0;
    }
    return 1;
}

/* §6.173 **自然说法里的新词**（默认关，`AIP_NATCOIN=1`）：
 * `麒麟 属于 神兽` 两个词都不在词表 ⇒ 以前整句掉降级答复（只有 `::teach` 学得到）。
 *
 * 怎么认（**不猜**）：从第 2 个词往后找**第一个已知的核心关系词**（`rel_id_of(...,0)`，
 * 清单里那 9 个）；它前面的是主语、后面的是宾语，两边各自**拼成一个词**。
 *
 * 守卫（宁可少学，不可学歪）：
 *   · 关系词必须是**核心**关系（`< REL_DYN_BASE`）—— 自然文本里**不登记新谓语**
 *     （那条路会把「觉得」这类词灌进关系表，§6.80 记过）；
 *   · 主语 / 宾语各最多 **2 个词**；每个词要么是**单字实词**、要么**已经在词池里**
 *     （多字词只有在池里才认，免得把 `学校`+`学习` 拼成 `学校学习`）；
 *   · 任何一边含虚词/语气词（的/了/在/很/就/都…，`rel_one_cjk` 就是问这个）⇒ 不认；
 *   · 句子里有疑问词 ⇒ 不认（§6.171：问句不许学成事实）；
 *   · 否定（`不`）⇒ 不认：分词把主语切碎时没法分清 `不` 属于哪边，宁可不学。
 * 返回要说的确认话（调用方 free），不认就 NULL。 */
static inline char *agent_learn_new_words(AI *ai, char *const *words, size_t n,
                                         float *out_conf) {
    if (!ai || ai->rel_off || !ai->nat_coin || !words || n < 3 || n > 9) return NULL;
    if (ai_has_question_word(words, n)) return NULL;
    for (size_t i = 0; i < n; ++i)
        if (!words[i] || !words[i][0] || words[i][0] == '[') return NULL;

    /* §6.270 **问句不当事实学**：`太阳 有 多大` 以前走这条路被学成一条边
     * （实测记下「太阳 有 多大」，还造了个词「多大」）。与 `agent_learn_relation`
     * 里那条 `ai_has_question_word` 守卫同一条原则，这里补一道。 */
    if (ai_has_question_word(words, n)) return NULL;
    /* 找第一个**已知核心关系词**（不调用 allow_new ⇒ 绝不登记新谓语） */
    size_t ri = 0;
    uint8_t rid = 0;
    int neg = 0, neg_sep = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        uint8_t id = rel_id_of(ai, words[i], 0);
        if (id && id < REL_DYN_BASE) { ri = i; rid = id; break; }
        /* §6.173c **合写否定**（`不是`/`不会`/`不属于`）：分词器把它切成**一个**词，
         * 那就用 `rel_merged_neg_id`（与两词写法共用同一份声明）认出来；
         * 否定已经含在这个词里了，措辞别再补一个「不」。 */
        uint8_t mid = 0;
        if (rel_merged_neg_id(words[i], &mid) && mid && mid < REL_DYN_BASE) {
            ri = i; rid = mid; neg = 1; break;
        }
    }
    if (!ri) return NULL;
    /* §6.173b **两词否定**：关系词**紧前面**那个 `不`/`没` 才是否定（`企鹅 不 属于 鸟`）；
     * 主语在它前面结束。别处的 `不`/`没` 要靠下面的虚词守卫拦（`rel_one_cjk` 不认虚词）。 */
    if (!neg && ri >= 2 &&
        (strcmp(words[ri - 1], "不") == 0 || strcmp(words[ri - 1], "没") == 0)) {
        neg = 1; neg_sep = 1;
    }
    size_t na = ri - (size_t)neg_sep, nb = n - ri - 1;
    if (!agent_nat_side_ok(ai, words, 0, na)) return NULL;
    if (!agent_nat_side_ok(ai, words, ri + 1, nb)) return NULL;
    char A[64], B[64];
    size_t oa = 0, ob = 0;
    for (size_t i = 0; i < na; ++i) {
        size_t l = strlen(words[i]);
        if (oa + l + 1 >= sizeof A) return NULL;
        memcpy(A + oa, words[i], l); oa += l;
    }
    A[oa] = 0;
    for (size_t i = 0; i < nb; ++i) {
        size_t l = strlen(words[ri + 1 + i]);
        if (ob + l + 1 >= sizeof B) return NULL;
        memcpy(B + ob, words[ri + 1 + i], l); ob += l;
    }
    B[ob] = 0;
    if (!A[0] || !B[0] || strcmp(A, B) == 0) return NULL;
    /* §6.270 拼出来的 A/B 也要过疑问词那关：分词把「多大」切成 多/大 两个**单字**，
     * 逐词检查看不出它是问句（单字不过 `strlen>=6` 那道），拼起来才看得见。
     * 实测 `太阳 有 多大` 就是这么被学成「太阳 --[有]--> 多大」的。 */
    if (ai_token_has_question_word(A) || ai_token_has_question_word(B)) return NULL;
    /* §6.264 **`在` 不归造词这条路**：`我 在 想 事情` 会被造出「想事情」这个"地点"（实测垃圾）。
     * 地点有它自己的路（`agent_learn_locative`：`A V 在 B` / `A 在 B V`），这里不掺和。 */
    if (rid == REL_AT) return NULL;
    /* §6.227 **虚词不当宾语**（`我 住 在 很` 会学成 `我住 在 很`）+ **代词不能跟别的词拼成主语**
     * （`我 住 在 …` 的主语是 我+住 两截 ⇒ 拼出来的「我住」是垃圾词）。 */
    if (fr_is_func(B)) return NULL;
    /* §6.245 并列/选择词留在宾语里 ⇒ 那是"两个东西"，不是一个新词（`猫或狗`/`猫、狗`）*/
    if (strstr(B, "或") || strstr(B, "、") || strstr(B, "以及")) return NULL;
    /* §6.229 主语不能是别的虚词（很/了/的…）。
     * §6.272 **例外：地名/方位复合**（`上海`）—— 首字 `上/下/里/中/外` 在虚词表里，
     * 于是「上海 属于 城市」整句学不进（实测），可问句那头已经会说"我还不认识上海"，
     * 教学闭环就断了。只放这几个**地名常用方位字**、且正好 2 个词、另一个必须是实词。 */
    if (!agent_subj_ok(words[0])) {
        static const char *loc0[] = {"上", "下", "里", "中", "外", NULL};
        int ok_loc = 0;
        if (na == 2)
            for (size_t t = 0; loc0[t]; ++t)
                if (!strcmp(words[0], loc0[t])) ok_loc = 1;
        if (!ok_loc) return NULL;
    }
    /* §6.231/§6.265 **多词那一侧不许混进虚词**，但**代词+名词**的复合是正常说法
     * （`我表弟 是 大学生`）：以前一律挡"代词 + 别的词"，把这类合法复合也挡了（实测
     * `我表弟是大学生` 整句学不进）。现在：含代词时，**其余词必须都是词池里已有的词**
     * 且不是虚词/关系词 —— `我住`（住 是动词、可以不在池）挡，`我表弟`（表弟 在池）放。
     * `猫 很 属于 动物` 仍挡（很 是虚词）。 */
    if (na > 1) {
        int has_pron = 0;
        for (size_t i = 0; i < na; ++i) {
            if (fr_is_func(words[i])) {
                static const char *pr[] = {"我", "你", "您", "他", "她", "它", "我们", "你们",
                                           "他们", NULL};
                int is_pron = 0;
                for (size_t k = 0; pr[k]; ++k) if (!strcmp(words[i], pr[k])) { is_pron = 1; break; }
                if (!is_pron) {
                    /* §6.272 同上：`上海` 的 `上` 是方位虚词，作为地名首字放行（只限 2 词）。 */
                    static const char *loc1[] = {"上", "下", "里", "中", "外", NULL};
                    int is_loc = 0;
                    if (na == 2 && i == 0)
                        for (size_t t = 0; loc1[t]; ++t)
                            if (!strcmp(words[i], loc1[t])) is_loc = 1;
                    if (!is_loc) return NULL;  /* 别的虚词一律不行（很/了/的…）*/
                }
                has_pron = 1;
            }
        }
        if (has_pron) {
            for (size_t i = 0; i < na; ++i) {
                if (fr_is_func(words[i])) continue;             /* 代词本身 */
                if (!pool_find(ai->pool, words[i])) return NULL; /* 别的词必须在池里 */
                if (rel_id_of(ai, words[i], 0)) return NULL;     /* 关系词不当名字的一部分 */
            }
        }
    }
    /* §6.274 **多词那一侧不许夹关系词**：以前只在有代词时查主语侧，
     * 宾语侧完全不查 ⇒ 实测学出「猫 喜欢 鱼属于狗」「猫 属于 狗属于猫」
     * 「猫 属于 狗每天叫」（都是把**另一个分句**当成了名字）。主语侧同样补上
     * —— 顺带挡住「我住 属于 城市」（住 是动态关系）。
     * 单侧只有 1 个词时不用查（形状一 already 保证它是实词或池里的词）。 */
    if (nb > 1) {
        for (size_t i = 0; i < nb; ++i) {
            const char *w = words[ri + 1 + i];
            if (fr_is_func(w)) return NULL;
            if (rel_id_of(ai, w, 0)) return NULL;
            /* §6.274 形状必须是"干净复合词"：**前面全是单字，末词单字或池里多字词**。
             * 中间夹一个多字词就是分句（`狗 每天 叫`），不是名字。 */
            if (i + 1 < nb && !rel_one_cjk(w)) return NULL;
        }
        if (!rel_one_cjk(words[ri + nb]) && !pool_find(ai->pool, words[ri + nb])) return NULL;
    }
    if (na > 1)
        for (size_t i = 0; i < na; ++i)
            if (rel_id_of(ai, words[i], 0)) return NULL;
    /* 试过但**证不出正例**：想再收一道"代词 + 单字 ⇒ 不是名字"（挡 `我住`），
     * 但 `我表弟` 在这种模型里正好切成 我/表/弟 三个单字 ⇒ 把合法复合也挡了
     * （interact ㊶ 当场红）。要挡 `我住` 需要词性，现有信息分不开 ⇒ 不留在树上。 */
    if (!pool_find(ai->pool, A) && strlen(A) > 12) return NULL;   /* 别造超长怪词 */
    if (!pool_find(ai->pool, B) && strlen(B) > 12) return NULL;
    Token *ta = pool_token_intern(ai->pool, A);
    Token *tb = pool_token_intern(ai->pool, B);
    if (!ta || !tb || ta == tb) return NULL;
    if (ai_write_rel(ai, ta, tb, rid, neg, AI_WSRC_RELATION) != 0) return NULL;
    ai->n_rel_learned++;
    ai->n_learned++;
    char resp0[192];
    snprintf(resp0, sizeof resp0, "记下 了 ： %s %s%s %s（新词当场造）",
             ta->name, neg_sep ? "不 " : "", words[ri], tb->name);
    cot_push(ai, COT_LEARN, 1, 80, "自然说法里含新词：当场造词 + 学一条核心关系", resp0);
    ai->n_cot++;
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
    /* §6.228 宾语/谓语是虚词就不设假设（`如果 猫 属于 很`）。主语同理不查（可以是代词）。 */
    if (fr_is_func(bw)) return NULL;
    if (!agent_subj_ok(aw)) return NULL;          /* §6.229 `如果 很 属于 猫` 不该设 */
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
 */
/* ③ 的复数主语表：视角判定与「问大家」共用这两张表（只写一处）。 */
static const char *const DIALOG_OTHERS_SUBJ[] = {
    "大家", "他们", "她们", "别人", "旁人", "大伙", "众人", "其他人", NULL
};
static const char *const DIALOG_OTHERS_ASK[] = {
    "怎么", "看", "想", "说", "讲", "觉得", "认为", "看法", "意见", "态度",
    "都", "是", "的", "呢", "吗", "呀", NULL
};

static inline int dialog_others_in_list(const char *const *list, const char *w) {
    if (!w) return 0;
    for (size_t i = 0; list[i]; ++i)
        if (strcmp(list[i], w) == 0) return 1;
    return 0;
}

/*
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
    /* ③（打开时）：**复数主语**（大家/他们/别人…）不站"某一个人"的立场 ——
     * 那个形状归"问大家"（`dialog_trigger_try_others`，四态判定）。
     * 关着时这一条不生效 ⇒ 行为与今天逐字节相同（今天它会拿我的知识冒充"大家看来"）。 */
    if (ai->others_ask && dialog_others_in_list(DIALOG_OTHERS_SUBJ, words[0]))
        return 0;
    /* 「我 觉得 X」= 我自己的视角 —— 标记照样剥掉，只是不换立场。
     * 不剥的话「我 觉得 猫 会 飞 吗」整句会掉到检索层去（实测）。 */
    if (strcmp(words[0], "我") == 0) {
        if (out_who) *out_who = NULL;
        return 1;
    }
    /* **指代消解最小版**（② 档，默认关）：句首的**单数**代词（他/她/它）指回
     * "最近一次说话的那个人"（`ai->last_who`，在他者信念写入时记下）——
     * 把 `view_who` 设成他，后面整条视角路径原样复用（他的信念当假设、答完撤回）。
     * ⚠ 必须在下面"把 words[0] 当人名 intern"**之前**返回：不然图上会多出一个叫「他」的人，
     * 问他的立场只会得到"我不知道他怎么想"（今天就是这个行为）。认不出就照旧走流水线。 */
    if (ai->pron_last && ai->last_who &&
        (strcmp(words[0], "他") == 0 || strcmp(words[0], "她") == 0 ||
         strcmp(words[0], "它") == 0)) {
        ai->view_who = ai->last_who;
        if (out_who) *out_who = ai->view_who;
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
/* **升格**（§6.119，用户设计最后一条）：假设撤掉之后，如果那条结论**从无条件的图里**也能推出来
 * （或用户后来直接教了它 ⇒ 图上有了非推论的边），就把**前提摘掉** —— 条件缓存升格成普通推论。
 * 判据用 `rel_solve_ignoring_caches()`：它**忽略 role=推论 的沉淀边** ✓，所以"能不能升格"
 * 问的是"没有这条缓存，图里还推得出来吗"，不会被缓存自己蒙混过去 ✓。 */
static inline void ai_hypo_cache_sweep(AI *ai) {
    if (!ai || !ai->hypo_cache || ai->n_hc == 0) return;
    for (size_t i = 0; i < ai->n_hc; ) {
        Token *A = ai->hc[i].A, *B = ai->hc[i].B, *P = ai->hc[i].P;
        uint8_t rid = ai->hc[i].rid;
        int neg = ai->hc[i].neg;
        if (!A || !B || !P || ai_premise_holds(ai, P)) { ++i; continue; }   /* 条件还活着：不动 */
        if (!rel_solve_ignoring_caches(ai, A, rid, B, neg)) { ++i; continue; }  /* 还推不出：留着 */
        /* 升格：摘掉前提（把 A→P 那条前提链接的 action 清掉 ⇒ 它不再是前提链接） */
        for (size_t k = 0; k < A->count; ++k) {
            AssocEntry *e = &A->assocs[k];
            if (e->main == P && edge_is_premise_link(e)) { e->action = NULL; break; }
        }
        char note[160];
        snprintf(note, sizeof note,
                 "升格：假设撤了、**无条件也推得出来** ⇒ 摘掉前提（%s %s%s %s）",
                 A->name ? A->name : "?", neg ? "不 " : "",
                 rel_name_of(ai, rid) ? rel_name_of(ai, rid) : "?", B->name ? B->name : "?");
        cot_push(ai, COT_HYP, 1, 55, "条件缓存升格", note);
        ai->n_cot++;
        /* 从登记表里换出（尾元素补位） */
        ai->hc[i] = ai->hc[--ai->n_hc];
    }
}

/* **假设下的结论 → 条件缓存**（§6.118）：假设不进图（铁律），但结论别丢：
 * 写成**有条件成立**的形状 —— `A —[前提]→ P → B`，其中 P = **整条假设栈的指纹**。
 * 前提的成立条件就是"这层假设还活着"（`ai_premise_holds` 里按指纹判 ✓）。
 * 栈一撤 / 换成别的假设 ⇒ 指纹不匹配 ⇒ 缓存自动失效（不需要去删它）。
 * ⚠ 默认关（`::hypo cache on` / `AIP_HYPOCACHE=1`）。 */
static inline void agent_hypo_cache(AI *ai, Token *A, uint8_t rid, Token *B, int neg, int truth) {
    if (!ai || !ai->hypo_cache || !A || !B || rid == 0 || truth == 0) return;
    if (ai->hypo.depth == 0) return;                 /* 没有假设就没有"条件" */
    char fp[32];
    ai_hypo_fingerprint(ai, fp, sizeof fp);
    if (!fp[0]) return;
    Token *P = pool_token_intern(ai->pool, fp);
    if (!P) return;
    /* 结论边先落地（极性照结论），**标成「推论」**（= 沉淀缓存 ⇒ `rel_solve_ignoring_caches`
     * 会忽略它，"升格"判据才问得出"没有这条缓存还推得出来吗"），再挂前提。 */
    if (neg) (void)token_tag_nrel(A, B, rid, REL_BOOST);
    else     (void)token_tag_rel(A, B, rid, REL_BOOST);
    for (size_t k = 0; k < A->count; ++k) {
        AssocEntry *e = &A->assocs[k];
        if (e->main == B && ((neg && (e->nrel & REL_BIT(rid))) || (!neg && (e->rel & REL_BIT(rid))))) {
            e->role = ASSOC_ROLE_DERIVED;
            token_cache_enqueue(A);              /* §6.127：同上 */
            break;
        }
    }
    char an[96], bn[96];
    snprintf(an, sizeof an, "%s", A->name ? A->name : "");
    snprintf(bn, sizeof bn, "%s", B->name ? B->name : "");
    /* **直接建前提链接**（`A —[前提]→ P`）：不走 `加前提` 那个 op —— 它是给**共现直连**用的
     * "拆直连"，而这里 A—R→B 是**知识/推论**（拆直连的规矩本来就拒绝拆知识 ✗ 实测队列跑完
     * 也没落地）。直接建成边 + `action=[前提]` 就是这个项目认的前提形状（判据只有一处：
     * `edge_premise_polarity` ✓）。 */
    Token *pm = pool_token_intern(ai->pool, AI_PREMISE_MARK);
    int made = 0;
    if (pm) {
        if (!token_premise_link(A, P)) (void)token_add_assoc_ex(A, P, pm);
        made = (token_premise_link(A, P) != NULL);   /* **以"真的建成"为准**，不看返回值 */
    }
    if (made) {
        ai->n_hypo_cache++;
        char why[THINK_WHY];
        {   /* 有界拼（三处来源都可能很长；截断是故意的，别让 -Wformat-truncation 拦下） */
            size_t off = 0;
            #define HYC_ADD(str_) do { const char *q_ = (str_); \
                while (*q_ && off + 1 < sizeof why) why[off++] = *q_++; } while (0)
            HYC_ADD("假设下的结论沉淀成**条件缓存**：");
            HYC_ADD(an); HYC_ADD(neg ? " 不 " : " ");
            HYC_ADD(rel_name_of(ai, rid) ? rel_name_of(ai, rid) : "?"); HYC_ADD(" ");
            HYC_ADD(bn); HYC_ADD("（前提 = 整条假设栈 "); HYC_ADD(fp); HYC_ADD("）");
            why[off] = 0;
            #undef HYC_ADD
        }
        cot_push(ai, COT_HYP, 1, 40, "条件缓存（假设下推出）", why);
        ai->n_cot++;
        /* 登记（有界）：撤回后靠它复查能不能升格（§6.119） */
        if (ai->n_hc < sizeof ai->hc / sizeof ai->hc[0]) {
            ai->hc[ai->n_hc].A = A; ai->hc[ai->n_hc].B = B; ai->hc[ai->n_hc].P = P;
            ai->hc[ai->n_hc].rid = rid; ai->hc[ai->n_hc].neg = (uint8_t)(neg ? 1 : 0);
            ai->n_hc++;
        }
    }
}

/* §6.168 **记下这个问句**（等用户下一轮回话）。
 * 三个地方调：记忆猜测、类比猜测、真的推不出来 —— 都是"我没把握"。
 * 不记"确定答出来"的那条：那时用户再说「会」是纠正，不是补充（纠正走改口那条路）。 */
/* §6.217 **「刚才聊了什么」**：列最近几轮**用户**说的话（原样）。 */
static inline int agent_history_kind(const char *input) {
    if (!input) return 0;
    char j[80]; size_t jl = 0;
    for (const char *q = input; *q && jl + 1 < sizeof j; ++q) {
        if (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') continue;
        j[jl++] = *q;
    }
    j[jl] = 0;
    static const char *list[] = {"我们刚才聊了什么", "刚才聊了什么", "我们刚才说了什么",
                                 "刚才说了什么", "我们聊了什么", "我们聊到哪了", "聊到哪了",
                                 "我们说到哪了", "刚才说到哪了", "我们聊到哪儿了", NULL};
    static const char *mine[] = {"我刚才说了什么", "我刚才说什么", "你记得我刚才说什么吗",
                                 "我刚才说啥", "刚才我说的什么", "我刚才说的什么",
                                 "我刚才说的是什么", "我说了什么", "我刚才说啥了", NULL};
    for (size_t i = 0; list[i]; ++i) if (strcmp(j, list[i]) == 0) return 1;
    for (size_t i = 0; mine[i]; ++i) if (strcmp(j, mine[i]) == 0) return 2;
    return 0;
}

static inline void agent_history_record(AI *ai, const char *input) {
    if (!ai || !input || !input[0]) return;
    size_t slot = ai->hist_head % 6;
    snprintf(ai->hist[slot], sizeof ai->hist[slot], "%s", input);
    ai->hist_head = (ai->hist_head + 1) % 6;
    if (ai->hist_n < 6) ai->hist_n++;
}

/* §6.216 **「再说一遍」**：把上一句答复原样再说一次。
 * 它本来就记着上一句（`ai->last_output` 存的是**答复**的词），只是没有这个指令 ⇒
 * 实测 `再 说 一 遍` 掉进生成流水线，答出「遍 我 睡 很 问 你」。 */
static inline int agent_repeat_request(const char *input) {
    if (!input) return 0;
    char j[64]; size_t jl = 0;
    for (const char *q = input; *q && jl + 1 < sizeof j; ++q) {
        if (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') continue;
        j[jl++] = *q;
    }
    j[jl] = 0;
    static const char *forms[] = {
        "再说一遍", "再说一次", "再说一下", "再讲一遍", "重复一遍", "重复一次",
        "你刚才说什么", "你说了什么", "你刚说啥", "刚才说什么",
        NULL
    };
    for (size_t i = 0; forms[i]; ++i) if (strcmp(j, forms[i]) == 0) return 1;
    return 0;
}

/* §6.203 **裸短回话**（没有挂着的追问时）：整句只是 对/不 对/嗯/好/不会 这类词。
 * 以前这种输入会掉进生成流水线，答出「对 自己 你 故意 的」「不会 到 回来 在 说 行 不」这种词串。
 * 返回 1=肯定、2=否定、0=不是裸短回话（照常走流水线）。 */
/* §6.261 「自我/能力」类问题的**统一路由**（顺序 = 优先级，具体 → 宽泛）：
 *   ① 实现事实（语言/年龄/位置/出处/感情）   [最具体，必须先问]
 *   ② 能做什么（清单）
 *   ③ 你是谁 / 你叫什么
 *   ④ 日期时刻（没有时钟）
 *   ⑤ 天气预报（没有数据）
 *   ⑥ 联网 / 算术（能力边界）
 * 返回非 NULL 表示已经答完（out 里有内容）。 */
static inline const char *agent_self_route(const char *input, char *out, size_t cap) {
    const char *sf = agent_self_fact(input);
    if (sf) { snprintf(out, cap, "%s", sf); return out; }
    if (agent_self_capability(input)) {
        snprintf(out, cap,
                 "就 三 件 事：① 回答 图 里 记 着 的 关系（「猫 属于 动物 吗」「谁 会 飞」）；"
                 "② 你 教 一 句 我 就 记 住（「考拉 属于 动物」「我 叫 小明」）；"
                 "③ 多 跳 推 理 / 假设 / 记 别人 的 看法（「如果 猫 会 飞」「他 以为 …」）。"
                 "不 会 的 我 直说：不 联 网、不 会 算 术、不 知道 天气 和 日期。");
        return out;
    }
    if (agent_self_identity(input)) {
        snprintf(out, cap,
                 "我 叫 JainkAI —— 一个 白箱 的 中文 对话 内核。"
                 "我 的 回答 全部 来自 图 里 记 的 知识（每条 都 能 追）；不 会 的 就 说 不 会。");
        return out;
    }
    {
        const char *gl = agent_social_glue(input);
        if (gl) { snprintf(out, cap, "%s", gl); return out; }
    }
    {
        const char *un = agent_unsupported_request(input);
        if (un) { snprintf(out, cap, "%s", un); return out; }
    }
    {
        const char *cf = agent_confirm_request(input);
        if (cf) { snprintf(out, cap, "%s", cf); return out; }
    }
    if (agent_clock_unknown(input)) {
        snprintf(out, cap,
                 "我 没有 时 钟，也 不 联 网 —— 具体 几 号/几 点 我 不 知道（你 看 一下 手机 更 准）。");
        return out;
    }
    if (agent_weather_unknown(input)) {
        snprintf(out, cap,
                 "天气 我 不 知道 —— 我 没有 天气 数据，也 连 不 上 网。"
                 "（这 类 问题 我 只 能 说 不 会，不 编。）");
        return out;
    }
    {
        const char *cap2 = agent_capability_unknown(input);
        if (cap2) { snprintf(out, cap, "%s", cap2); return out; }
    }
    {
        const char *mq = agent_measure_question(input);
        if (mq) { snprintf(out, cap, "%s", mq); return out; }
    }
    return NULL;
}

static inline int agent_bare_reply(const char *input) {
    if (!input) return 0;
    char buf[64];
    size_t bl = 0;
    for (const char *q = input; *q && bl + 1 < sizeof buf; ++q) {
        if (*q == '\r' || *q == '\n') continue;
        buf[bl++] = *q;
    }
    buf[bl] = 0;
    /* 前后空白切干净 */
    char *b = buf; while (*b == ' ' || *b == '\t') b++;
    /* §6.226 句首代词**只对「没听懂」那组**放宽（`我 不 明白` / `我 不懂`）：
     * ⚠ 不能拿去匹配肯定/否定组 —— 第一版这么干，`你 好` 被剥成「好」⇒ 当成裸回话「嗯，好」，
     * 连带把他者模型那条路也绕掉了（features 套件「他者模型」当场红）。 */
    char *bp = b;
    {
        static const char *pron[] = {"我们", "我", "你", "您", "他", "她", "它", NULL};
        for (size_t i = 0; pron[i]; ++i) {
            size_t pl = strlen(pron[i]);
            /* §6.270 无空格也要剥（`我不明白`）：以前要求代词后面有空格，真人输入没空格
             * ⇒ 「我 不 明白」掉生成流水线，答出「明白 故意 的」。仍然**只给 confuse 组用**。 */
            if (strncmp(b, pron[i], pl) == 0) {
                char *q = b + pl;
                while (*q == ' ' || *q == '\t') q++;
                bp = q;
                break;
            }
        }
    }
    char *e = b + strlen(b); while (e > b && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if (!*b) return 0;
    static const char *yes[] = {"对", "是", "嗯", "好", "行", "好的", "是啊", "是的", "没错",
                                "对 的", "可以", "嗯 嗯", "懂了", "明白", "明白了", "知道了",
                                "能", "会", "行 的", NULL};   /* §6.234：单字肯定也别掉流水线 */
    static const char *no[]  = {"不", "不是", "不对", "不会", "不能", "没", "没有", "不 对",
                                "不 是", "不 会", "不 能", "没 有", "不 太 对",
                                "不知道", NULL};
    /* §6.226 **没听懂**是第三种回话（不是"你说错了"）：`不懂`/`不 懂`/`不明白`/`没 听懂` …
     * 以前它们既不算肯定也不算否定 ⇒ 掉进生成流水线，答出「懂 没 到 来 不 完全 但」。 */
    static const char *confuse[] = {"不懂", "不 懂", "不明白", "不 明白", "没懂", "没 懂",
                                    "没听懂", "没 听 懂", "听不懂", "听 不 懂",
                                    "不清楚", "不 清楚", NULL};
    for (size_t i = 0; yes[i]; ++i) if (strcmp(b, yes[i]) == 0) return 1;
    for (size_t i = 0; no[i]; ++i)  if (strcmp(b, no[i]) == 0)  return 2;
    for (size_t i = 0; confuse[i]; ++i)
        if (strcmp(b, confuse[i]) == 0 || strcmp(bp, confuse[i]) == 0) return 3;
    return 0;
}

/* §6.188 **把 pending 落成一个标记节点**：`[追问:k|n|f|R|A|B]`（普通符号节点、随图存盘）。
 * pending 本身只在内存 ⇒ 存盘再开就没了；有了标记，`agent_ask_restore` 能把它接回来。
 * 注意：标记是**新增节点**（不删不挪）⇒ 存档 id 仍然连续（§6.186 的约束）。 */
static inline void agent_ask_persist(AI *ai) {
    if (!ai || !ai->pool || !ai->ask_pending) return;
    Micro *mk = micro_of(ai);
    if (!mk || mk->safe.mod_off) return;
    /* §6.202 `::thinklearn off` 的承诺是"思维的结果不改脑"。`[追问:…]` 标记**也是改脑**
     * （多一个符号节点）—— 反向整包实测：开着 askback 时这条断言红（新符号节点 1≠0）。
     * 所以思维被要求不改脑时，**别落标记**（pending 仍留内存，存盘就丢，可接受）。 */
    if (think_of(ai) && think_of(ai)->learn_off) return;
    const char *rn = rel_name_of(ai, ai->ask_rid);
    if (!rn) return;
    if (strchr(ai->ask_a, '|') || strchr(ai->ask_b, '|') || strchr(rn, '|')) return;
    char nm[96];
    int n = snprintf(nm, sizeof nm, MICRO_PENDING_TOKEN_PREFIX "%d|%d|%d|%s|%s|%s]",
                     ai->ask_kind, ai->ask_neg, ai->ask_fact_neg, rn, ai->ask_a, ai->ask_b);
    if (n <= 0 || (size_t)n >= sizeof nm) return;
    /* §6.202 **同一个追问已经挂着活标记 ⇒ 复用它**（别再造一个）。
     * 空闲思考会把同一个没想通的问题反复重挂；每造一个新标记就得把旧标记标垃圾
     * ⇒ 图里"垃圾 +1、边 -1"，"空闲是只读"这条就被破坏（反向整包实测红在这里）。 */
    {
        Token *old = pool_find(ai->pool, nm);
        if (old && !old->is_garbage) {
            snprintf(ai->ask_mark, sizeof ai->ask_mark, "%s", nm);
            return;
        }
    }
    ModIntent in;
    memset(&in, 0, sizeof in);
    in.type = MOD_ADD_TOKEN;
    in.target = pool_find(ai->pool, ai->ask_a);
    if (!in.target) in.target = ai->self;
    if (!in.target) return;
    snprintf(in.name, sizeof in.name, "%s", nm);
    snprintf(in.source, sizeof in.source, "来源:追问");
    (void)mod_propose(&mk->queue, &in);
    (void)micro_drain_queue(ai);          /* 当场落地（这些路径在 agent_step 开头就返回了） */
    snprintf(ai->ask_mark, sizeof ai->ask_mark, "%s", nm);
}

/* 只把**上一条标记**标成垃圾（不动当前 pending）：kind=2 覆盖时用 —— 免得旧标记
 * 在载入时被当成"还挂着的追问"（§6.201）。 */
static inline void agent_ask_drop_mark_only(AI *ai) {
    if (!ai || !ai->ask_mark[0]) return;
    Token *m = pool_find(ai->pool, ai->ask_mark);
    if (m) m->is_garbage = 1;
    ai->ask_mark[0] = 0;
}

/* 丢掉 pending（无论成不成）：**标记标成垃圾**（不删节点 ⇒ id 连续），字段清干净。 */
static inline void agent_ask_drop(AI *ai) {
    if (!ai) return;
    if (ai->ask_mark[0]) {
        Token *m = pool_find(ai->pool, ai->ask_mark);
        if (m) m->is_garbage = 1;          /* 内核自己的账，不是知识：直写 */
        ai->ask_mark[0] = 0;
    }
    ai->ask_pending = 0;
    ai->ask_kind = 0;
    ai->ask_fact_neg = 0;
}

/* §6.188 **载入后把挂着的追问接回来**（只扫一次）：取**最新的**非垃圾 `[追问:…]` 标记，
 * 解析成 (kind, neg, fact_neg, R, A, B)。关系词认不出（比如动态谓语）就跳过。 */
static inline void agent_ask_restore(AI *ai) {
    if (!ai || !ai->pool || ai->ask_restored) return;
    ai->ask_restored = 1;
    if (ai->ask_pending) return;
    const Token *best = NULL;
    const size_t plen = strlen(MICRO_PENDING_TOKEN_PREFIX);
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        if (!t || t->is_garbage || !t->name) continue;
        if (strncmp(t->name, MICRO_PENDING_TOKEN_PREFIX, plen) != 0) continue;
        if (!best || t->id > best->id) best = t;
    }
    if (!best) return;
    char buf[96];
    snprintf(buf, sizeof buf, "%s", best->name + plen);
    size_t bl = strlen(buf);
    if (bl && buf[bl - 1] == ']') buf[bl - 1] = 0;
    char *f[6]; size_t nf = 0;
    for (char *q = buf; *q && nf < 6; ) {
        f[nf++] = q;
        char *bar = strchr(q, '|');
        if (!bar) { q += strlen(q); break; }
        *bar = 0; q = bar + 1;
    }
    if (nf < 6) return;
    uint8_t rid = rel_id_of(ai, f[3], 0);
    if (!rid) return;
    if (!pool_find(ai->pool, f[4]) || !pool_find(ai->pool, f[5])) return;
    snprintf(ai->ask_a, sizeof ai->ask_a, "%s", f[4]);
    snprintf(ai->ask_b, sizeof ai->ask_b, "%s", f[5]);
    ai->ask_rid = rid;
    ai->ask_kind = (atoi(f[0]) == 2) ? 2 : 1;
    ai->ask_neg = (uint8_t)(atoi(f[1]) ? 1 : 0);
    ai->ask_fact_neg = (uint8_t)(atoi(f[2]) ? 1 : 0);
    snprintf(ai->ask_mark, sizeof ai->ask_mark, "%s", best->name);
    ai->ask_pending = 1;
}

static inline void agent_askback_arm_name(AI *ai, const char *a, uint8_t rid,
                                         int is_neg, const char *b,
                                         int kind, int fact_neg) {
    if (!ai || !ai->askback || !a || !b || !a[0] || !b[0] || rid == 0) return;
    snprintf(ai->ask_a, sizeof ai->ask_a, "%s", a);
    snprintf(ai->ask_b, sizeof ai->ask_b, "%s", b);
    ai->ask_rid = rid;
    ai->ask_neg = (uint8_t)(is_neg ? 1 : 0);
    ai->ask_kind = (kind == 2) ? 2 : 1;
    ai->ask_fact_neg = (uint8_t)(fact_neg ? 1 : 0);
    ai->ask_pending = 1;
    ai->ask_mark[0] = 0;
    /* §6.201 **只有"我没把握"才落标记**（kind=1）。kind=2 是"我给了确定答案、等你确认/纠正"——
     * 那种 pending **每个确定答案都会有一个**，落标记等于每次问答都往图里塞一个垃圾节点。
     * kind=2 留在内存里（存盘丢了就丢了：下次重问一句即可）。 */
    if (ai->ask_kind == 1) agent_ask_persist(ai);   /* §6.188 */
    else                   agent_ask_drop_mark_only(ai);
}
/* §6.174 **问句里含新词**（`恐龙 会 飞 吗`）：关系层解析不了（主语被切成 恐/龙），
 * 于是连追问都不会挂 —— 用户回一句「不 会」也白说。
 * 这里用与 §6.173 **同一套守卫**切出 (A,R,B)，只**挂追问**、**不当场造词**：
 * 造词留到用户回话确认的时候（`agent_askback_reply` 里 intern）——问句上不写图。
 * 两边都在词池里 ⇒ 不挂（关系层本该处理，别抢）。 */
static inline void agent_ask_new_word_arm(AI *ai, char *const *words, size_t n) {
    if (!ai || !ai->askback || !words || n < 4 || n > 7) return;
    if (!rel_is_qmark(words[n - 1])) return;
    size_t last = n - 1;
    if (last < 3) return;
    if (ai_has_question_word(words, last)) return;   /* `什么/谁` 那种是开放问句，另走一条路 */
    for (size_t i = 0; i < last; ++i)
        if (!words[i] || !words[i][0] || words[i][0] == '[') return;
    size_t ri = 0;
    uint8_t rid = 0;
    int neg = 0, neg_sep = 0;
    for (size_t i = 1; i + 1 < last; ++i) {
        uint8_t id = rel_id_of(ai, words[i], 0);
        if (id && id < REL_DYN_BASE) { ri = i; rid = id; break; }
        uint8_t mid = 0;
        if (rel_merged_neg_id(words[i], &mid) && mid && mid < REL_DYN_BASE) {
            ri = i; rid = mid; neg = 1; break;       /* 合写否定：不是/不会 */
        }
    }
    if (!ri) return;
    if (!neg && ri >= 2 &&
        (strcmp(words[ri - 1], "不") == 0 || strcmp(words[ri - 1], "没") == 0)) {
        neg = 1; neg_sep = 1;
    }
    size_t na = ri - (size_t)neg_sep, nb = last - ri - 1;
    if (!agent_nat_side_ok(ai, words, 0, na)) return;
    if (!agent_nat_side_ok(ai, words, ri + 1, nb)) return;
    char A[64], B[64];
    size_t oa = 0, ob = 0;
    for (size_t i = 0; i < na; ++i) {
        size_t l = strlen(words[i]);
        if (oa + l + 1 >= sizeof A) return;
        memcpy(A + oa, words[i], l); oa += l;
    }
    A[oa] = 0;
    for (size_t i = 0; i < nb; ++i) {
        size_t l = strlen(words[ri + 1 + i]);
        if (ob + l + 1 >= sizeof B) return;
        memcpy(B + ob, words[ri + 1 + i], l); ob += l;
    }
    B[ob] = 0;
    if (!A[0] || !B[0] || strcmp(A, B) == 0) return;
    if (pool_find(ai->pool, A) && pool_find(ai->pool, B)) return;
    agent_askback_arm_name(ai, A, rid, neg, B, 1, neg);
}

static inline void agent_askback_arm(AI *ai, const Token *A, uint8_t rid,
                                     int is_neg, const Token *B, int as_other,
                                     int kind, int fact_neg) {
    if (!ai || !ai->askback || as_other) return;
    if (!A || !B || !A->name || !B->name || rid == 0) return;
    agent_askback_arm_name(ai, A->name, rid, is_neg, B->name, kind, fact_neg);
}

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
    /* **只查不建**：解析阶段绝不往池子里塞新符号节点（造出过 `火锅了` 那种垃圾）。 */
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
        /* §6.118：这一轮如果**在假设下**，把结论沉淀成条件缓存（假设本身不进图 ✓） */
        agent_hypo_cache(ai, A, rid, B, is_neg ? 1 : 0, d->truth);
        if (agree) ai->n_rel_yes++;
        else       ai->n_rel_no++;
        if (pf.n > 2) ai->n_rel_deep++;
        if (out_conf) *out_conf = 0.9f;
        /* §6.172 **确定答案也要记**（kind=2）：用户下一轮说「不 对」就该**改口**，
         * 而不是重新教一条（以前那种回话掉进闲聊模板「没 关系 谁 都 会 有 错」，
         * 知识一点没动）。断言的事实极性 = 问句极性 ^（我答的是不是「不对」）。 */
        agent_askback_arm(ai, A, rid, is_neg, B, as_other, 2,
                          (is_neg ? 1 : 0) ^ (agree ? 0 : 1));
        char *r = (char *)malloc(strlen(resp0) + 1);
        if (r) memcpy(r, resp0, strlen(resp0) + 1);
        return r;
    }

    /* §6.125 **记忆里的猜测**（默认关）：先前类比推出来的结论，这一轮可以拿来当输入 ——
     * ① 同一个问题再问一次（答案一致，不再从零猜）；② **拿它当中转再走一跳**（递归迁移）。
     * 这一趟显式打开"记忆可见"，跑完立刻还原；验证器照旧要独立重推（它会认出记忆步）。 */
    if (!solved && ai->rel_remember) {
        int save_gv = g_rel_guess_visible;
        g_rel_guess_visible = 1;
        Proof mp;
        rel_solve(ai, A, rid, B, want, &mp);
        if (mp.status == 0) rel_solve(ai, A, rid, B, !want, &mp);
        g_rel_guess_visible = save_gv;
        if (mp.status != 0) {
            der_reset(d);
            der_from_proof(ai, d, A, rid, B, &mp);
            int agree = (mp.status == +1) != (want == 1);
            d->has_claim = 1;
            /* §6.131：把握**按猜测环数衰减**（1 环 = 45，与原来逐字节相同；多一环乘 0.7）。
             * 链越接越长还写着 45 的话，等于"猜得越多越有把握"。 */
            float mconf = ai_guess_conf(d->guess_hops);
            int mpct = (int)(mconf * 100.0f + 0.5f);
            der_conclude(d, agree ? +1 : -1, mpct);
            ai->n_der++;
            if (der_verify(ai, d) == 0) {
                der_chain_text(ai, d, line, sizeof line);
                char mwhy[128];
                if (d->guess_hops > 1)
                    snprintf(mwhy, sizeof mwhy,
                             "记忆链：**%d 环**都是先前推测出来的（每多一环把握就乘一次 0.7）",
                             d->guess_hops);
                else
                    snprintf(mwhy, sizeof mwhy, "记忆：拿先前**推测**出来的结论当输入（不是知识）");
                cot_push(ai, COT_INFER, 1, mpct, mwhy, line);
                ai->n_cot++;
                der_say(ai, d, ai->der_style, resp0, sizeof resp0);
                ai->n_rel_guess++;
                /* §6.172b/§6.250 记**猜的极性**：回「对」要按"我猜的那句"来教。
                 * ⚠ 以前写成 `问句极性 ^ (truth>0 ? 0 : 1)` —— 对**否定问句**会算反
                 * （`桌子 不 会 飞 吗` + 猜"不会" ⇒ fact_neg=0）。`d->truth` 本身就是
                 * "A R B 还是 A 不 R B"，直接用它的符号。 */
                agent_askback_arm(ai, A, rid, is_neg, B, as_other, 1,
                                  (d->truth < 0 ? 1 : 0));
                if (out_conf) *out_conf = mconf;
                char *r = (char *)malloc(strlen(resp0) + 1);
                if (r) memcpy(r, resp0, strlen(resp0) + 1);
                return r;
            }
        }
    }

    /* 未知 → 类比推测（明确标注）。§6.133：**和兜底那条走同一个函数** ——
     * 找依据（最近邻/结构相似，退一步同类互推）→ 建链 → **过验证器** → 渲染 → 记账。
     * 以前这一支与 `agent_analogy_fallback` 各写一遍，判据/记账分家（§6.123 原因①的尾巴）。 */
    {
        der_reset(d);
        d->has_claim = 1;
        float gc = 0.45f;
        char gresp[512];
        gresp[0] = 0;
        if (agent_guess_answer(ai, A, rid, B, is_neg ? 1 : 0,
                               AGENT_GUESS_NEIGHBOR | AGENT_GUESS_SIBLING,
                               d, line, sizeof line, gresp, sizeof gresp, &gc)) {
            ai->n_rel_guess++;
            agent_askback_arm(ai, A, rid, is_neg, B, as_other, 1,
                              (d->truth < 0 ? 1 : 0));   /* §6.172b */
            if (out_conf) *out_conf = gc;
            char *r = (char *)malloc(strlen(gresp) + 1);
            if (r) memcpy(r, gresp, strlen(gresp) + 1);
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
    /* §6.168：**我承认推不出来**的这一支，记下问句（等用户下一轮回话）。
     * 判据放在这里而不是去认答复文本里的"不确定"：答复可能是检索来的句子
     * （样例会话里就有一条「我 不 确定 X 会 不 会 Y」），拿文本当信号会误判。
     * 只在 `as_other == 0`（不是"站在他立场"）时记：那条路是别人的信念。 */
    agent_askback_arm(ai, A, rid, is_neg, B, as_other, 1, is_neg);   /* 没说反也没说正：按问句极性 */
    return NULL;
}

/* 帧层给的是**文本**：把它拆回符号节点序列，再用同一套冲突检测。
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
    /* §6.228 **宾语/谓语是虚词就不记**（`他 以为 猫 属于 很`）。⚠ 主语不查：
     * `他 说 我 拥有 猫` 的主语是代词 我，查了会把这条（合理的）看法也挡掉，
     * belief 的"同义改写"断言当场红。 */
    if (fr_is_func(bw)) return NULL;
    if (!agent_subj_ok(aw)) return NULL;          /* §6.229 `他 以为 很 属于 猫` 不该记 */
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
    size_t             n_weak_yes, n_weak_no;    /* ② 上位降权：他说的更宽 ⇒ 只当**半票参考** */
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
            if (!entail) {
                v->n_related++;
                /* ② **上位降权**（默认关）：他说的比你问的**更宽**（"我 喜欢 动物" 对 "我 喜欢 猫"）——
                 * 以前只是"相关"，完全不参与判定；开关打开时算**半票**（放在弱票格里，
                 * 只在**没有强票**时才决定倾向，而且措辞里明说是"参考"）。 */
                if (ai->other_weak_up) { if (f->neg) v->n_weak_no++; else v->n_weak_yes++; }
                continue;
            }
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
    else if (ai->other_weak_up && v->n_weak_yes && v->n_weak_no) v->verdict = OBV_DEFER;
    else if (ai->other_weak_up && v->n_weak_yes) v->verdict = OBV_YES;   /* 只有上位票 ⇒ 倾向"认" */
    else if (ai->other_weak_up && v->n_weak_no)  v->verdict = OBV_NO;
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
    if (v.n_related)
        printf("  （另有 %zu 条是**上位**：他说的比你问的更宽 —— %s）\n", v.n_related,
               ai->other_weak_up ? "当**半票参考**（只在没有确定表态时才决定倾向）"
                                 : "只当相关，不参与判定（要纳入就 ::others refine on）");
    for (size_t i = 0; i < v.n; ++i) {
        const OtherBeliefObs *o = &v.obs[i];
        const HypoFact *f = o->fact;
        printf("    %s：%s %s%s %s", o->who && o->who->name ? o->who->name : "?",
               f->a && f->a->name ? f->a->name : "?",
               f->neg ? "不 " : "", rn ? rn : "?",
               f->b && f->b->name ? f->b->name : "?");
        if (!o->entail)
            printf("   ← 相关，但**不蕴含**所问（上位说法）：%s",
                   ai->other_weak_up ? "当**半票参考**（没有确定表态时才决定倾向）"
                                     : "不算依据（要纳入就 ::others refine on）");
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

/* ============================ 过程执行（第 9 条 ③ 写引擎，默认关） ============================
 *
 * ①+② 让过程**记得下**（role 标记 + `[步骤:N]`/`[调用]`/`[回边]`，§6.70）；这一节做 §6.56 第 3 条：
 * 让 `[过程:名字]` **真的跑一遍**。原话是"**执行轨迹复用 `Derivation`，一步一步可复核，
 * 断点用 `DerGap`**" —— v1 照这个来，但把话说清楚：v1 的**步骤是扁平的动作词**（没有对象/条件），
 * 所以"拿 `der_verify` 独立重推每一步"那条留给 ⑤（那时步骤才是可复核的关系目标）；
 * v1 的每一步写明**用了哪条现成判据**，断点写明**缺什么**。
 *
 * 三条判据（**都是现成的**，没有新语义）：
 *   ① **认识吗**：动作词在图上有没有连结（孤立词做不了）；
 *   ② **前提成立吗**：过程根与这一步之间的**前提符号节点**（`token_premises`）用
 *      `ai_premise_cond_ok` 判（就是"提到过 / 够得着 / 有据"那三档）—— 不成立 ⇒ 断点；
 *   ③ **是符号节点行为吗**：是就走 `token_op_run`（**受审通道**）真执行；要参数的 v1 只记一条断点
 *      （**不猜参数** —— 猜出来的执行比不执行更坏）。
 * 控制流：`[调用]` → 递归跑子过程（深度 ≤ 3）；`[回边]` → 跳回第 N 步（每步迭代 ≤ 2，防死循环）。
 * **只读 + 受审**：核对不动图；真执行某一步行为时走的是老那条通道（有审查、有预算、有账）。
 * 慢是设计（§6.56 原话"明说会慢"）：每步都要查图 + 判前提 + 可能跑一次行为。 */
#define PROC_RUN_DEPTH_MAX 3
#define PROC_RUN_LOOP_MAX  2
#define PROC_RUN_STEP_MAX  64

typedef struct {
    size_t n_step;      /* 记了几步（含调用展开） */
    size_t n_ok;        /* 放行的步数 */
    size_t n_gap;       /* 断点数 */
    size_t n_ran;       /* 真执行过的符号节点行为数 */
    size_t n_call;      /* 进过几次子过程 */
    /* ---- ④ 假设层（执行栈与作用域）---- */
    size_t n_assume;      /* 记了几条假设（干跑：前提假定成立） */
    size_t n_assume_ok;   /* 跑完核对：还在层里的假设数（应当 == n_assume） */
    size_t n_defer;       /* 干跑里**推迟**没执行的行为步骤数 */
    size_t n_rollback;    /* 回退（撤掉某一步的假设）次数 */
    size_t n_skip;        /* ⑤ 条件不成立又没有否则支 / 循环条件不成立 —— 跳过的次数 */
    size_t n_exc_handled; /* ⑤ 出错之后处理步跑通了几次 */
    size_t n_verified;    /* ① 带"要核的事"的步骤：`der_verify` **独立复核通过**的次数 */
    size_t n_verify_fail; /* ① 复核不过（缺口/推不出来）的次数 */
    size_t hypo_base;     /* 跑之前的假设层深度（跑完必须回到它） */
    size_t hypo_peak;     /* 跑的时候到过的最大深度 */
    Derivation trace;   /* 执行轨迹（复用 Derivation：goal + 每步 fact/gap + conclude） */
} ProcRun;

static inline Token *proc_root_of(AI *ai, const char *bare) {
    if (!ai || !bare || !bare[0]) return NULL;
    char full[96];
    int n = snprintf(full, sizeof full, "%s%s]", PROC_TOKEN_PREFIX, bare);
    if (n <= 0 || (size_t)n >= sizeof full) return NULL;
    return pool_find(ai->pool, full);
}

static int proc_step_ptr_cmp(const void *x, const void *y) {
    const AssocEntry *a = *(const AssocEntry *const *)x;
    const AssocEntry *b = *(const AssocEntry *const *)y;
    return edge_step_index(a) - edge_step_index(b);
}

/* 按步回退：撤掉这一步刚加的那些假设（`hypo_del` 是假设层"撤单条"的那一半）。
 * 只撤这一步的 —— 上一步的假设留着（这才是作用域：**回退回到这一步之前**）。 */
static inline void proc_step_rollback(AI *ai, const Token *act, const Token **added,
                                      size_t n_added, ProcRun *pr) {
    if (!ai || !pr || !n_added) return;
    size_t undone = 0;
    for (size_t q = 0; q < n_added; ++q)
        if (hypo_del(&ai->hypo, act, added[q], REL_NEED)) undone++;
    pr->n_rollback += undone;
    if (undone) printf("      （回退：这一步的假设撤了 —— 假设层回到这一步之前）\n");
}

/* 跑**一步**（判据只写这一处：主循环与回边重跑都调它 —— 两处各写一套的话会当场不一致，
 * 实测踩过：主循环放行的步，回边重跑时按老判据判成断点）。返回 0 = 断点 / 1 = 过。 */
static inline int proc_step_run(AI *ai, Token *root, const AssocEntry *se, int idx,
                                ProcRun *pr, const char *tag, int dry) {
    /* 这一步**加了哪些假设**（失败时按步回退，只撤这一步的；见 `hypo_del`） */
    const Token *added[8];
    size_t n_added = 0;
    /* ⑤ 分支/异常那几条边没有步号（idx=0）→ 用 tag 当标签，别打"第 0 步" */
    char label[48];
    if (idx > 0) snprintf(label, sizeof label, "%s第 %d 步", tag ? tag : "", idx);
    else         snprintf(label, sizeof label, "%s", tag && tag[0] ? tag : "这一步");
    const Token *act = se ? se->main : NULL;
    if (!act || act->is_garbage) {
        printf("    %s（动作词没了）\n", label);
        der_gap(&pr->trace, "动作词没了");
        pr->n_gap++;
        return 0;
    }
    /* ② 前提成立吗（根与这一步之间的前提符号节点；三档判据都是现成的） */
    {
        const Token *ps[8];
        int pol[8];
        size_t np = token_premises(root, act, ps, pol, 8);
        for (size_t k = 0; k < np; ++k) {
            if (ai_premise_cond_ok(ai, ps[k], pol[k])) continue;
            if (dry) {
                /* **干跑：假定它成立** —— 用现成的关系「需要」记进假设层（这一步需要这个前提），
                 * 只活在这次跑的作用域里；跑完 pop 干净。 */
                if (hypo_add(&ai->hypo, (Token *)act, (Token *)ps[k], REL_NEED, 0)) {
                    if (n_added < 8) added[n_added++] = ps[k];
                    pr->n_assume++;
                }
                continue;   /* 攒着，和"过"合并成一行打（不然同一步打两遍） */
            }
            printf("    %s %-12s ✗ 断点：前提「%s」没成立（%s）\n", label,
                   act->name ? act->name : "?", ps[k]->name ? ps[k]->name : "?",
                   pol[k] < 0 ? "要求它不在注意窗口里，可它在" : "提到过没有，这次没有");
            der_gap(&pr->trace, "前提没成立");
            pr->n_gap++;
            proc_step_rollback(ai, act, added, n_added, pr);
            return 0;
        }
    }
    /* ③ 是符号节点行为吗 → 真的走受审通道执行；要参数的 v1 不猜 */
    {
        const TokenOp *op = token_op_by_name(act->name);
        if (op && op->min_args > 0) {
            /* **要参数是结构性问题**（干跑也一样做不了）⇒ 先判它，别拿"干跑"把问题盖过去。
             * 这一条也是"按步回退"唯一的真实可达路径：先假定了前提、随后发现这一步做不了。 */
            printf("    %s %-12s ⚠ 断点：这是行为「%s」，要参数（v1 不支持，不猜）\n",
                   label, act->name, op->name);
            der_gap(&pr->trace, "行为要参数（v1 不支持）");
            pr->n_gap++;
            proc_step_rollback(ai, act, added, n_added, pr);
            return 0;
        }
        if (op && dry) {
            /* 干跑：行为**不执行**（不落图），只记"假定能做" —— 这正是干跑与实跑的区别 */
            printf("    %s %-12s ≈ 干跑：行为「%s」**假定能做，不执行**\n",
                   label, act->name ? act->name : "?", op->name);
            DerStep *ds = der_add(&pr->trace, DER_HYP);
            if (ds) { ds->subj = (Token *)act; ds->truth = +1;
                      cot_copy(ds->note, sizeof ds->note, "干跑：假定行为能做（没执行）"); }
            pr->n_ok++; pr->n_defer++;
            return 1;
        }
        if (op) {
            char note2[192];
            int got = token_op_run(ai, op->name, NULL, 0, note2, sizeof note2);
            if (got >= 0) {
                printf("    %s %-12s ✓ 执行了行为「%s」（受审通道）：%s\n",
                       label, act->name, op->name, note2);
                DerStep *ds = der_add(&pr->trace, DER_FACT);
                if (ds) { ds->subj = (Token *)act; ds->truth = +1;
                          cot_copy(ds->note, sizeof ds->note, "执行了行为"); }
                pr->n_ok++; pr->n_ran++;
                return 1;
            }
            printf("    %s %-12s ✗ 断点：行为「%s」没跑成：%s\n",
                   label, act->name, op->name, note2);
            der_gap(&pr->trace, "行为没跑成");
            pr->n_gap++;
            proc_step_rollback(ai, act, added, n_added, pr);
            return 0;
        }
    }
        /* ① **动作符号节点**（§6.82）：这一步的目标是 `[执行:动作词 参数…]` ⇒ **真的调一次**
         * （走 `action_run`：白名单 + 超时 + 账）。干跑时只核过；执行层关着就是**断点**
         * （不许假装做过了）。 */
        if (act->name && strncmp(act->name, MICRO_EXEC_TOKEN_PREFIX,
                                 strlen(MICRO_EXEC_TOKEN_PREFIX)) == 0) {
            const char *cmdline = act->name + strlen(MICRO_EXEC_TOKEN_PREFIX);
            char cmd[160];
            size_t ck = 0;
            for (const char *q2 = cmdline; *q2 && *q2 != ']' && ck + 1 < sizeof cmd; ++q2)
                cmd[ck++] = *q2;
            cmd[ck] = 0;
            if (dry) {
                printf("    %s %-12s ✓ 干跑：动作**不执行**（要真做得跑非干跑）｜命令「%s」\n",
                       label, act->name, cmd);
                der_gap(&pr->trace, "干跑不执行动作");
                pr->n_ok++;
                return 1;
            }
            if (!ai->act_on) {
                printf("    %s %-12s ⚠ 断点：外部执行层**关着**（::act on 打开 / AIP_ACT=1）"
                       "—— 这一步是要真动外部世界的，不许假装做过\n", label, act->name);
                der_gap(&pr->trace, "执行层关着，动作没做");
                pr->n_gap++;
                return 0;
            }
            int arc = action_run(ai, cmd);
            if (arc == 0) {
                printf("    %s %-12s ✓ **真执行了**：%s（%s）\n", label, act->name, cmd,
                       g_act.last_mark[0] ? g_act.last_mark : "rc=0");
                pr->n_ran++;
                pr->n_ok++;
                return 1;
            }
            printf("    %s %-12s ✗ 断点：动作执行失败 rc=%d（%s）\n", label, act->name, arc,
                   g_act.last_reason[0] ? g_act.last_reason : "没理由记下来");
            der_gap(&pr->trace, "动作执行失败");
            pr->n_gap++;
            return 0;
        }
        /* ① **步骤带对象**：记号里有"要核的事"（`[步骤:N@要:S/R/B]`）⇒ 走
         * `rel_solve` 推一遍 + `der_verify` **独立复核**（不信任构造时写下的东西）。
         * 这是 §6.56 五步走里唯一没接的那条，这一轮接上。 */
        {
            int cidx = 0, cneg = 0;
            char cs[64], cr[64], cb2[64];
            if (proc_step_claim(se, &cidx, cs, sizeof cs, cr, sizeof cr, cb2, sizeof cb2, &cneg)) {
                Token *csb = pool_find(ai->pool, cs);
                Token *cob = pool_find(ai->pool, cb2);
                uint8_t crid = rel_id_of(ai, cr, 0);
                if (!csb || !cob || crid == 0) {
                    printf("    %s %-12s ⚠ 断点：要核的「%s %s %s」认不全（主语/关系/宾语得是图里的）\n",
                           label, act->name ? act->name : "?", cs, cr, cb2);
                    der_gap(&pr->trace, "复核目标认不全");
                    pr->n_gap++; pr->n_verify_fail++;
                    proc_step_rollback(ai, act, added, n_added, pr);
                    return 0;
                }
                Proof pf;
                memset(&pf, 0, sizeof pf);
                rel_solve(ai, csb, crid, cob, cneg, &pf);   /* 求解器：void，结果在 pf.status */
                int st = pf.status;
                Derivation d;
                der_from_proof(ai, &d, csb, crid, cob, &pf);
                der_conclude(&d, st, 40);
                int vg = der_verify(ai, &d);
                for (size_t k = 0; k < d.n && pr->trace.n < DER_MAX_STEP; ++k)
                    pr->trace.step[pr->trace.n++] = d.step[k];   /* 复核过的链并进整轮轨迹 */
                if (vg == 0 && st != 0) {
                    printf("    %s %-12s ✓ **独立复核过**（要核「%s%s %s %s」，链 %zu 步、缺口 0）\n",
                           label, act->name ? act->name : "?", cneg ? "不 " : "", cs, cr, cb2,
                           d.n > 0 ? d.n - 1 : 0);
                    pr->n_ok++; pr->n_verified++;
                    return 1;
                }
                printf("    %s %-12s ✗ 断点：**复核不过**（要核「%s%s %s %s」：%s，缺口 %d）\n",
                       label, act->name ? act->name : "?", cneg ? "不 " : "", cs, cr, cb2,
                       st == 0 ? "推不出来" : "验证器不认", vg);
                der_gap(&pr->trace, "独立复核不过");
                pr->n_gap++; pr->n_verify_fail++;
                proc_step_rollback(ai, act, added, n_added, pr);
                return 0;
            }
            (void)cidx;
        }
    /* 普通动作词：v1 **只核"词在 + 前提成立"**，做不做得了**不判**（照实说） */
    {
        size_t hits = 0;
        for (size_t k = 0; k < act->count; ++k)
            if (act->assocs[k].hits > 0) hits++;
        if (n_added) {
            printf("    %s %-12s ≈ 干跑：**过**（假定前提「%s」成立%s —— 记进假设层：%s 需要 %s，跑完 pop）\n",
                   label, act->name ? act->name : "?",
                   added[0] && added[0]->name ? added[0]->name : "?",
                   n_added > 1 ? "…" : "", act->name ? act->name : "?",
                   added[0] && added[0]->name ? added[0]->name : "?");
        } else {
            printf("    %s %-12s ✓ 过（词在 + 前提成立；它自己有 %zu 条连边、%zu 条有命中"
                   " —— **做不做得了 v1 不判**）\n",
                   label, act->name ? act->name : "?", act->count, hits);
        }
        DerStep *ds = der_add(&pr->trace, n_added ? DER_HYP : DER_FACT);
        if (ds) { ds->subj = (Token *)act; ds->truth = +1;
                  cot_copy(ds->note, sizeof ds->note,
                           n_added ? "干跑：靠假定的前提过" : "词在 + 前提成立"); }
        pr->n_ok++;
        return 1;
    }
}

/* 跑一条过程（递归跑子过程）。返回"断点数"（0 = 每一步都过）。 */
static inline size_t proc_exec(AI *ai, const char *bare, int depth, ProcRun *pr, int dry) {
    if (!ai || !pr) return 0;
    if (depth > PROC_RUN_DEPTH_MAX) {
        printf("    （调用太深，停在第 %d 层）\n", depth);
        return 0;
    }
    Token *root = proc_root_of(ai, bare);
    if (!root) { printf("  [找不到过程：%s]\n", bare ? bare : "?"); return 0; }
    /* ---- ④ 执行栈与作用域：进一个过程 = 压一层假设（`who` = 这个过程的根）----
     * 回退用 pop（或按步 `hypo_del`）；**不管从哪儿 return，出来都必须回到进来的深度**。 */
    size_t hypo_in = ai->hypo.depth;
    int pushed = hypo_push(&ai->hypo, root);
    if (!pushed) printf("    （假设层满了：这一步的假定没地方放 —— v1 只在层够用时假定）\n");
    if (ai->hypo.depth > pr->hypo_peak) pr->hypo_peak = ai->hypo.depth;
    struct { size_t depth; } hypo_guard = { hypo_in };   /* 作用域守卫（防漏 pop） */
    (void)hypo_guard;
    const AssocEntry *steps[PROC_SHOW_MAX], *calls[PROC_SHOW_MAX], *backs[PROC_SHOW_MAX];
    const AssocEntry *conds[PROC_SHOW_MAX], *elses[PROC_SHOW_MAX], *excs[PROC_SHOW_MAX];
    size_t ns = 0, nc = 0, nb = 0, ncd = 0, nel = 0, nex = 0;
    for (size_t i = 0; i < root->count; ++i) {
        const AssocEntry *e = &root->assocs[i];
        if (edge_step_index(e) > 0)      { if (ns < PROC_SHOW_MAX) steps[ns++] = e; }
        else if (edge_is_cond(e))        { if (ncd < PROC_SHOW_MAX) conds[ncd++] = e; }
        else if (edge_is_else(e))        { if (nel < PROC_SHOW_MAX) elses[nel++] = e; }
        else if (edge_is_exc(e))         { if (nex < PROC_SHOW_MAX) excs[nex++] = e; }
        else if (edge_is_call(e))        { if (nc < PROC_SHOW_MAX) calls[nc++] = e; }
        else if (edge_is_back(e))        { if (nb < PROC_SHOW_MAX) backs[nb++] = e; }
    }
    if (ns > 1) qsort(steps, ns, sizeof steps[0], proc_step_ptr_cmp);
    printf("  [进过程] %s（%zu 步 + %zu 条件 + %zu 否则 + %zu 异常 + %zu 调用 + %zu 回边）\n",
           root->name ? root->name : "?", ns, ncd, nel, nex, nc, nb);
    size_t gaps_before = pr->n_gap;
    /* ---- 按步号逐步跑 ---- */
    for (size_t i = 0; i < ns; ++i) {
        if (pr->n_step >= PROC_RUN_STEP_MAX) {
            printf("    第 %d 步 …（到一次跑的步数上限 %d，停）\n",
                   edge_step_index(steps[i]), PROC_RUN_STEP_MAX);
            der_gap(&pr->trace, "到步数上限");
            pr->n_gap++;
            break;
        }
        pr->n_step++;
        (void)proc_step_run(ai, root, steps[i], edge_step_index(steps[i]), pr, "", dry);
    }
    /* ---- ⑤ 分支：条件边（成立走它自己的动作；不成立走 `[否则]` 那一支）----
     * 条件判据**就是前提那三档**（`ai_premise_holds`）：注意窗口里 / 图上够得着 / 够得着且有据。
     * 不另造一套"条件为真"的判据 —— §6.56 第 2 条说的就是"记号挂动作槽、判据用现成的"。 */
    for (size_t i = 0; i < ncd; ++i) {
        char cb[64];
        (void)proc_mark_bare(conds[i]->action ? conds[i]->action->name : NULL,
                             PROC_COND_PREFIX, cb, sizeof cb);
        /* ② **求值层样板先试**（默认关）：条件写成可计算的形式（`3<5`）时，词当然不在图里 ——
         * 所以要**排在图内判据前面**（第一版放在后面 ⇒ 永远走到"图里没有这个词"那条路）。 */
        int evaluated = 0, ev = 0;
        if (ai->eval_num && ai_eval_cmp(cb, &ev)) evaluated = 1;
        Token *x = evaluated ? NULL : (cb[0] ? pool_find(ai->pool, cb) : NULL);
        if (!evaluated && !x) {
            /* 条件词**图里根本没这个词** ⇒ 不是"没提到"，是"判不了"。照实说，并按不成立走
             * （不然一个拼错的条件会静默变成"成立"，那是最坏的一种骗人）。 */
            printf("    条件「%s」：图里没有这个词 ⇒ **判不了**（按不成立处理）\n", cb);
            if (nel > 0) {
                printf("      → 走 「[否则]」 那一支 %s\n",
                       elses[0]->main && elses[0]->main->name ? elses[0]->main->name : "?");
                pr->n_step++;
                (void)proc_step_run(ai, root, elses[0], 0, pr, "否则分支", dry);
            } else {
                printf("      没有 「[否则]」 那一支 → 跳过（不当成断点）\n");
                pr->n_skip++;
            }
            continue;
        }
        /* 求值层算出来了就用它的真值；否则走图内判据（提到过/够得着/有据） */
        if (evaluated)
            printf("    条件「%s」：**求值层算了一遍**（数值比较）⇒ %s\n", cb,
                   ev ? "成立" : "不成立");
        int holds = evaluated ? ev : ai_premise_holds(ai, x);
        if (holds) {
            printf("    条件「%s」**成立** → 走它那一支 %s\n", cb,
                   conds[i]->main && conds[i]->main->name ? conds[i]->main->name : "?");
            pr->n_step++;
            (void)proc_step_run(ai, root, conds[i], 0, pr, "条件分支", dry);
        } else if (nel > 0) {
            printf("    条件「%s」**不成立** → 走 「[否则]」 那一支 %s\n", cb,
                   elses[0]->main && elses[0]->main->name ? elses[0]->main->name : "?");
            pr->n_step++;
            (void)proc_step_run(ai, root, elses[0], 0, pr, "否则分支", dry);
        } else {
            printf("    条件「%s」**不成立**，也没有 `[否则]` 那一支 → 跳过（不当成断点）\n", cb);
            pr->n_skip++;
        }
    }
    /* ---- ⑤ 异常：哪一步失败了，就跑 `[异常:类型]` 那一支（v1 只跑一次；类型作标签）---- */
    if (pr->n_gap > gaps_before && nex > 0) {
        char xb[64];
        (void)proc_mark_bare(excs[0]->action ? excs[0]->action->name : NULL,
                             PROC_EXC_PREFIX, xb, sizeof xb);
        printf("    出错了 → 跑 「[异常:%s]」 那一支 %s\n", xb,
               excs[0]->main && excs[0]->main->name ? excs[0]->main->name : "?");
        size_t before = pr->n_gap;
        pr->n_step++;
        (void)proc_step_run(ai, root, excs[0], 0, pr, "异常处理", dry);
        if (pr->n_gap == before) {
            printf("      （处理步过了 —— **账照旧记**：出错的那一步仍然是断点，不假装没发生）\n");
            pr->n_exc_handled++;
        }
    }
    /* ---- 调用：递归跑子过程 ---- */
    for (size_t i = 0; i < nc; ++i) {
        const Token *tgt = calls[i]->main;
        if (!tgt || !tgt->name) continue;
        pr->n_call++;
        printf("    调用 %s：\n", tgt->name);
        /* 目标可能是 `[过程:名字]`（子过程），也可能是普通动作词 */
        if (strncmp(tgt->name, PROC_TOKEN_PREFIX, strlen(PROC_TOKEN_PREFIX)) == 0) {
            const char *bare = tgt->name + strlen(PROC_TOKEN_PREFIX);
            char sub[80];
            size_t k = 0;
            for (const char *q = bare; *q && *q != ']' && k + 1 < sizeof sub; ++q) sub[k++] = *q;
            sub[k] = 0;
            (void)proc_exec(ai, sub, depth + 1, pr, dry);
        } else {
            printf("      （这一条指向普通词 %s —— v1 只跑 `[过程:…]` 子过程）\n", tgt->name);
            der_gap(&pr->trace, "调用目标不是过程");
            pr->n_gap++;
        }
    }
    /* ---- 回边：跳回第 N 步，迭代有上限（防死循环） ---- */
    for (size_t i = 0; i < nb; ++i) {
        const Token *tgt = backs[i]->main;
        int back_to = 0;
        if (tgt && tgt->name) {
            if (strncmp(tgt->name, PROC_STEP_PREFIX, strlen(PROC_STEP_PREFIX)) == 0)
                back_to = (int)strtol(tgt->name + strlen(PROC_STEP_PREFIX), NULL, 10);
        }
        if (back_to <= 0) {
            printf("    回边 → %s：（不是 `[步骤:N]` 端点 ⇒ 回不到某一步，v1 只记一条）\n",
                   tgt && tgt->name ? tgt->name : "?");
            der_gap(&pr->trace, "回边端点不是步号");
            pr->n_gap++;
            continue;
        }
        /* 找到那一步，**再跑一遍**（按上限）；跑过几遍记在次数里 */
        int done = 0;
        for (size_t k = 0; k < ns; ++k)
            if (edge_step_index(steps[k]) == back_to) { done = 1; break; }
        if (!done) {
            printf("    回边 → 第 %d 步：（没有这一步）\n", back_to);
            der_gap(&pr->trace, "回边指向不存在的步");
            pr->n_gap++;
            continue;
        }
        /* ⑤ 循环条件：条件写在**回边自己的记号**里（`[回边:条件词]`）——
         * 每轮开头先看它还成不成立，不成立就跳出循环（while，不是 for）。 */
        char loop_cond[64] = {0};
        {
            const char *bc = edge_back_cond(backs[i]);
            if (bc && bc[0]) {
                size_t k = 0;
                while (bc[k] && bc[k] != ']' && k + 1 < sizeof loop_cond) {
                    loop_cond[k] = bc[k];
                    k++;
                }
                loop_cond[k] = 0;
            }
        }
        if (loop_cond[0]) {
            Token *lx = pool_find(ai->pool, loop_cond);
            if (!lx || !ai_premise_holds(ai, lx)) {
                printf("    回边 → 第 %d 步：循环条件「%s」**不成立** ⇒ 跳出循环（一次都不重跑）\n",
                       back_to, loop_cond);
                pr->n_skip++;
                continue;
            }
            printf("    回边 → 第 %d 步（循环条件「%s」成立 ⇒ 按上限 %d 次重跑，每轮再看一次）\n",
                   back_to, loop_cond, PROC_RUN_LOOP_MAX);
        } else {
            printf("    回边 → 第 %d 步（按上限 %d 次重跑）\n", back_to, PROC_RUN_LOOP_MAX);
        }
        for (int round = 0; round < PROC_RUN_LOOP_MAX; ++round) {
            if (loop_cond[0]) {      /* 每轮开头再看一次条件（while，不是 for） */
                Token *lx = pool_find(ai->pool, loop_cond);
                if (!lx || !ai_premise_holds(ai, lx)) {
                    printf("      （第 %d 轮：循环条件「%s」不成立了 ⇒ 停）\n", round + 1, loop_cond);
                    break;
                }
            }
            if (pr->n_step >= PROC_RUN_STEP_MAX) break;
            const AssocEntry *se = NULL;
            for (size_t k = 0; k < ns; ++k)
                if (edge_step_index(steps[k]) == back_to) { se = steps[k]; break; }
            pr->n_step++;
            /* **同一处判据**（`proc_step_run`）：重跑不过就停（别把同一件事判两遍） */
            if (!proc_step_run(ai, root, se, back_to, pr, "（重跑）", dry)) break;
        }
    }
    /* 出过程之前：**核一遍假设还在不在层里**（独立核对，不是自己说自己），再 pop 干净 */
    if (pushed) {
        for (size_t i = 0; i < ai->hypo.depth; ++i) {
            const HypoFrame *f = &ai->hypo.fr[i];
            for (size_t k = 0; k < f->n; ++k) {
                const HypoFact *ft = &f->f[k];
                if (hypo_find(&ai->hypo, NULL, ft->a, ft->rid, ft->b, NULL)) pr->n_assume_ok++;
            }
        }
        /* 只数这次的：核对的上限 = 这次加过、又没被回退撤掉的那些 */
        { size_t expect = pr->n_assume > pr->n_rollback ? pr->n_assume - pr->n_rollback : 0;
          if (pr->n_assume_ok > expect) pr->n_assume_ok = expect; }
        while (ai->hypo.depth > hypo_in) hypo_pop(&ai->hypo);
    }
    return pr->n_gap - gaps_before;
}

/* 跑一条过程并打总结 + 轨迹（`::proc run <名字>` 用；返回断点数）。 */
static inline size_t proc_run(AI *ai, const char *bare, ProcRun *out) {
    ProcRun pr;
    memset(&pr, 0, sizeof pr);
    der_reset(&pr.trace);
    /* **开关在引擎自己的入口上**（不只在外面的命令里）：关着一律不发车 ——
     * 免得换个调用方（C 层/别的命令）就绕过了"默认关"。 */
    if (!ai || !ai->proc_run) {
        printf("[过程执行引擎关着（AIP_PROCRUN=0 / ::proc off）—— 关着时这条路一个字都不动]\n");
        if (out) *out = pr;
        return 0;
    }
    Token *root = proc_root_of(ai, bare);
    if (!root) { printf("[没有这个过程：%s]\n", bare ? bare : "?"); if (out) *out = pr; return 0; }
    int dry = ai->proc_dry;
    pr.hypo_base = ai->hypo.depth;
    pr.hypo_peak = pr.hypo_base;
    printf("  [跑过程%s] %s\n", dry ? "·干跑（假设世界）" : "", root->name ? root->name : "?");
    der_goal(&pr.trace, root, 0, NULL);
    (void)proc_exec(ai, bare, 0, &pr, dry);
    /* 轨迹自己也要"知道"有几个断点（`der_gap` 只加一步、不动计数 —— 那是 `der_verify`
     * 的活儿；这里把数补上，免得读轨迹的人以为"gaps=0 就是全过"） */
    pr.trace.gaps = pr.n_gap;
    der_conclude(&pr.trace, pr.n_gap ? 0 : +1, pr.n_gap ? 20 : 70);
    printf("  汇总：%zu 步（含调用展开）—— %zu 步过 / **%zu 个断点** / 真执行 %zu 个行为 / 进 %zu 次子过程\n",
           pr.n_step, pr.n_ok, pr.n_gap, pr.n_ran, pr.n_call);
    if (pr.n_verified || pr.n_verify_fail)
        printf("  ① 步骤带对象：`der_verify` **独立复核通过 %zu 个 / 不过 %zu 个**"
               "（复核过的链并进了上面那条轨迹）\n", pr.n_verified, pr.n_verify_fail);
    if (pr.n_skip || pr.n_exc_handled)
        printf("  ⑤ 分支/异常：跳过 %zu 次（条件不成立且没否则支 / 循环条件不成立）；"
               "异常处理步跑通 %zu 次\n", pr.n_skip, pr.n_exc_handled);
    printf("  轨迹（复用 Derivation）：goal + %zu 步 + conclude —— %s\n",
           pr.trace.n > 0 ? pr.trace.n - 1 : 0,
           pr.n_gap ? "有断点 ⇒ **不许说跑通了**（断点在哪就停在哪）"
                    : "没有断点 ⇒ 每一步**现在都过得去**（v1 的判据只有：词在 + 前提成立 + "
                      "行为跑得成 —— **不等于「做完了」**）");
    printf("  假设层（执行栈与作用域）：进 %zu 层（每个过程一层，`who` = 过程根）/ 回退 %zu 次"
           "（按步撤假设）/ 出 —— 深度 %zu → %zu（跑完回到原样，一个字都没漏）\n",
           pr.hypo_peak - pr.hypo_base, pr.n_rollback, pr.hypo_base, ai->hypo.depth);
    if (pr.n_assume) {
        size_t expect = pr.n_assume > pr.n_rollback ? pr.n_assume - pr.n_rollback : 0;
        printf("  假定（只活在这次跑里）：%zu 条；回退撤掉 %zu 条；跑完核对 **%zu 条还在层里**"
               "（应当 %zu，`hypo_find` 独立核过）—— %s\n",
               pr.n_assume, pr.n_rollback, pr.n_assume_ok, expect,
               pr.n_assume_ok == expect ? "对得上" : "**对不上**（当 bug 处理）");
    }
    if (pr.n_defer)
        printf("  干跑推迟没执行的行为：%zu 个（实跑时它们会走受审通道真做）\n", pr.n_defer);
    printf("  %s：核对没动图；%s\n",
           dry ? "干跑（假设世界）" : "只读 + 受审",
           dry ? "行为步骤一个都没执行（假定能做），跑完假设层 pop 干净"
               : "真执行的那几步走的是老那条通道（有审查/预算/账）");
    if (out) *out = pr;
    return pr.n_gap;
}

/* ============================ ② 过程接进**思维**（默认关，只读） ============================
 *
 * 缺口（用户 2026-10-10 点的候选）：过程记号现在只有 `::op`（建）和 `::proc`（看/跑）两个**人的**入口，
 * AI 自己在思考时**认不出"这件事我以前记过过程"**。这一版接上：
 * 一轮思考收尾时，拿这一轮的话去对 `[过程:X]` 的根 —— X 在话里出现就算命中，
 * 然后在**痕**里写一句"这件事我记过过程 X（N 步）"；引擎开着时顺手**干跑一遍**（`proc_run` 的
 * 干跑是只读的：不执行行为、假定前提也只在假设层里），把"几步过/几个断点"并进痕。
 * **关着时一个字都不加**（默认关）。不动图：这一条只往痕和思考对象上写，没有任何写入。 */
static inline void think_note_process(AI *ai, char *const *words, size_t n) {
    if (!ai || !ai->proc_think || !words || !n) return;
    Think *t = (Think *)ai->think;
    if (!t) return;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *r = ai->pool->tokens[i];
        if (!r || !r->name || r->is_garbage) continue;
        if (strncmp(r->name, PROC_TOKEN_PREFIX, strlen(PROC_TOKEN_PREFIX)) != 0) continue;
        /* 过程名（去掉 `[过程:` 和结尾 `]`） */
        char bare[64];
        size_t k = 0;
        for (const char *q = r->name + strlen(PROC_TOKEN_PREFIX); *q && *q != ']' && k + 1 < sizeof bare; ++q)
            bare[k++] = *q;
        bare[k] = 0;
        if (!bare[0]) continue;
        /* 命中判据：这一轮的话里**出现过**这个名字（分词器可能把它切开 ⇒ 也试"首字"） */
        int hit = 0;
        for (size_t w = 0; w < n && !hit; ++w) {
            if (!words[w] || !words[w][0]) continue;
            if (strcmp(words[w], bare) == 0) { hit = 1; break; }
            /* 分词器会把「泡茶」切成 泡+茶 ⇒ 也认"**名字的前缀**"（至少一个汉字、且比名字短）。
             * 注意按**字节**判：一个汉字 3 字节（第一版写成 strlen==1，永远不成立 —— 实测没命中）。 */
            size_t lw = strlen(words[w]), lb = strlen(bare);
            if (lw >= 3 && lw < lb && strncmp(bare, words[w], lw) == 0) { hit = 1; break; }
        }
        if (!hit) continue;
        /* 数一下过程有多少步（只读） */
        size_t steps = 0;
        for (size_t e = 0; e < r->count; ++e)
            if (edge_step_index(&r->assocs[e]) > 0) steps++;
        char note[THINK_WHY];
        snprintf(note, sizeof note, "这件事我记过过程「%s」（%zu 步）", bare, steps);
        if (ai->proc_run) {
            size_t gaps = proc_run(ai, bare, NULL);      /* 干跑是只读的（不执行行为、假定只在假设层） */
            snprintf(note, sizeof note, "这件事我记过过程「%s」（%zu 步）→ 干跑一遍：%s",
                     bare, steps, gaps ? "有断点（停在断点上）" : "每一步都过得去");
        }
        think_step(t, TH_REFLECT, 1, 55, "这我记过过程", note);
        return;   /* 一条就够（多个命中时不再堆） */
    }
}

/* ============ ② 过程接进**决策**：这类问题直接用计划回答（默认关） ============
 *
 * 上一版（§6.75）只在**痕**里提一句"这件事我记过过程"，回答照旧走流水线。这一版让它**做决定**：
 * 问"怎么/如何/怎样 <过程名>"时，直接把过程**列出来**当回答（计划就是答案本身）。
 * 认法窄到"疑问词 + 命中一个我记过的过程"；引擎开着再附一句干跑结论（只读）。 */
static inline int dialog_try_proc_plan(AI *ai, char *const *words, size_t n,
                                      char *out, size_t out_cap) {
    if (!ai || !ai->proc_plan || !words || !n || !out || out_cap == 0) return 0;
    out[0] = 0;
    /* ① 这一句得是个"怎么做"的问法 */
    int howto = 0;
    for (size_t i = 0; i < n; ++i)
        if (words[i] && (strcmp(words[i], "怎么") == 0 || strcmp(words[i], "如何") == 0 ||
                         strcmp(words[i], "怎样") == 0 || strcmp(words[i], "咋") == 0)) howto = 1;
    if (!howto) return 0;
    /* ② 命中一个我记过的过程（名字或名字前缀出现在话里） */
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *r = ai->pool->tokens[i];
        if (!r || !r->name || r->is_garbage) continue;
        if (strncmp(r->name, PROC_TOKEN_PREFIX, strlen(PROC_TOKEN_PREFIX)) != 0) continue;
        char bare[64];
        size_t k = 0;
        for (const char *q = r->name + strlen(PROC_TOKEN_PREFIX); *q && *q != ']' && k + 1 < sizeof bare; ++q)
            bare[k++] = *q;
        bare[k] = 0;
        if (!bare[0]) continue;
        size_t lw = strlen(bare);
        int hit = 0;
        for (size_t w = 0; w < n && !hit; ++w) {
            if (!words[w] || !words[w][0]) continue;
            if (strcmp(words[w], bare) == 0) { hit = 1; break; }
            size_t l = strlen(words[w]);
            if (l >= 3 && l < lw && strncmp(bare, words[w], l) == 0) hit = 1;
        }
        if (!hit) continue;
        /* ③ 把计划列出来（步骤 / 条件 / 否则 / 异常 / 调用 / 回边） */
        size_t off = 0;
        off += (size_t)snprintf(out + off, out_cap > off ? out_cap - off : 0,
                                "我 记过 过程 「 %s 」 ：", bare);
        size_t nstep = 0;
        for (size_t e = 0; e < r->count && off + 24 < out_cap; ++e) {
            const AssocEntry *ed = &r->assocs[e];
            const char *nm = ed->main && ed->main->name ? ed->main->name : "?";
            if (edge_step_index(ed) > 0) {
                off += (size_t)snprintf(out + off, out_cap - off, " 第 %d 步 %s ；",
                                        edge_step_index(ed), nm);
                nstep++;
            } else if (edge_is_cond(ed)) {
                char cb[48]; (void)proc_mark_bare(ed->action ? ed->action->name : NULL,
                                                  PROC_COND_PREFIX, cb, sizeof cb);
                off += (size_t)snprintf(out + off, out_cap - off, " （%s 成立 才 %s）", cb, nm);
            } else if (edge_is_else(ed)) {
                off += (size_t)snprintf(out + off, out_cap - off, " 否则 %s ；", nm);
            } else if (edge_is_exc(ed)) {
                off += (size_t)snprintf(out + off, out_cap - off, " 出错 就 %s ；", nm);
            } else if (edge_is_call(ed)) {
                off += (size_t)snprintf(out + off, out_cap - off, " 调用 %s ；", nm);
            }
        }
        if (!nstep)
            off += (size_t)snprintf(out + off, out_cap - off, " （还 没 记 步骤）");
        if (ai->proc_run) {
            size_t gaps = proc_run(ai, bare, NULL);      /* 干跑：只读（结论进痕，不进回答） */
            off += (size_t)snprintf(out + off, out_cap - off, " ｜ 干跑 一遍 ： %s",
                                    gaps ? "有 断点" : "每 一步 都 过 得 去");
        } else {
            off += (size_t)snprintf(out + off, out_cap - off, " ｜ （执行 引擎 关着 ， 只 给 计划）");
        }
        return 1;
    }
    return 0;
}

/* ============================ ② 过程记号接进**对话**（默认关） ============================
 *
 * 用户 2026-10-10：三个候选都要。这一条是"AI 能靠说话把一件事记成过程、并跑它"：
 *   「记个过程 泡茶」      → `建过程 泡茶`
 *   「第 1 步 烧水」       → `加步骤 泡茶 烧水 1`（过程名从**当前唯一的那个过程**推）
 *   「跑 泡茶」/「跑过程」 → `::proc run`
 * 认法**窄**到三种（宁可漏，不可误触发）：不认识就整条不触发，走流水线。动作照样走
 * `token_op_run`（受审通道）—— 对话只是入口，不是特权。 */
/* 把 from 起剩下的词**拼成一个词**（分词器会把「泡茶」切成 泡+茶 —— §C 那条老坑：
 * 触发这一层拿到的是符号节点，不是原句；用户说的确实是「泡茶」，拼回去才是他说的那个词）。 */
static inline size_t dialog_join_words(char *const *words, size_t n, size_t from,
                                       char *buf, size_t cap) {
    size_t k = 0;
    if (buf && cap) buf[0] = 0;
    for (size_t i = from; i < n && i < from + 4; ++i) {
        if (!words[i] || !words[i][0]) continue;
        for (const char *q = words[i]; *q && k + 1 < cap; ++q) buf[k++] = *q;
    }
    if (buf && cap) buf[k] = 0;
    return k;
}

static inline int dialog_try_proc(AI *ai, char *const *words, size_t n, char *out, size_t out_cap) {
    if (!ai || !ai->proc_talk || !words || n < 2 || !out || out_cap == 0) return 0;
    out[0] = 0;
    /* ① 记个过程 <名字> / 建过程 <名字> */
    if (dialog_others_in_list((const char *const[]){"记", "建", "记个", "建个", NULL}, words[0])) {
        size_t j = 1;
        while (j < n && (strcmp(words[j], "个") == 0 || strcmp(words[j], "一个") == 0 ||
                         strcmp(words[j], "过程") == 0)) j++;
        char nm[64];
        if (j < n && dialog_join_words(words, n, j, nm, sizeof nm) > 0 && words[n - 1][0] != '?') {
            char note[160];
            char *a[1] = { nm };
            if (token_op_run(ai, "建过程", a, 1, note, sizeof note) >= 0) {
                snprintf(out, out_cap, "好 ， 过程 「 %s 」 记下 了 （ %s ）", nm, note);
                return 1;
            }
        }
        return 0;
    }
    /* ② 第 N 步 <动作>（N 认不出就自动排号） */
    if (strcmp(words[0], "第") == 0 || strncmp(words[0], "第", strlen("第")) == 0) {
        size_t j = 1;
        int idx = 0;
        if (j < n && strcmp(words[j], "步") != 0) { idx = atoi(words[j]); j++; }
        if (j < n && strcmp(words[j], "步") == 0) j++;
        if (j < n && j < n + 1) {
            /* 过程名：当前只有一个过程时就用它（多说一句"哪个过程"也可以：`第1步 泡茶 烧水`） */
            char pname[64] = {0};
            size_t k = 0, nproc = 0;
            for (size_t i = 0; i < ai->pool->count; ++i) {
                Token *t = ai->pool->tokens[i];
                if (!t || !t->name || t->is_garbage) continue;
                if (strncmp(t->name, PROC_TOKEN_PREFIX, strlen(PROC_TOKEN_PREFIX)) != 0) continue;
                nproc++;
                if (nproc == 1) {
                    const char *q = t->name + strlen(PROC_TOKEN_PREFIX);
                    while (*q && *q != ']' && k + 1 < sizeof pname) pname[k++] = *q++;
                    pname[k] = 0;
                }
            }
            if (nproc != 1) {
                snprintf(out, out_cap, "我 这儿 有 %zu 个 过程 —— 说 「 记个过程 <名字> 」 先 定 一个", nproc);
                return 1;
            }
            char note[160], act[64];
            (void)dialog_join_words(words, n, j, act, sizeof act);
            char *a[3] = { pname, act, NULL };
            size_t an = 2;
            char nb[16];
            if (idx > 0) { snprintf(nb, sizeof nb, "%d", idx); a[2] = nb; an = 3; }
            if (act[0] && token_op_run(ai, "加步骤", a, an, note, sizeof note) >= 0) {
                snprintf(out, out_cap, "记下 了 ： 过程 「 %s 」 第 %s 步 是 「 %s 」", pname,
                         idx > 0 ? nb : "（自动排）", act);
                return 1;
            }
        }
        return 0;
    }
    /* ③ 跑 <过程名> / 跑过程 [<名字>] */
    if (strcmp(words[0], "跑") == 0 || strcmp(words[0], "跑跑") == 0) {
        char nm[64];
        size_t from = (n >= 2 && strcmp(words[1], "过程") == 0) ? 2 : 1;
        if (dialog_join_words(words, n, from, nm, sizeof nm) == 0) return 0;
        const char *bare = nm;
        if (!proc_root_of(ai, bare)) {
            snprintf(out, out_cap, "没有 「 %s 」 这个 过程", bare);
            return 1;
        }
        if (!ai->proc_run) {
            snprintf(out, out_cap, "过程 「 %s 」 在 ， 可 执行 引擎 关着 （ ::proc on 打开 ）", bare);
            return 1;
        }
        size_t gaps = proc_run(ai, bare, NULL);
        snprintf(out, out_cap, "跑 完 了 「 %s 」 ： %s", bare,
                 gaps ? "有 断点 —— 停 在 断点 上" : "每 一步 都 过 得 去");
        return 1;
    }
    return 0;
}

/* ============================ ③ 他者视角进回答路径（默认关） ============================
 *
 * 用户 2026-10-07 那条尾巴：「他者视角」的四态判定（§6.45：都说认 / 都说不认 /
 * 有分歧（待验，不是否定）/ 没人说过）只有 `::others` 这个**看**的入口，
 * **复数主语**（"大家 / 他们 / 别人"）问出来的时候没有回答入口。
 *
 * 三条边界：
 *   · **默认关**（`ai->others_ask`）：关着时这条函数第一行就返回 0，行为与今天逐字节相同；
 *   · **只读**：他者信念只在**他的**名下，判定是现算的，不落库、不改图；
 *   · **分开说**：答复只说"他们怎么说"，我**自己**的知识另起半句 ——
 *     别人的看法不是我的知识（这一层的老规矩）。
 * 认法（宁可漏，不可误触发）：句首的**复数主语** + 一串"问法词"（怎么看/觉得/说…），
 * 剩下的整段交给 `rel_parse` —— 它认不出关系问题就**整条不触发**（流水线照常回答）。 */
static inline int dialog_trigger_try_others(AI *ai, char *const *words, size_t n,
                                            char *out, size_t out_cap) {
    if (!ai || !ai->others_ask || !words || n < 4 || !out || out_cap == 0) return 0;
    out[0] = 0;
    /* ① 句首的复数主语（允许前面垫一两个指代词） */
    size_t si = (size_t)-1;
    for (size_t i = 0; i < n && i < 3; ++i)
        if (dialog_others_in_list(DIALOG_OTHERS_SUBJ, words[i])) { si = i; break; }
    if (si == (size_t)-1) return 0;
    /* ② 跳过问法词，剩下的才是问题本身 */
    size_t j = si + 1;
    while (j < n && dialog_others_in_list(DIALOG_OTHERS_ASK, words[j])) j++;
    if (n - j < 3) return 0;
    /* ③ 交给关系解析（认不出就整条不触发 —— 宁可漏，不可误触发） */
    char jbuf[64];
    const char *aw = NULL, *bw = NULL, *rw = NULL;
    uint8_t rid = 0;
    int is_q = 0, is_neg = 0;
    if (!rel_parse(ai, words + j, n - j, jbuf, sizeof jbuf, &aw, &rid, &is_neg,
                   &bw, &is_q, &rw, 0) || !is_q || rid == 0)
        return 0;
    Token *A = pool_find(ai->pool, aw);
    Token *B = pool_find(ai->pool, bw);
    if (!A || !B) return 0;
    /* ④ 现算四态（只读；判定不看我的知识） */
    OtherBeliefView v;
    other_belief_view(ai, A, rid, B, &v);
    const char *rn = rel_name_of(ai, rid);
    const char *an = A->name ? A->name : "?";
    const char *bn = B->name ? B->name : "?";
    /* 我自己的知识：另起半句（"别人的看法 ≠ 我的知识"） */
    int own = rel_direct(A, rid, B) ? +1 : (rel_direct_neg(A, rid, B) ? -1 : 0);
    const char *own_txt = own > 0 ? "我 也 这么 看"
                        : (own < 0 ? "我 不 这么 看" : "我 自己 也 不 知道");
    switch (v.verdict) {
    case OBV_YES:
        snprintf(out, out_cap, "大家 都 说 ： %s %s%s %s（%zu 个人 说 过 ；%s）",
                 an, is_neg ? "不 " : "", rn ? rn : "?", bn, v.n_who, own_txt);
        break;
    case OBV_NO:
        snprintf(out, out_cap, "大家 都 说 ： %s 不 %s %s（%zu 个人 说 过 ；%s）",
                 an, rn ? rn : "?", bn, v.n_who, own_txt);
        break;
    case OBV_DEFER:
        snprintf(out, out_cap,
                 "这 件 事 大家 说 法 不 一致 ： 有人 说 %s %s %s 、 有人 说 不 %s"
                 "（%zu 认 / %zu 不认 —— 待验 ， 不是 否定 ；%s）",
                 an, rn ? rn : "?", bn, rn ? rn : "?", v.n_yes, v.n_no, own_txt);
        break;
    default:
        if (v.n_related)
            snprintf(out, out_cap,
                     "这 件 事 他们 没 说 过（只 有 %zu 条 相关 说法 ， 不 算 答案 ；%s）",
                     v.n_related, own_txt);
        else
            snprintf(out, out_cap, "这 件 事 他们 没 说 过（%s）", own_txt);
        break;
    }
    return 1;
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

/* §6.139 脏思考检查器的**动作**：截停 + 记账 + 痕 + 元认知提醒。
 * 抽成函数是为了**可测**（optest 直接喂字符串看它截到哪儿、账怎么记）。
 * 返回 1 = 真的截了；`kind_out` 拿到判据（1 词级 / 2 句级）。 */
static inline int ai_dirty_apply(AI *ai, char *out, size_t cap, size_t *kind_out) {
    if (kind_out) *kind_out = 0;
    if (!ai || !out || cap == 0 || !ai->dirty_guard) return 0;
    size_t before = 0;
    for (const char *p = out; *p; ) { while (*p == ' ') p++; if (!*p) break; before++; while (*p && *p != ' ') p++; }
    size_t dk = 0;
    size_t cut = ai_dirty_cut(out, &dk);
    if (cut == 0) return 0;
    while (cut > 0 && out[cut - 1] == ' ') cut--;     /* 别留下悬空的空格 */
    out[cut] = 0;
    size_t after = 0;
    for (const char *p = out; *p; ) { while (*p == ' ') p++; if (!*p) break; after++; while (*p && *p != ' ') p++; }
    if (before > after) ai->n_dirty_words += before - after;
    ai->n_dirty_stops++;
    if (!out[0]) snprintf(out, cap, "……");            /* 整句都被截光：只回省略号 */
    if (kind_out) *kind_out = dk;
    char dwhy[THINK_WHY];
    snprintf(dwhy, sizeof dwhy, "%s：连着重复，说到那儿就停（截掉 %zu 个词）",
             dk == 1 ? "同一个词连着说" : "同一句话连着说", before - after);
    cot_push(ai, COT_DONE, 0, 0, "脏思考：重复输出，强制停", dwhy);
    ai->n_cot++;
    micro_remind(ai, MICRO_REMIND_DIRTY_REPEAT, NULL);
    return 1;
}

/* §6.168 **追问回话 → 教学**。返回 1 = 这一轮被它接住（out 里就是答复）。
 *
 * 场景：上一轮用户问 `鲸鱼 会 游 吗`，关系层推不出来（并且**明确承认**了），
 * 这一轮用户只回「会」/「不 会」/「对」/「不对」。
 * 以前这条回话会掉进流水线，答出「会 怕 得 快 我 答 上」这种词串 —— 互动就断在这儿。
 *
 * 纪律：
 *   - 只认**一两词的短回话**，且每个词都在白名单里（多余一个词就是别的话，不猜）；
 *   - 学的是**上一轮那个问句**（A/R/B 都是当时解析出来的），不是从输入里重新解析 ——
 *     回话里根本没有 A/B；
 *   - 极性相乘：问「不 R」答「对」⇒ 记成 `不 R`；问「不 R」答「不」⇒ 记成 `R`；
 *   - 写入走 `ai_write_rel`（统一通道，::safe 里能看理由），并记 `n_rel_learned`。
 */
static inline int agent_askback_reply(AI *ai, const char *input, char *out, size_t cap) {
    if (!ai || !ai->ask_pending || !input || !out || cap == 0) return 0;
    if (!ai->ask_a[0] || !ai->ask_b[0] || ai->ask_rid == 0) return 0;
    size_t n = 0;
    char **w = ai_tokenize(ai, input, &n);
    if (!w) return 0;
    /* §6.249 **两套回话要分开**：
     *   ① **答词**（会/能/可以/不 会/不 能）—— 回答的是**问句**，极性是绝对的；
     *   ② **判词**（对/是/没错/不 对/不 是）—— 评的是**我刚才那句**，极性相对（fact_neg ^ no）。
     * 以前两套混着按"相对"处理 ⇒ 我猜「大概 不 会」、用户答「会」（其实是"会飞"）
     * 被教成「不 会 飞」（实测极性反了）。
     * ⚠ `不 会` 是两个 token（`不` + `会`），逐 token 分类会一半判词一半答词 ⇒ 先拼成整串再比。 */
    int yes = 0, no = 0, junk = 0, saw_judge = 0, saw_answer = 0;
    char j[64];
    size_t jo = 0;
    for (size_t i = 0; i < n && jo + 1 < sizeof j; ++i) {
        const char *t = w[i] ? w[i] : "";
        if (!strcmp(t, "的") || !strcmp(t, "了") || !strcmp(t, "啊") ||
            !strcmp(t, "呀") || !strcmp(t, "吧") || !strcmp(t, "呢")) continue;
        jo += (size_t)snprintf(j + jo, sizeof j - jo, "%s%s", jo ? " " : "", t);
    }
    if (n < 1 || n > 3) junk = 1;
    if (!junk) {
        /* §6.251 `可以` 挪到**判词**：「桌子 会 飞 吗」回「可以」不是"能飞"，是"行吧/同意"；
         * 能力问句的肯定回答是「会/能」。 */
        static const char *ay[] = {"会", "能", NULL};
        static const char *an[] = {"不 会", "不会", "不 能", "不能", NULL};
        static const char *jy[] = {"对", "是", "嗯", "好", "行", "没 错", "没错",
                                   "对 的", "是 的", "可以", "好 的", "行 了",
                                   "嗯 嗯", "嗯嗯", NULL};
        static const char *jn[] = {"不 对", "不对", "不 是", "不是", "不", "没",
                                   "没 有", "没有", "错", NULL};
        for (size_t i = 0; ay[i]; ++i) if (!strcmp(j, ay[i])) { yes = 1; saw_answer = 1; }
        for (size_t i = 0; an[i]; ++i) if (!strcmp(j, an[i])) { no = 1; saw_answer = 1; }
        for (size_t i = 0; jy[i]; ++i) if (!strcmp(j, jy[i])) { yes = 1; saw_judge = 1; }
        for (size_t i = 0; jn[i]; ++i) if (!strcmp(j, jn[i])) { no = 1; saw_judge = 1; }
        if (!yes && !no) junk = 1;
    }
    ai_free_words(w, n);
    if (junk || (!yes && !no)) return 0;

    /* §6.174 问句里可能是**新词**（`恐龙 会 飞 吗`）：当时只挂了追问、没造词；
     * 用户现在回话确认了 ⇒ 到**写路径**上再造词（与 `::teach` 同一个口径）。 */
    Token *A = pool_find(ai->pool, ai->ask_a);
    Token *B = pool_find(ai->pool, ai->ask_b);
    uint8_t rid = ai->ask_rid;
    int fact_neg = ai->ask_fact_neg;    /* 先取下来：drop 会把字段清干净（§6.188） */
    agent_ask_drop(ai);                 /* 这条追问用过就没了（无论成不成；标记标垃圾） */
    if (!A && ai->ask_a[0] && strlen(ai->ask_a) <= 12) A = pool_token_intern(ai->pool, ai->ask_a);
    if (!B && ai->ask_b[0] && strlen(ai->ask_b) <= 12) B = pool_token_intern(ai->pool, ai->ask_b);
    if (!A || !B || rid == 0) return 0;
    /* §6.172b/§6.249 极性的两种口径（见上面的 saw_judge / saw_answer）：
     *   答词（会/不会…）⇒ 绝对极性（问的是"会不会"，答"会"就是会）；
     *   判词（对/不对…）⇒ 相对我刚才那句（fact_neg ^ 否定）。
     * 没有明确分类时（比如只说「嗯」）按判词口径（更保守）。 */
    /* 判词的基准极性：**kind=2**（我给了确定答案）用"我说的那句"（fact_neg，§6.172b）；
     * **kind=1**（我没把握/是猜的）用**问句本身的极性**（ask_neg）—— 猜出来的那句话
     * 极性在图上没有落点，实测按 fact_neg 会反（`桌子 不 会 飞 吗` 回「对」教成「会 飞」）。 */
    int base = fact_neg;    /* §6.250 fact_neg 修对之后，两种 kind 都用它 */
    int tneg;
    if (saw_answer && !saw_judge) tneg = no ? 1 : 0;
    else                          tneg = base ^ (no ? 1 : 0);
    if (ai_write_rel(ai, A, B, rid, tneg, AI_WSRC_RELATION) != 0) return 0;
    ai->n_rel_learned++;
    const char *rn = rel_name_of(ai, rid);
    snprintf(out, cap, "好 ， 记下 了 ： %s %s%s %s（你 说 的）",
             A->name, tneg ? "不 " : "", rn ? rn : "?", B->name);
    return 1;
}

/* §6.172 **纠正已有答案**（kind=2）：上一轮我给了一个**确定答案**，
 * 这一轮用户说「不 对 / 不 是 / 错 了 / 说 错 了」⇒ 那条事实**改口**（极性取反）。
 * 说「对 / 是 / 没 错」⇒ 只是确认：回一句"那我就记着"，**不写图**。
 * 别的话 ⇒ 追问作废（只等一轮）。
 *
 * 为什么需要它：实测今天 `猫 属于 动物 吗` ⇒「对 ， 猫 -[属于]-> 动物」，
 * 用户答「不 对」⇒「没 关系 谁 都 会 有 错」（闲聊模板）—— **知识一点没动**，
 * 再问还是「对」。互动里"我说错了、你纠正"这条路等于不存在。
 *
 * 纪律：只认**短回话白名单**（多的当闲聊，不猜）；改口靠 `token_tag_*` 的
 * "观测压过同一条边的对立位"（`g_obs_revise` 默认 1）⇒ 同一条边上不会正负并存；
 * 只改**我说过的那条**（A/R/B 都来自上一轮的解析，不从回话里重新解析）。 */
static inline int agent_correct_reply(AI *ai, const char *input, char *out, size_t cap) {
    if (!ai || !ai->ask_pending || !input || !out || cap == 0) return 0;
    if (!ai->ask_a[0] || !ai->ask_b[0] || ai->ask_rid == 0) return 0;
    size_t n = 0;
    char **w = ai_tokenize(ai, input, &n);
    if (!w) return 0;
    char j[96];
    size_t off = 0;
    for (size_t i = 0; i < n && off + 1 < sizeof j; ++i) {
        const char *t = w[i] ? w[i] : "";
        /* 语气词先剔掉（「错 了」⇒「错」）；别的词原样留着拼 */
        if (!strcmp(t, "的") || !strcmp(t, "了") || !strcmp(t, "啊") ||
            !strcmp(t, "呀") || !strcmp(t, "吧") || !strcmp(t, "呢")) continue;
        off += (size_t)snprintf(j + off, sizeof j - off, "%s%s", off ? " " : "", t);
    }
    ai_free_words(w, n);
    static const char *corr[] = {"不 对", "不 是", "错", "说 错", "不 正 确", NULL};
    static const char *conf[] = {"对", "是", "嗯", "没 错", "好", "行", NULL};
    int is_corr = 0, is_conf = 0;
    for (size_t i = 0; corr[i]; ++i) if (strcmp(j, corr[i]) == 0) { is_corr = 1; break; }
    if (!is_corr)
        for (size_t i = 0; conf[i]; ++i) if (strcmp(j, conf[i]) == 0) { is_conf = 1; break; }
    /* §6.172c **整句说法**也认：`你 说 的 不 对` / `你 说 错 了`（语气词已剔掉）。
     * 只认"你/您/这/那 + 纠正词"这个形状 —— `我 说 的 不 对`（用户说自己说错了）
     * 不该拿来改**我的**知识，落空更安全。 */
    if (!is_corr && !is_conf) {
        size_t jl = strlen(j);
        static const char *lead[] = {"你", "您", "这", "那", NULL};
        for (size_t i = 0; !is_corr && lead[i]; ++i) {
            size_t ll = strlen(lead[i]);
            if (strncmp(j, lead[i], ll) != 0 || jl <= ll) continue;
            for (size_t k = 0; !is_corr && corr[k]; ++k) {
                size_t cl = strlen(corr[k]);
                if (jl > cl && j[jl - cl - 1] == ' ' && strcmp(j + jl - cl, corr[k]) == 0)
                    is_corr = 1;
            }
        }
    }
    if (!is_corr && !is_conf) { agent_ask_drop(ai); return 0; }   /* 不是短回话：作废 */
    Token *A = pool_find(ai->pool, ai->ask_a);
    Token *B = pool_find(ai->pool, ai->ask_b);
    uint8_t rid = ai->ask_rid;
    int fact_neg = ai->ask_fact_neg;
    agent_ask_drop(ai);
    if (!A || !B || rid == 0) return 0;
    const char *rn = rel_name_of(ai, rid);
    if (is_conf) {                        /* 确认：不写图，只说"记着" */
        snprintf(out, cap, "嗯 ， 那 我 就 这 么 记 着 ： %s %s%s %s",
                 A->name, fact_neg ? "不 " : "", rn ? rn : "?", B->name);
        return 1;
    }
    int want = fact_neg ? 0 : 1;          /* 取反 */
    if (ai_write_rel(ai, A, B, rid, want, AI_WSRC_RELATION) != 0) return 0;
    ai->n_revised++;
    ai->n_rel_learned++;
    /* §6.172d **留一个 `[修正:…]` 标记节点**（与陈述那条改口路一致）：
     * 这样"这件事被改口过"在图里**查得到**（`::edges [修正:猫_不_属于_动物]`），
     * 而不是只在 `::stats` 的计数里。走受审通道；同名 intern ⇒ 改口多次只有一个标记。 */
    if (!micro_of(ai)->safe.mod_off) {
        ModIntent in;
        memset(&in, 0, sizeof in);
        in.type = MOD_ADD_TOKEN;
        in.target = A;
        agent_revise_name(in.name, sizeof in.name, A->name, want,
                          rel_name_of(ai, rid), B->name);
        snprintf(in.source, sizeof in.source, "来源:改口");
        (void)mod_propose(&micro_of(ai)->queue, &in);
        /* **当场过安全点**：这一轮在 `agent_step` 开头就返回了（没走到 micro_step），
         * 不冲一下的话标记要等**下一个**文本回合才落地 —— 实测 `::edges [修正:…]` 当场查不到。
         * `micro_drain_queue` 末尾清队列，所以这里冲完不会把意图留到下一轮。 */
        (void)micro_drain_queue(ai);
    }
    snprintf(out, cap, "改 过来 了 ： %s %s%s %s（我 刚才 说 反 了）",
             A->name, want ? "不 " : "", rn ? rn : "?", B->name);
    return 1;
}

/* §6.242 **复句：一句话里多条陈述**（`猫 属于 动物，狗 属于 动物`）。
 * 实测以前整句掉兜底。做法：**在 token 层按分隔符切子句**（分词器会把 ，；。 切成独立 token，
 * 比在字节层切安全得多 —— 字节层切会把多字节标点切碎），逐条走既有学习链
 * （看法/并列/地点/关系/造词）；只有**全部子句都学下来**才算成功（有问句或学不动就整句回落）。 */
static inline char *agent_learn_multi(AI *ai, char *const *words, size_t n, float *out_conf) {
    if (!ai || ai->rel_off || !words || n < 5 || n > 24) return NULL;
    size_t seg[9]; size_t nseg = 0;
    seg[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *t = words[i];
        if (!t) continue;
        if (!strcmp(t, "，") || !strcmp(t, ",") || !strcmp(t, "；") || !strcmp(t, ";") ||
            !strcmp(t, "。") || !strcmp(t, ".")) {
            if (nseg + 1 >= sizeof seg / sizeof seg[0]) return NULL;
            seg[++nseg] = i + 1;
        }
    }
    if (!nseg) return NULL;
    size_t ncl = nseg + 1;
    char out[384]; size_t off = 0; size_t learned_n = 0;
    /* §6.243 省略/指代：`猫 属于 动物，狗 也 是` —— 后一句的主语换人、谓语宾语全省。
     * 记住上一句的（谓语词, 宾语），遇到 `[X][也][R]` 就补成 `[X][上一句谓语][上一句宾语]`。 */
    char prev_rw[16] = {0}, prev_obj[40] = {0};
    for (size_t c = 0; c < ncl; ++c) {
        size_t b = seg[c];
        size_t e = (c + 1 < ncl) ? seg[c + 1] - 1 : n;   /* 去掉分隔符那个 token */
        if (e <= b || e - b < 3 || e - b > 8) return NULL;
        char *const *cw = words + b;
        size_t cn = e - b;
        if (ai_has_question_word(cw, cn)) return NULL;
        float rc = 0.5f;
        char *r = NULL;
        /* 省略句：`[X][也][R]`（也/同样），用上一句补全 */
        if (!r && cn == 3 && prev_rw[0] && prev_obj[0] &&
            (!strcmp(cw[1], "也") || !strcmp(cw[1], "都"))) {
            uint8_t r1 = rel_id_of(ai, cw[2], 0), r0 = rel_id_of(ai, prev_rw, 0);
            int same = (r1 && r0 && r1 == r0) ||
                       (!strcmp(cw[2], "是") && r0 == REL_IS_A) ||
                       (!strcmp(prev_rw, "是") && r1 == REL_IS_A);
            if (same) {
                char *const syn[3] = {cw[0], prev_rw, prev_obj};
                r = agent_learn_relation(ai, syn, 3, &rc);
                if (r) {
                    /* 换成"补全后"的说明，别让用户以为是原句 */
                    char fix[160];
                    snprintf(fix, sizeof fix, "记下 了 ： %s %s %s（接着 上 一 句）",
                             cw[0], prev_rw, prev_obj);
                    free(r); r = (char *)malloc(strlen(fix) + 1);
                    if (r) memcpy(r, fix, strlen(fix) + 1);
                }
            }
        }
        if (!r) r = agent_learn_belief(ai, cw, cn, &rc);
        if (!r) { int cq = 0; r = agent_conj_both(ai, cw, cn, &cq, &rc); }
        if (!r) { int cq = 0; r = agent_conj_obj(ai, cw, cn, &cq, &rc); }
        if (!r) r = agent_learn_locative(ai, cw, cn, &rc);
        if (!r) r = agent_learn_relation(ai, cw, cn, &rc);
        if (!r) r = agent_learn_new_words(ai, cw, cn, &rc);
        if (!r) return NULL;
        /* 记下这句的谓语/宾语，供下一句省略用 */
        {
            char jb[64]; const char *aw2 = NULL, *bw2 = NULL, *rw2 = NULL;
            uint8_t rid2 = 0; int neg2 = 0, q2 = 0;
            if (rel_parse(ai, cw, cn, jb, sizeof jb, &aw2, &rid2, &neg2, &bw2, &q2, &rw2, 0) &&
                rw2 && bw2 && strlen(rw2) < sizeof prev_rw && strlen(bw2) < sizeof prev_obj) {
                snprintf(prev_rw, sizeof prev_rw, "%s", rw2);
                snprintf(prev_obj, sizeof prev_obj, "%s", bw2);
            } else { prev_rw[0] = 0; prev_obj[0] = 0; }
        }
        const char *body = r;
        /* 各条自带的抬头都去掉（学过的回「我 知道 ： 」、新学的回「记下 了 ： 」）*/
        static const char *pres[] = {"记下 了 ： ", "我 知道 ： ", NULL};
        for (size_t pi = 0; pres[pi]; ++pi)
            if (strncmp(body, pres[pi], strlen(pres[pi])) == 0) { body += strlen(pres[pi]); break; }
        if (off + strlen(body) + 8 < sizeof out) {
            off += (size_t)snprintf(out + off, sizeof out - off, "%s%s",
                                    learned_n ? " ； " : "", body);
            learned_n++;
        }
        free(r);
    }
    if (!learned_n) return NULL;
    char full[420];
    snprintf(full, sizeof full, "记下 了 ： %s", out);
    if (out_conf) *out_conf = 0.75f;
    if (learned_n > 1) ai->n_rel_learned += learned_n - 1;
    char *rr = (char *)malloc(strlen(full) + 1);
    if (rr) memcpy(rr, full, strlen(full) + 1);
    return rr;
}


/* §6.262 前置声明（定义在下面"自我/能力"那一块附近）*/
static inline int agent_rewrite_where(const char *in, char *out, size_t cap);

static inline size_t agent_step(Agent *a, const char *input, char *out, size_t out_cap) {
    if (!a || !input || !out || out_cap == 0) return 0;
    out[0] = 0;

    AI *ai = a->brain;
    ai->pool->current_tick++;
    a->cycle++;
    /* §6.168 追问回话：上一轮我说"推不出来"的那个问句，这一轮用户只回
     * 「会/对/不 会/不 对」⇒ 当教学写进图。说了别的话 ⇒ 这条追问作废（只等一轮）。 */
    if (!ai->ask_restored) agent_ask_restore(ai);   /* §6.188：载入后的第一轮接回追问 */
    /* §6.202 **空输入不算回话**：空闲思考/空行也会走这里，以前会把挂着的追问直接作废
     * （`agent_ask_drop` ⇒ 标记标垃圾、边 -1）⇒ "空闲是只读"被破坏（反向整包红在这里）。
     * 空行只是"没人说话"，追问继续挂着。 */
    int empty_turn = 1;
    for (const char *q = input; q && *q; ++q)
        if (*q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') { empty_turn = 0; break; }
    /* §6.217 记"用户说了什么"——但**元问题本身不记**（「我刚才说了什么」不是内容，
     * 记了就会答成"你刚才说的是这句问题本身"，实测踩过）。 */
    if (!empty_turn && input && input[0] != ':' &&
        !agent_history_kind(input) && !agent_repeat_request(input))
        agent_history_record(ai, input);
    if (ai->ask_pending && !empty_turn) {
        if (ai->askback) {
            /* kind 1 = 我没把握（回一句就教）；kind 2 = 我给了确定答案（回「不对」就改口） */
            int done = (ai->ask_kind == 2) ? agent_correct_reply(ai, input, out, out_cap)
                                           : agent_askback_reply(ai, input, out, out_cap);
            if (done) {
                ai->turns++;
                return strlen(out);
            }
        }
        agent_ask_drop(ai);
    }
    /* §6.217 「刚才聊了什么 / 我刚才说了什么」：从最近几轮**用户原话**里答（不用生成）*/
    {
        int hk = agent_history_kind(input);
        if (hk) {
            if (hk == 2) {
                if (ai->hist_n == 0) snprintf(out, out_cap, "你 还 没 说 过 什么");
                else {
                    size_t slot = (ai->hist_head + 6 - 1) % 6;
                    snprintf(out, out_cap, "你 刚才 说 的 是 ： %s", ai->hist[slot]);
                }
            } else {
                if (ai->hist_n == 0) snprintf(out, out_cap, "我们 还 没 聊 过 什么");
                else {
                    size_t off = (size_t)snprintf(out, out_cap, "刚才 我们 聊 了 ： ");
                    size_t n_list = ai->hist_n < 3 ? ai->hist_n : 3;
                    for (size_t k = 0; k < n_list && off + 1 < out_cap; ++k) {
                        size_t idx = (ai->hist_head + 6 - n_list + k) % 6;
                        off += (size_t)snprintf(out + off, out_cap - off, "%s%s",
                                                k ? " ／ " : "", ai->hist[idx]);
                        if (off >= out_cap) { out[out_cap - 1] = 0; break; }
                    }
                }
            }
            ai->turns++;
            return strlen(out);
        }
    }
    /* §6.216 「再说一遍」：原样重复上一句答复（不用生成） */
    if (!ai->ask_pending && agent_repeat_request(input)) {
        if (ai->last_reply[0]) snprintf(out, out_cap, "我 刚才 说 的 是 ： %s", ai->last_reply);
        else                   snprintf(out, out_cap, "我 还 没 说 过 什么");
        ai->turns++;
        return strlen(out);
    }
    /* §6.261 **"自我/能力"类问题统一路由**（以前是散在 agent_step 里的 6 个 if 块）。
     * 顺序就是优先级，**从具体到宽泛** —— 踩过的坑：`你 会 什么 语言` 被"你会什么"（能做什么）
     * 先抢走。现在顺序写在一处、带编号，加新判据时看这张表就知道该插在哪。 */
    {
        const char *sr = agent_self_route(input, out, out_cap);
        if (sr) { ai->turns++; return strlen(out); }
    }
    /* §6.203 没有挂着的追问时，裸短回话给一句实话（别掉进流水线出词串） */
    if (!ai->ask_pending) {
        int bk = agent_bare_reply(input);
        if (bk == 1) {
            const char *r = "嗯 ， 好";
            snprintf(out, out_cap, "%s", r);
            ai->turns++;
            return strlen(out);
        } else if (bk == 2) {
            const char *r = "哦 ？ 哪儿 不 对 ？ 我 刚才 说 的 是 哪 一 句";
            snprintf(out, out_cap, "%s", r);
            ai->turns++;
            return strlen(out);
        } else if (bk == 3) {
            /* 「没听懂」：把上一句再说一遍（比答词串有用得多）*/
            if (ai->last_reply[0])
                snprintf(out, out_cap, "没 讲 清 吗 ？ 我 再 说 一 遍 ： %s", ai->last_reply);
            else
                snprintf(out, out_cap, "那 我 换 个 说 法 —— 你 想 问 哪 方 面 ？");
            ai->turns++;
            return strlen(out);
        }
    }
    /* §6.135 分段计时的起点（关着时 `ai_seg_mark` 返回 0，`ai_seg_lap` 一次都不读表）。 */
    uint64_t seg_t = ai_seg_mark(ai);
    /* §6.119：每轮开头复查一遍条件缓存 —— 假设撤了、而无条件也推得出来的，就**升格**（摘前提）。 */
    ai_hypo_cache_sweep(ai);
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

    ai_seg_lap(ai, AI_SEG_HEAD, &seg_t);
    /* 感知 */
    size_t n;
    /* §6.120：**逐字模式**（默认关）时不分词 —— 先顺序接地，再把字符当"词"；
     * 关着时一个字都不变（还是 `ai_tokenize`，用户明确要求现在不要撤分词器）。 */
    /* §6.255 **指代消解**（句首 它/它们/这个/那个 + 上一轮主语）：
     * 实测 `猫 属于 动物` 之后问 `它 会 飞 吗`，答复是「我 不 确定 它 会 不 会 飞」——
     * 代词没落地，等于没回答。这里在**分词之前**把代词换成上一轮的主语
     * （只换句首那一处；`他/她` 不换 —— 那是"他者模型"的人，不是指代）。
     * 用栈上的缓冲拼，不动 `input`（历史记录仍记用户原话）。 */
    char decl_in[256];
    const char *input_eff = input;
    {
        const char *q = input;
        while (q && (*q == ' ' || *q == '\t')) q++;
        static const char *pron[] = {"它们", "它", "这个", "那个", NULL};
        if (q && ai->last_subject[0]) {
            for (size_t i = 0; pron[i]; ++i) {
                size_t pl = strlen(pron[i]);
                if (strncmp(q, pron[i], pl) != 0) continue;
                if (q[pl] && q[pl] != ' ' && q[pl] != '\t') continue;
                const char *rest = q + pl;
                snprintf(decl_in, sizeof decl_in, "%s%s", ai->last_subject, rest);
                input_eff = decl_in;
                break;
            }
        }
    }
    {   /* §6.263 「教你 / 告诉你 / 教我」这种**引导语**先剥掉（不做这步，后面的学习链
         * 拿到的是 `教/你/…`，要么出词串要么学不进）。只在句首、且剥完还有内容时才剥。 */
        const char *q = input_eff;
        while (*q == ' ' || *q == '\t') q++;
        static const char *lead[] = {"告诉你", "教你", "教我", "帮我记住", "请记住", "记住", "告诉 你", "教 你", "教 我", " teaches"};

        for (size_t i = 0; lead[i]; ++i) {
            size_t ll = strlen(lead[i]);
            if (strncmp(q, lead[i], ll) != 0) continue;
            const char *rest = q + ll;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (!*rest) break;
            snprintf(decl_in, sizeof decl_in, "%s", rest);
            input_eff = decl_in;
            break;
        }
    }
    /* §6.269 「教你 会飞 的 动物 是 鸟」这类**规则句**：明说学不进（别掉流水线出词串）。 */
    if (agent_rule_teach(input_eff) && input_eff != input) {
        snprintf(out, out_cap,
                 "这 是 一 条 规则（「…的…是…」），我 只 学 事 实：「A 关系 B」。"
                 "你 先 教「鸟 会 飞」和「鸟 属于 动物」，多 跳 推 理 会 用 上 它 们。");
        ai->turns++;
        return strlen(out);
    }
    {   /* §6.268 「那狗呢 / 狗呢」⇒ 「狗 <上一句的关系> <上一句的宾语> 吗」*/
        char el[160];
        if (agent_rewrite_ellipsis(ai, input_eff, el, sizeof el)) {
            snprintf(decl_in, sizeof decl_in, "%s", el);
            input_eff = decl_in;
        }
    }
    {   /* §6.262 「X在哪 / X住哪」⇒ 「X 在 什么」（必须在分词前：`我在` 会被当成一个词）*/
        char wh[224];
        if (agent_rewrite_where(input_eff, wh, sizeof wh)) {
            snprintf(decl_in, sizeof decl_in, "%s", wh);
            input_eff = decl_in;
        }
    }
    char **words = ai->char_stream ? ai_char_tokenize(ai, input_eff, &n)
                                   : ai_tokenize(ai, input_eff, &n);
    /* §6.268 记下"这是不是一个问句、问的是什么（关系/极性/宾语）"——「那狗呢」靠它补全。
     * 放在这里（分词后、所有路径之前）一处记录，比在各条答复路径里分别记可靠。 */
    if (words && n >= 3) {
        char jb[64];
        const char *aw2 = NULL, *bw2 = NULL, *rw2 = NULL;
        uint8_t rid2 = 0;
        int neg2 = 0, q2 = 0;
        if (rel_parse(ai, words, n, jb, sizeof jb, &aw2, &rid2, &neg2, &bw2, &q2, &rw2, 0) &&
            q2 && rid2 && bw2 && strlen(bw2) < sizeof ai->last_q_b) {
            ai->last_q_rid = rid2;
            ai->last_q_neg = (uint8_t)(neg2 ? 1 : 0);
            snprintf(ai->last_q_b, sizeof ai->last_q_b, "%s", bw2);
        }
    }
    /* 视角切换会把 words/n 往后挪两个词，但**释放必须用原始的指针** ——
     * 不然 free 的是数组中间，是堆破坏（不是崩一下就完的那种）。 */
    char **words_alloc = words;
    size_t n_alloc = n;
    /* §6.120 **逐字模式的安全上限**：整行语料（几十个字）在逐字模式下会变成几十个"词"，
     * 而下游有些路径按"一句话几个词"写的（固定数组），实测 `driveprobe` 直接 SIGSEGV ✗。
     * 所以逐字模式只用**最近 32 个字**（顺序仍然是接地的顺序）；分词器那条路不受影响。
     * ⚠ 只能在这里挪 `words`：`words_alloc` 必须still 指向**原始**数组（下面 free 用它）。 */
    if (ai->char_stream && n > 32) { words += (n - 32); n = 32; }
    ai_seg_lap(ai, AI_SEG_TOK, &seg_t);
    if (!words || n == 0) {
        /* **没人说话的这一轮**：N4 的空闲思考（默认关）自己从 `[自问:…]` 里挑一个
         * 没想通的问题来想。它**只读**（不提议意图、不沉淀、不动图，见 think.h 的
         * `think_idle`），所以回复一个字都不变（还是"……"）—— 变的是痕 / 思考对象。
         * 这一轮照旧**不进**下面的维护段：睡眠 / 巩固 / 衰减的账一点不动。 */
        /* A1：空闲思考**主动说话**（默认关）——把它刚想出来的那一步说出来，而不是只回"……"。
         * 判据：`think_idle` 跑完之后痕里**新增了步子**（有 why 的那条）。 */
        Think *th = think_of(ai);
        size_t trace_before = th ? th->trace_n : 0;          /* ⚠ trace 是数组，行数是 trace_n */
        (void)think_idle(ai);
        if (ai->idle_speak && th && th->trace_n > trace_before) {
            const char *say = NULL;
            for (size_t k = trace_before; k < th->trace_n; ++k)
                if (th->trace[k].why[0]) say = th->trace[k].why;
            if (say) {
                snprintf(out, out_cap, "（空闲时想到的）%s", say);
                ai_seg_lap(ai, AI_SEG_THINK, &seg_t);   /* 空轮：剩下的都算「思维」（think_idle）*/
                ai_free_words(words_alloc, n_alloc);
                return strlen(out);
            }
        }
        snprintf(out, out_cap, "……");
        ai_seg_lap(ai, AI_SEG_THINK, &seg_t);
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
     * 补丁把外部信号变成符号节点交进来，内核只当它们是一批符号节点
     * （放进焦点 + 挂到当前时刻，剩下的靠关联学习自己长）。
     * **没挂补丁时这里是空转** —— 内核不依赖任何补丁。 */
    micro_patch_sense(ai);

    ai_seg_lap(ai, AI_SEG_LEARN, &seg_t);
    /* 内部对话 + 想象 */
    ai_inner_dialogue(ai);
    ai_seg_lap(ai, AI_SEG_INNER, &seg_t);
    ai_imagine(ai);
    ai_seg_lap(ai, AI_SEG_IMAGINE, &seg_t);

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

    ai_seg_lap(ai, AI_SEG_GEN, &seg_t);
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
        cot_push(ai, COT_READ, 1, 100, "分词 + 符号节点代数", note);
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

    ai_seg_lap(ai, AI_SEG_THINK, &seg_t);
    /* ---- 关系层（L2）----
     * 顺序：先看是不是在**陈述事实**（那就学下来），再看是不是在**问关系**
     * （那就推理回答）。两条都在联想之后 —— 先想一遍，再动确定性推理。 */
    if (!resp) {
        float rc = 0.5f;
        resp = agent_set_hypothesis(ai, words, n, &rc);
        /* ② 触发接求解：闭合问句答不了时，试**开放式**关系问句（默认关） */
        if (!resp && ai->rel_open) resp = agent_open_relation(ai, words, n, &rc);
        /* ② 类比兜底（默认关）：规则**答不上来**时（不只是没答，而是答了 DER_VERDICT_UNSURE）
         * 换成"同类互推"的**大概**答案。判据用 `DER_VERDICT_UNSURE` 一处定义，别抄字符串。
         * ⚠ §6.124：这里的位置**在确定性推理之前**，于是它会**抢答** ——
         * 老顺序（默认）留着不动；开了 `ai->rel_first` 就挪到 `agent_answer_relation` 后面。 */
        if (!ai->rel_first) {
            if (resp && !strstr(resp, DER_VERDICT_UNSURE)) {
                /* 规则真答了（对/不对）⇒ 不动 */
            } else {
                char *an = agent_analogy_fallback(ai, words, n, &rc);
                if (an) { free(resp); resp = an; }
            }
        }
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
        int wh = 0;
        resp = agent_why_question(ai, words, n, &rc, &wh);
        if (resp) conf = rc;
    }
    if (!resp) {
        float rc = 0.5f;
        resp = agent_forget_request(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    if (!resp) {
        float rc = 0.5f;
        resp = agent_learn_multi(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    if (!resp) {
        float rc = 0.5f;
        int cq = 0;
        resp = agent_conj_both(ai, words, n, &cq, &rc);
        if (resp) conf = rc;
        if (!resp) resp = agent_conj_obj(ai, words, n, &cq, &rc);
        if (resp) conf = rc;
        if (!resp) resp = agent_or_ask(ai, words, n, &cq, &rc);
        if (resp) conf = rc;
    }
    if (!resp) {
        float rc = 0.5f;
        resp = agent_learn_relation(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    /* §6.218 地点块：`A V 在 B` / `A 在 B V` ⇒ 学 `A 在 B`（动词那半句如实说没记）*/
    if (!resp) {
        float rc = 0.5f;
        resp = agent_learn_locative(ai, words, n, &rc);
        if (resp) conf = rc;
    }
    /* §6.173 含**新词**的自然说法（默认关）：关系词是已知核心关系、主语/宾语是没见过的
     * 单字词（或池里已有的整词）⇒ 当场造词 + 学一条。放着不吃就整句掉降级答复（实测）。 */
    if (!resp) {
        float rc = 0.5f;
        resp = agent_learn_new_words(ai, words, n, &rc);
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

    /* §6.174 问句里含新词：关系层解析不了 ⇒ 至少**挂一条追问**（用户回话时再造词+学）。 */
    if (!resp && ai->askback) agent_ask_new_word_arm(ai, words, n);

    /* §6.124 **先链后联想**（默认关）：确定性推理（上面那条）答不上来 / 只答"不确定"时，
     * 才轮到"同类互推"的**大概**答案。这样"证明过的答案"不会被"猜的答案"抢掉 ——
     * 与「先有链、才有话」同一条规矩。关着时这段不跑，顺序与一直以来的样子一致。 */
    if (ai->rel_first && (!resp || strstr(resp, DER_VERDICT_UNSURE))) {
        float rc = conf;
        char *an = agent_analogy_fallback(ai, words, n, &rc);
        if (an) { free(resp); resp = an; conf = rc; }
    }

    ai_seg_lap(ai, AI_SEG_REL, &seg_t);
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
            /* §6.271 **问句只配问句**：用户这句是问句，而命中的那条例句**自己不是问句**
             * ⇒ 它答的不是这一类问题，是假阳性。实测（session.bin，8 句答不上来的问句）
             * 5 句被不相干的陈述句抢答：「猫喜欢吃什么」⇒「饿的时候就去吃点好的」。
             * 判据只看命中句的**词**（有没有疑问词），不看分数字段 —— 分不开的正是这一类。 */
            if (ok && is_question) {
                /* §6.271 **用户的内容词必须在那条例句里出现过**（第一道）。
                 * 实测「猫 喜欢 吃 什么」命中了语料的「你 喜欢 吃 什么」（4 词里 3 个一样），
                 * 于是把"你"的答案原样吐给"猫" —— 检索复述最典型的错法。
                 * 虚词和疑问词不比（它们不承载"问的是谁"）。 */
                if (ok) {
                    int miss = 0;
                    for (size_t i2 = 0; i2 < n && !miss; ++i2) {
                        const char *wn = words[i2];
                        if (!wn || !wn[0] || fr_is_func(wn) || ai_token_has_question_word(wn))
                            continue;
                        if (!toks[i2]) { miss = 1; break; }
                        int found = 0;
                        for (size_t k2 = 0; k2 < hit->in_n && !found; ++k2)
                            if (hit->in[k2] && hit->in[k2]->id == toks[i2]->id) found = 1;
                        if (!found) miss = 1;
                    }
                    if (miss) ok = 0;
                }
            }
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
    ai_seg_lap(ai, AI_SEG_LOOKUP, &seg_t);
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

    ai_seg_lap(ai, AI_SEG_TAIL, &seg_t);
    /* ---- 微内核安全点 ----
     * 一轮的**唯一**结构修改入口：补丁动作先做（它看的是刚说出去的话），
     * 然后自修改走「提议 → 安全内核审查 → 应用 + 记账 → 元认知」。
     * 放在这里是因为所有遍历早就结束了 —— 加边/加符号节点不会
     * 让任何正在进行的 `for (i < t->count)` 失效。 */
    /* ---- 对话触发：自然语言 → 符号节点行为（表驱动，窄集）----
     * 放在这里（学完、话也组好了之后）：命中就用它的答复**替掉**流水线那句，
     * 但**不跳过**学习 —— 触发是"入口"，不是"特权"，动作照样走受审通道。 */
    {
        char tobuf[256];
        /* ③ 他者视角（复数主语"大家/他们/别人"）先试：**默认关**，关着时这一行什么都不做 */
        if (dialog_try_proc_plan(ai, words, n, tobuf, sizeof tobuf) ||
            dialog_try_proc(ai, words, n, tobuf, sizeof tobuf) ||
            dialog_trigger_try_others(ai, words, n, tobuf, sizeof tobuf) ||
            dialog_trigger_try(ai, words, n, tobuf, sizeof tobuf)) {
            snprintf(out, out_cap, "%s", tobuf);
            cot_push(ai, COT_INFER, 1, 80, "对话触发（符号节点行为）", tobuf);
            ai->n_cot++;
        }
    }

    /* ---- ② 过程接进思维（默认关、只读）：这一步的问题是不是我以前记过的某个过程？----
     * 放在思维循环**之后**：这时候痕已经写完了，补一条"这我记过过程"谁都不会打断。 */
    think_note_process(ai, words, n);

    /* ---- 输出侧补丁（第一步）：暂存 + 动作补丁看符号节点 ----
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
        /* §6.256b 截断有界：`out_cap` 可能小于 `pout`（optest 用 600），
         * 直接 `%s` 会触发 -Wformat-truncation（探针也要 -Werror 过的）。 */
        if (pr == 1)       snprintf(out, out_cap, "%.*s",
                                    (int)(out_cap ? out_cap - 1 : 0), pout);
        else if (pr == -1) out[0] = 0;          /* 补丁明确说：这一轮不说话 */
    }

    /* §6.139 **脏思考检查器**：说出口的话连续重复词/句 ⇒ 当场截停（强制停止）。
     * 放在**所有输出决策之后**（含输出侧补丁）—— 这样哪条路说的都要过这一关。
     * 只截**说出去的话**：`resp` / 自举学到的东西照旧（它想的是那些，用户看到的是截过的）。 */
    (void)ai_dirty_apply(ai, out, out_cap, NULL);

    /* §6.216 记住**发出去的原文**（"再说一遍"要原样复述，含排版）*/
    if (out[0] && strcmp(out, "……") != 0) snprintf(ai->last_reply, sizeof ai->last_reply, "%s", out);

    ai_seg_lap(ai, AI_SEG_KERNEL, &seg_t);
    /* 维护 */
    ai->turns++;
    /* §6.237 三段各自计时（只在 AIP_SEGTIME=1 时读表） */
    uint64_t mnt_t = ai_seg_mark(ai);
    if (ai->consolidate_every && (ai->turns % ai->consolidate_every) == 0) {
        ai->n_scan_tokens += ai->pool->count;
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t->is_garbage) continue;
            token_consolidate(t);
            ai_write_count(ai, AI_WSRC_DECAY);
            token_decay_hits(t, ai->decay_factor);
        }
    }
    {   /* 巩固段收尾 */
        if (ai_seg_on()) { ai->mnt_ns[0] += ai_seg_now() - mnt_t; ai->mnt_hits[0]++; }
        mnt_t = ai_seg_mark(ai);
    }
    if (ai->gc.gc_interval && (ai->turns % ai->gc.gc_interval) == 0)
        ai_gc(ai);
    {   /* GC 段收尾 */
        if (ai_seg_on()) { ai->mnt_ns[1] += ai_seg_now() - mnt_t; ai->mnt_hits[1]++; }
        mnt_t = ai_seg_mark(ai);
    }
    if (ai->sleep_every && (ai->turns % ai->sleep_every) == 0)
        ai_sleep_begin(ai);
    if (ai_sleep_pending(ai)) ai_sleep_step(ai);
    if (ai_seg_on()) { ai->mnt_ns[2] += ai_seg_now() - mnt_t; ai->mnt_hits[2]++; }

    ai_seg_lap(ai, AI_SEG_MAINT, &seg_t);
    /* 漂移 / 诊断 / 叙事 */
    if (ai->drift_sample_every && (ai->turns % ai->drift_sample_every) == 0)
        ai_drift_sample(ai);
    if (ai->turns % 300 == 0)
        ai_diagnose(ai);
    if (ai->narrate_every && (ai->turns % ai->narrate_every) == 0)
        (void)ai_narrate_apply(ai);            /* §6.141：与 `::narrate now` 同一处实现 */

    ai_seg_lap(ai, AI_SEG_NARR, &seg_t);
    /* §6.255 记"这一轮的主语"，供下一句的指代用（代词/虚词不记）。
     * **必须放在最后**：`鲸鱼 会 游泳` 这种新词是 natcoin 在**后面**才造出来的，
     * 分词刚结束时 `pool_find(鲸鱼)` 还是 NULL ⇒ 记录会漏（实测：下一句「它」指不回去）。 */
    if (words && n >= 1 && words[0] && words[0][0] && input_eff == input) {
        static const char *skip0[] = {"我", "你", "您", "他", "她", "它", "我们", "你们",
                                      "他们", "这", "那", "这个", "那个", "什么", "谁",
                                      NULL};
        int skip = 0;
        for (size_t i = 0; skip0[i]; ++i) if (!strcmp(words[0], skip0[i])) { skip = 1; break; }
        if (!skip && pool_find(ai->pool, words[0]) && strlen(words[0]) < sizeof ai->last_subject)
            snprintf(ai->last_subject, sizeof ai->last_subject, "%s", words[0]);
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