/* ============================================================================
 * saveio.h — **跨进程安全的落盘**（§6.583）：唯一临时名 + 落盘 + 原子替换
 *
 * 为什么需要（并发阶段剩下的那一半）：
 *   §6.582 解决的是**进程内**并发；但两个 `aip` 进程同时写同一份 `model.bin` /
 *   `*.parts` / 流水，照样会互相踩。§6.88 那次已经给 `ai_save_file` 加了
 *   "写临时文件 → fsync → rename"，**可临时文件名是固定的 `path.tmp`**：
 *   两个进程同时写 `model.bin` 就是同时写**同一个** `model.bin.tmp` ——
 *   各自持有文件偏移、互相截断/覆盖，最后 rename 出来的是一份**混着两边的坏文件**。
 *   模型文件就是它的全部记忆（写坏 = 记忆清零），所以这一条必须修：
 *
 *     **临时名带上 pid + 序号** ⇒ 每个写者写自己的临时文件，
 *     rename 在同一个文件系统上是原子的 ⇒ 读的人永远看到"旧的完整"或"新的完整"。
 *     并发写同一路径的语义 = **最后一个 rename 的人赢**（不排序、不合并，也没有中间态）。
 *
 * 用法（三段）：
 *     char tmp[1024];
 *     if (aip_tmp_name(path, tmp, sizeof tmp) != 0) return -1;
 *     FILE *f = fopen(tmp, "wb");
 *     ... 写 ...
 *     if (aip_sync_close(f) != 0) { remove(tmp); return -1; }   // 先落盘
 *     if (aip_commit_rename(tmp, path) != 0) { remove(tmp); return -1; }
 *
 * 边界（照实说）：
 *   · 只保证"文件不会坏"，**不保证"并发写的顺序/合并"**（没有文件锁，也没有版本号）；
 *   · 需要多写者读到彼此的结果，得自己加锁（`flock`），这一轮没做；
 *   · 临时文件在**同一目录**（跨目录 rename 不是原子的）；
 *   · 依赖 POSIX（`getpid`/`fsync`/`rename`）；Windows 上 `rename` 覆盖已存在文件会失败，
 *     这里没有做兼容（项目本来只在 Linux 上跑）。
 * ==========================================================================*/
#ifndef SAVEIO_H
#define SAVEIO_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>     /* getpid / fsync / fileno */
#include "conc.h"       /* AIP_* 原子小工具 */

/* 生成"同目录 + 唯一"的临时名：`<path>.tmp.<pid>.<序号>`。
 * 返回 0 = 成功；-1 = 名字太长（不做截断：截断会让两个不同路径撞到同一个临时文件）。 */
static inline int aip_tmp_name(const char *path, char *out, size_t cap) {
    static _Atomic unsigned long seq = 0;
#if AIP_HAS_ATOMICS
    unsigned long n = atomic_fetch_add_explicit(&seq, 1ul, memory_order_relaxed);
#else
    unsigned long n = seq++;
#endif
    long pid = (long)getpid();
    int w = snprintf(out, cap, "%s.tmp.%ld.%lu", path, pid, n);
    if (w < 0 || (size_t)w >= cap) return -1;
    return 0;
}

/* 落盘 + 关闭。返回 0 = 成功（写盘错误、fsync 失败、close 失败都算失败）。 */
static inline int aip_sync_close(FILE *f) {
    if (!f) return -1;
    int bad = 0;
    if (fflush(f) != 0) bad = 1;
    if (!bad && fsync(fileno(f)) != 0) bad = 1;
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}

/* 原子替换：失败时**不动**目标文件（旧的那份还在）。 */
static inline int aip_commit_rename(const char *tmp, const char *path) {
    if (!tmp || !path) return -1;
    return rename(tmp, path) == 0 ? 0 : -1;
}

#endif /* SAVEIO_H */
