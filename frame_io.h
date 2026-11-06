/* frame_io.h — 把结构化帧库挂到 AI 上（读帧文件 / 释放 / 生成入口）
 * 依赖 ai.h（AI 类型）与 frame.h（FrameSet 类型）。
 */
#ifndef FRAME_IO_H
#define FRAME_IO_H

#include "ai.h"
#include "frame.h"

static inline void frames_free(AI *ai);   /* 前置：下面 frames_load 里要把它挂给 ai_free */

/* §6.769 **从图里装载帧**（`帧库 --[帧]--> 帧:X`、`帧:X --[问法/答法/填充]--> …`）。
 * 与 `frames.txt` 的装载结果**逐条等价**（`tests/framesink.sh` 证明过 226 条全等），
 * 所以它可以替代文件装载：帧从此能随模型走、能审计，不依赖 cwd 下的 txt。
 * ⚠ 模板里用 `_` 代替空格（图的行不能有空格），这里换成空格再走 `fr_split`。 */
static inline size_t fr_from_graph(AI *ai, FrameSet *fs) {
    if (!ai || !ai->pool || !fs) return 0;
    Token *lib = pool_find(ai->pool, "帧库");
    if (!lib) return 0;
    size_t n_add = 0;
    for (size_t j = 0; j < lib->count && fs->n_fr < FR_MAX_FRAMES; ++j) {
        const AssocEntry *e = &token_assocs_ro(lib)[j];
        if (!e->main || !e->main->name) continue;
        const char *nm = e->main->name;
        if (strncmp(nm, "帧:", (size_t)strlen("帧:")) == 0) nm += strlen("帧:");
        if (!*nm) continue;
        Frame *fe = &fs->fr[fs->n_fr];
        memset(fe, 0, sizeof *fe);
        fe->name = strdup(nm);
        fs->n_fr++;
        n_add++;
        Token *fn = e->main;
        int has_fill = 0;
        for (size_t q = 0; q < fn->count; ++q) {
            const AssocEntry *ae = &token_assocs_ro(fn)[q];
            if (!ae->dyn || !ae->main || !ae->main->name) continue;
            const char *dn = rel_dyn_name(ai, (uint8_t)(REL_DYN_BASE + ae->dyn - 1));
            if (!dn) continue;
            if (!strcmp(dn, "问法") || !strcmp(dn, "答法")) {
                FrTemplates *T = !strcmp(dn, "答法") ? &fe->ans : &fe->ask;
                if (T->n_tmpl >= FR_MAX_TMPL) continue;
                char buf[256];
                size_t bl = strlen(ae->main->name);
                if (bl >= sizeof buf) continue;
                for (size_t k = 0; k < bl; ++k) buf[k] = (ae->main->name[k] == '_') ? ' ' : ae->main->name[k];
                buf[bl] = 0;
                const char *slot_names[FR_MAX_SLOT];
                size_t n_slot = fe->n_slot;
                for (size_t i = 0; i < fe->n_slot; ++i) slot_names[i] = fe->slot[i].name;
                Token *toks[FR_MAX_TOK];
                int slot_at[FR_MAX_TOK];
                size_t tn = 0;
                fr_split(ai, buf, toks, slot_at, FR_MAX_TOK, slot_names, &n_slot, &tn);
                for (size_t i = fe->n_slot; i < n_slot; ++i) { fr_slot(fe, slot_names[i], 1); free((void *)slot_names[i]); }
                for (size_t i = 0; i < tn; ++i) {
                    T->toks[T->n_tmpl][i] = toks[i];
                    T->slot_at[T->n_tmpl][i] = (int8_t)slot_at[i];
                }
                T->n_toks[T->n_tmpl] = tn;
                T->n_tmpl++;
            } else if (!strcmp(dn, "填充")) {
                has_fill = 1;
                if (fe->n_slot == 0) fr_slot(fe, "X", 1);
                FrSlot *sl = &fe->slot[0];
                char buf[512];
                size_t bl = strlen(ae->main->name);
                if (bl >= sizeof buf) continue;
                memcpy(buf, ae->main->name, bl + 1);
                char *tok = strtok(buf, ",");
                while (tok && sl->n_fill < FR_MAX_FILL) {
                    Token *t = pool_token_intern(ai->pool, tok);   /* 填充词也进图 */
                    if (t) sl->fill[sl->n_fill++] = t;
                    tok = strtok(NULL, ",");
                }
            }
        }
        (void)has_fill;
    }
    return n_add;
}

/* 读帧文件并挂上。返回解析到的帧数（0 表示失败/空）。 */
static inline size_t frames_load(AI *ai, const char *path) {
    /* §6.191 挂上释放钩子：`ai_free()` 在 ai.h 里（比这个文件早），它通过函数指针
     * 调这里的 `frames_free` —— 否则每 `::load` 一次就泄一整个 FrameSet（≈900 KB）。 */
    AIP_ST(ai_frames_free, frames_free);   /* §6.582 原子写（并发建脑时不再竞争）*/
    if (!ai || !path) return 0;
    FrameSet *fs = (FrameSet *)ai->frames;
    if (!fs) {
        fs = (FrameSet *)calloc(1, sizeof *fs);
        if (!fs) return 0;
        ai->frames = fs;
    }
    /* §6.769 `AIP_FRAMES_FROM_GRAPH=1` 时**从图装载**（图里没有帧就退回文件，不让启动变哑） */
    if (getenv("AIP_FRAMES_FROM_GRAPH")) {
        size_t g = fr_from_graph(ai, fs);
        if (g > 0) return g;
    }
    return fr_load(ai, fs, path);
}

static inline void frames_free(AI *ai) {
    if (!ai || !ai->frames) return;
    FrameSet *fs = (FrameSet *)ai->frames;
    for (size_t i = 0; i < fs->n_fr; ++i) {
        Frame *fr = &fs->fr[i];
        free((void *)fr->name);
        for (size_t k = 0; k < fr->n_slot; ++k) free((void *)fr->slot[k].name);
    }
    free(fs);
    ai->frames = NULL;
}

/* 用结构化帧生成一句新话；没有帧库或匹配不上返回 NULL。 */
/* tr 可以是 NULL（不关心过程）。给了就填上「哪一帧、卡在哪、用没用降级」，
 * 交给思维链去记 —— 帧层只报告发生了什么，不解释含义。 */
static inline char *frames_generate(AI *ai, Token *const *words, size_t n,
                                    FrTrace *tr) {
    if (tr) memset(tr, 0, sizeof *tr);
    if (!ai || !ai->frames) return NULL;
    return fr_generate(ai, (FrameSet *)ai->frames, words, n, tr);
}

#endif /* FRAME_IO_H */
