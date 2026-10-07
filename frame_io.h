/* frame_io.h — 把结构化帧库挂到 AI 上（读帧文件 / 释放 / 生成入口）
 * 依赖 ai.h（AI 类型）与 frame.h（FrameSet 类型）。
 */
#ifndef FRAME_IO_H
#define FRAME_IO_H

#include "ai.h"
#include "frame.h"

/* 读帧文件并挂上。返回解析到的帧数（0 表示失败/空）。 */
static inline size_t frames_load(AI *ai, const char *path) {
    if (!ai || !path) return 0;
    FrameSet *fs = (FrameSet *)ai->frames;
    if (!fs) {
        fs = (FrameSet *)calloc(1, sizeof *fs);
        if (!fs) return 0;
        ai->frames = fs;
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
