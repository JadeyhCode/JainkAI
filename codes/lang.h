/* lang.h — **语言包**（阶段二 WP6；§6.639）
 *
 * 目标（用户）：**现场学一门新的语言**。这一版只做最小可验证的一层：
 *
 *   · 语言包 = 数据：`lang/<code>/words.txt`（`外语=中文` 一行一对）+ `meta.txt`（`order=SVO|SOV`）；
 *   · `::lang load <code>` 把词表读进内存（**全局**，和 patterns.txt 同层），`::lang current/list` 看状态；
 *   · 装了语言包之后，**输入先逐词翻译 + 按词序重排**，再走原来的中文流水线
 *     —— 于是"外语提问"能落到已有的 `A 关系 B` 语义上；
 *   · 没装语言包/词不认识 ⇒ **照实说**（不猜、不硬翻）。
 *
 * 明确不做（写死）：
 *   1. 不做语法分析/形态（时态、复数、冠词）—— 只按空格切词、逐词替换；
 *   2. 不做"从零归纳"（少样本归纳是下一步，见 `::lang induce` 的占位说明）；
 *   3. 不是翻译器：翻不出来的词**原样留着**，整句交给中文流水线时自然答"不太懂"。
 */
#ifndef LANG_H
#define LANG_H

#include "agent.h"
#include "lang_core.h"

/* 表与翻译在 lang_core.h（agent.h 也要用，避免循环 include） */

/* §6.640 归纳时的"关系词"判据：交给调用方（main.c）用 AI 的关系表判。
 * 这里只声明一个 typedef 用法示例，实现在 main.c 里作为回调传进 lang_induce()。 */
#endif /* LANG_H */
