/* ============================================================================
 * xbrain.h — **跨脑知识语义**（§6.580）
 *
 * 起因（用户 2026-10-24）：「跨脑知识语义你看看怎么办，我也拿不准」。
 * 上一轮（§6.574）的 `::parts journal/replay` 是**再学一遍**：把流水行拼成一句话
 * 喂回 `agent_step`。它有两个问题，都是实测出来的：
 *
 *   ① **不确定**：目标脑已有相反知识时，学习路径会**拒绝/撤回**（实测
 *      `蛇 属于 植物`：#1 收下、#0 拒绝）⇒ 同一份流水搬过去，结果取决于
 *      目标脑当时的状态。迁移要的是"带走 N 条就是 N 条"，不是"看它心情"。
 *   ② **看不见**：被拒绝的那些**没有任何痕迹**（只落到答复文本里），
 *      两个脑为什么不一致、哪里不一致，事后查不出来。
 *
 * 这一轮把语义定下来（一共三条，都能用探针钉住）：
 *
 *   **A. 照抄（默认）= 确定性迁移。** 不再走解析/学习，直接按名字把事实写进图
 *      （写还是走 `ai_write_rel` 那条统一通道，只是账记成"照抄"）。
 *      关系的名字写成**谓词名**（不是关系 id）：目标脑的动态谓语表是它自己的，
 *      id 跨脑不可比（同一个"想念"在两个脑里可能是不同的 id）。
 *
 *   **B. 本地优先 + 冲突可见。** 外来事实**不静默覆盖**本地知识。逐条判四种：
 *        · 已知  —— 本地已经有同一条（同极性）：不动；
 *        · 冲突  —— 本地有相反极性，或独占关系已有别的值：**默认不写**，
 *                  记进账（`AIMOD_REL_CONFLICT`），并在命令输出里列出来；
 *        · 新增  —— 本地没有：写（`AIMOD_REL_COPY`）；
 *        · 坏行  —— 关系名认不出 / 行格式坏了：不写，计数。
 *      理由：这个项目的边界是"每个智能体只维护自己的修改"。用户亲口教的
 *      才是本地的权威（那是"改口"，有明确语义）；`replay` 只是**搬数据**，
 *      搬数据不该替人做"谁对"的决定。冲突摆到台面上，由人决定。
 *
 *   **C. `force` = 明知冲突还是写，但**两种情况分开说**：**
 *        · 极性冲突（同一对词、同一关系，本地是相反极性）：写下去就行 ——
 *          学习路径本来就是"后落的观测替掉对立极性"（`token_tag_*` 的改口语义），
 *          所以 `force` 在这里是**真覆盖**，不留下两条互斥的边；
 *        · 独占冲突（本地有别的值，典型是 `属于` 的上下位/时间变化）：**并列写入、
 *          不撤回旧值**。为什么不撤回：`属于` 有嵌套（`猫 属于 猫科`、`猫科 属于 动物`
 *          可以同时为真），照搬 `在` 的那套"新地点替掉旧地点"会删掉真知识
 *          （agent.h 里那条注释已经写明这个坑）⇒ 宁可让两条并存，让 `::brain diff`
 *          把它们露出来，也不替用户删知识。
 *
 * 还没做（照实说）：
 *   · 只搬**关系事实**（`R` 行）。共现边（听到的）不搬 —— 那是"经历"，不是"知识"；
 *   · 目标脑若把新词 intern 进来，它只有名字、没有类别/共现，**不是**一次完整学习；
 *   · 冲突判定是**本地视角的单跳**（`rel_direct` / 独占见证），不展开整条推理链 ——
 *     所以"冲突"只保证"直接写下去会打架"，不保证"逻辑上一定互斥"。
 * ==========================================================================*/
#ifndef XBRAIN_H
#define XBRAIN_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* 一次照抄的账。顺序就是输出顺序（新增 / 已知 / 冲突 / 覆盖 / 坏行 / 新建词）。 */
typedef struct {
    size_t added, known, conflicts, overridden, bad, newwords;
} XbReport;

typedef enum {
    XB_MODE_LOCAL_WINS = 0,   /* 默认：本地优先，冲突不写 */
    XB_MODE_FORCE      = 1,   /* 明知冲突还是写 */
    XB_MODE_CHECK      = 2    /* 只查不写（干跑：告诉你会发生什么） */
} XbMode;

#define XB_MAX_CONFLICT_LINES 6
typedef struct {
    XbReport r;
    size_t   n_lines;
    char     lines[XB_MAX_CONFLICT_LINES][176];
} XbResult;

/* ---- 事实"本地是什么样" ----
 * 返回 1 = 本地有同一条（同极性）、-1 = 本地有**相反极性**、0 = 本地没有。
 * 动态谓语（§6.80：否定旗子在 `nrel` 第 15 位）单独走一遍边扫描 ——
 * `rel_direct_neg` 对动态关系一律返回 0（"否定只支持核心关系"），
 * 照抄不能跟着这个口径看不见动态否定。 */
static inline int xb_pair_state(const Token *A, uint8_t rid, const Token *B, int neg) {
    if (!A || !B || rid == 0) return 0;
    if (rid >= REL_DYN_BASE) {
        uint8_t d = (uint8_t)(rid - REL_DYN_BASE + 1);
        for (size_t i = 0; i < A->count; ++i) {
            const AssocEntry *e = &token_assocs_ro(A)[i];
            if (e->main != B || e->dyn != d) continue;
            int e_neg = edge_dyn_neg(e) ? 1 : 0;
            return (e_neg == (neg ? 1 : 0)) ? 1 : -1;
        }
        return 0;
    }
    if (neg ? rel_direct_neg(A, rid, B) : rel_direct(A, rid, B)) return 1;
    if (neg ? rel_direct(A, rid, B) : rel_direct_neg(A, rid, B)) return -1;
    return 0;
}

/* 独占关系里"本地已有的那个别的值"（没有就 NULL）。
 * 只对肯定事实问：`A 不 R C` 与 `A R B` 不一定打架（否定常常是说"它不属于那个更细的类"）。*/
static inline Token *xb_excl_other(AI *ai, const Token *A, uint8_t rid, const Token *B, int neg) {
    if (!ai || !A || neg) return NULL;
    if (rid >= REL_DYN_BASE) return NULL;          /* 动态谓语不参与独占判定（表里没声明）*/
    if (!rel_is_excl(ai, rid)) return NULL;
    if (B) return rel_excl_witness(ai, A, rid, B, 0);
    /* 宾语在本地**还没有这个词** ⇒ 它一定不是"同一个值"（有边才有词）。
     * 这时不能直接判"没冲突"：实测「桌子 属于 家具」在，外来的是「桌子 属于 玩具」
     * （玩具这个词本地没有）—— 先建词再判的话会留下一个空词，先判的话得自己找已有的值。 */
    uint16_t bit = REL_BIT(rid);
    for (size_t i = 0; i < A->count; ++i) {
        const AssocEntry *e = &token_assocs_ro(A)[i];
        if (!e->main || e->main == A) continue;
        if (!(e->rel & bit)) continue;
        if (e->role == ASSOC_ROLE_DERIVED || edge_is_guess(e)) continue;   /* 缓存不算知识 */
        return e->main;
    }
    return NULL;
}

/* 写一行冲突说明（最多 6 行，之后只计数 —— 输出不能刷屏） */
static inline void xb_note_line(XbResult *res, const char *fmt, ...) {
    if (!res || res->n_lines >= XB_MAX_CONFLICT_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->lines[res->n_lines], sizeof res->lines[res->n_lines], fmt, ap);
    va_end(ap);
    res->n_lines++;
}

/* ---- 流水"存"：只记**自己改过**的词（`edges_dirty`）的关系边，按**名字**写 ----
 * 格式 `R\t主语\t谓词\t宾语\t极性`（极性 1 肯定 / 0 否定）。
 * 不记共现边：那是"听到的"，不是"知识"；也不记 `[` 开头的内部节点。 */
static inline int xb_journal_save(AI *ai, const char *path) {
    if (!ai || !ai->pool || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        Token *t = ai->pool->tokens[i];
        if (!t || !t->name || t->name[0] == '[' || t->count == 0) continue;
        if (!t->edges_dirty) continue;             /* 只记**自己改过的**词 */
        for (size_t k = 0; k < t->count; ++k) {
            const AssocEntry *e = &token_assocs_ro(t)[k];
            if (!e->main || !e->main->name || e->main->name[0] == '[') continue;
            /* 动态谓语（§6.80）：按名字记，不记 id —— 跨脑的 id 不可比 */
            if (e->dyn) {
                uint8_t drid = (uint8_t)(REL_DYN_BASE + e->dyn - 1);
                const char *dn = rel_dyn_name(ai, drid);
                if (dn) {
                    fprintf(f, "R\t%s\t%s\t%s\t%d\n", t->name, dn, e->main->name,
                            edge_dyn_neg(e) ? 0 : 1);
                    n++;
                }
                continue;
            }
            if (!e->rel && !e->nrel) continue;     /* 只装关系边（共现是"听到的"）*/
            for (uint8_t r = 1; r <= REL_NEED; ++r) {
                const char *rn = rel_name_of(ai, r);
                if (!rn) continue;
                if (e->rel & REL_BIT(r))  { fprintf(f, "R\t%s\t%s\t%s\t1\n", t->name, rn, e->main->name); n++; }
                if (e->nrel & REL_BIT(r)) { fprintf(f, "R\t%s\t%s\t%s\t0\n", t->name, rn, e->main->name); n++; }
            }
        }
    }
    fclose(f);
    return n;
}

/* 目标脑里按名字找词（找不到就 NULL；`make` 才 intern）。
 * 为什么不无条件 intern：`check` 模式是**干跑**，不能改图。
 * 注意 intern 出来的词只有名字 —— 它没有类别、没有共现，不是"完整学会了一个词"，
 * 但至少"这条事实"能落地（词级 delta 带不走新词，这条流水能带走）。 */
static inline Token *xb_word(AI *ai, const char *name, int make, int *made) {
    Token *t = pool_find(ai->pool, name);
    if (t || !make) return t;
    t = pool_token_intern(ai->pool, name);
    if (t && made) (*made)++;
    return t;
}

/* ---- 流水"照抄" ----
 * 逐行判定 + 落地。`res` 可以为 NULL（只要计数）。返回值：落地的条数（含覆盖）；
 * 打开失败返回 -1。 */
static inline int xb_journal_replay(AI *ai, const char *path, XbMode mode, XbResult *res) {
    if (res) memset(res, 0, sizeof *res);
    if (!ai || !ai->pool || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    XbReport r;
    memset(&r, 0, sizeof r);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (L < 4 || line[0] != 'R' || line[1] != '\t') continue;
        char *s1 = strchr(line, '\t'); if (!s1) { r.bad++; continue; }
        char *s2 = strchr(s1 + 1, '\t'); if (!s2) { r.bad++; continue; }
        char *s3 = strchr(s2 + 1, '\t'); if (!s3) { r.bad++; continue; }
        /* 第 **4** 个 tab 也不能漏：漏了 `bn` 会一路吃到行尾，
         * 实测变成 `家具\t1` 这样的词名（§6.580 抓到的第二个老 bug）。 */
        char *s4 = strchr(s3 + 1, '\t'); if (!s4) { r.bad++; continue; }
        *s1 = 0; *s2 = 0; *s3 = 0; *s4 = 0;
        /* 字段偏移（§6.580 抓到的老 bug）：`s1` 是 R 后面那个 tab、
         * `s2` 是主语后面那个、`s3` 是谓词后面那个 ⇒ 三段分别在 s1+1(截到 s2)、
         * **s2+1**（截到 s3）、**s3+1**（截到行尾）。§6.574 的旧代码写成了
         * `rn = s1+1, bn = s2+1` —— 于是「主语 主语 谓词」被当成一句话，
         * 解析必然失败，但旧代码**无条件** `applied++` ⇒ 报了"照着改了 N 条"
         * 而实际上一条都没改（静默空转）。这一轮把偏移改对，
         * 顺便把"落地条数"改成**真的写成功才计数**。 */
        const char *an = line + 2, *rn = s2 + 1, *bn = s3 + 1;
        int pol = atoi(s4 + 1);
        int neg = pol ? 0 : 1;
        /* 空字段不是事实：`pool_token_intern("")` 会真造一个空名字的词（比丢一行更糟）*/
        if (!*an || !*rn || !*bn) { r.bad++; continue; }
        /* 谓词名 → 目标脑自己的关系 id。`allow_new=1`：要带走的事实里可能有
         * 目标脑还没登记过的动态谓语（比如「想念」），不登记就等于丢知识。
         * `check` 模式不登记（干跑不许改图）⇒ 认不出就算坏行。 */
        uint8_t rid = rel_id_of(ai, rn, mode == XB_MODE_CHECK ? 0 : 1);
        if (!rid) { r.bad++; continue; }
        /* **先查后建**：冲突判定要两端都在本地才判得出来；不在的词**不提前建** ——
         * 否则"冲突没写"却留下一个没有边的空词（实测过：冲突 1 条、新建词 1 个）。
         * 干跑（check）同理：只数"会新建几个"，一个也不建。 */
        Token *A = pool_find(ai->pool, an);
        Token *B = pool_find(ai->pool, bn);
        int st = 0;
        Token *other = NULL;
        if (A && B) st = xb_pair_state(A, rid, B, neg);
        if (A && st == 0) other = xb_excl_other(ai, A, rid, B, neg);
        if (st == 1) { r.known++; continue; }
        if (st == -1) {
            if (mode != XB_MODE_FORCE) {
                r.conflicts++;
                if (res) xb_note_line(res, "%s %s%s %s —— 本地记的是：%s %s%s %s", an,
                                      neg ? "不 " : "",
                                      rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn, bn,
                                      an, neg ? "" : "不 ",
                                      rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn, bn);
                if (mode != XB_MODE_CHECK)
                    ai_modlog_note_raw(ai, A->id, B ? B->id : 0, rid, neg,
                                       AI_WSRC_REPLAY, AIMOD_REL_CONFLICT);
                continue;
            }
            /* force：真覆盖（改口语义：后落的观测替掉对立极性） */
            if (ai_write_rel_k(ai, A, B, rid, neg, AI_WSRC_REPLAY, AIMOD_REL_OVERRIDE) != 0) {
                r.bad++;
                continue;
            }
            r.overridden++;
            if (res) xb_note_line(res, "%s %s%s %s —— 覆盖本地「%s %s%s %s」", an,
                                  neg ? "不 " : "",
                                  rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn, bn,
                                  an, neg ? "" : "不 ",
                                  rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn, bn);
            continue;
        }
        /* st == 0：本地没有这条。独占关系还要看"有没有别的值" */
        if (other) {
            if (mode != XB_MODE_FORCE) {
                r.conflicts++;
                if (res) xb_note_line(res, "%s %s %s —— 本地已有别的值：%s %s %s", an,
                                      rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn, bn,
                                      an, rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn,
                                      other->name ? other->name : "?");
                if (mode != XB_MODE_CHECK)
                    ai_modlog_note_raw(ai, A->id, B ? B->id : 0, rid, neg,
                                       AI_WSRC_REPLAY, AIMOD_REL_CONFLICT);
                continue;
            }
            /* force + 独占冲突：**并列写入、不撤回**（见文件头 C 段：属于有嵌套，
             * 照搬「在」的替换语义会删掉真知识）。
             * 宾语本地没有词（上面那种情况）时，写之前才建 —— 和下面新增那条同一个口径。 */
            if (!B) {
                int made = 0;
                B = xb_word(ai, bn, 1, &made);
                r.newwords += (size_t)made;
            }
            if (!A || !B) { r.bad++; continue; }
            if (ai_write_rel_k(ai, A, B, rid, neg, AI_WSRC_REPLAY, AIMOD_REL_OVERRIDE) != 0) {
                r.bad++;
                continue;
            }
            r.overridden++;
            if (res) xb_note_line(res, "%s %s %s —— 与本地「%s %s %s」**并列**（旧值没撤）", an,
                                  rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn, bn,
                                  an, rel_name_of(ai, rid) ? rel_name_of(ai, rid) : rn,
                                  other->name ? other->name : "?");
            continue;
        }
        if (mode == XB_MODE_CHECK) {
            if (!A) r.newwords++;
            if (!B) r.newwords++;
            r.added++;
            continue;
        }
        {   /* 要写了才建词（照抄也能带走新词 —— 词级携带）*/
            int made = 0;
            A = xb_word(ai, an, 1, &made);
            B = xb_word(ai, bn, 1, &made);
            r.newwords += (size_t)made;
            if (!A || !B) { r.bad++; continue; }
        }
        if (ai_write_rel_k(ai, A, B, rid, neg, AI_WSRC_REPLAY, AIMOD_REL_COPY) != 0) { r.bad++; continue; }
        r.added++;
    }
    fclose(f);
    /* AI 上的累计账（诊断用；`::stats`/`::mods` 之外还能被探针读到） */
    ai->n_xcopy_added += r.added;
    ai->n_xcopy_known += r.known;
    ai->n_xcopy_conflict += r.conflicts;
    ai->n_xcopy_override += r.overridden;
    ai->n_xcopy_bad += r.bad;
    ai->n_xcopy_newwords += r.newwords;
    if (res) res->r = r;
    return (int)(r.added + r.overridden);
}

#endif /* XBRAIN_H */
