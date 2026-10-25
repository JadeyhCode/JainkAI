/* ============================================================================
 * conc.h — **并发地基**（§6.582）：线程局部暂存 + 原子自旋锁
 *
 * 用户 2026-10-24：「解决并发难题」。这个文件只放两件最小、最标准的东西，
 * 不引线程库（C11 的 `<stdatomic.h>` / `_Thread_local` 就够），
 * 所以**默认构建的命令行一个字节都不用改**，单线程路径行为完全不变。
 *
 * 一、`AIP_TLS`：把"每次调用都会被写的暂存"变成**每线程一份**。
 *     这些变量原来的语义就是"这一次问答的草稿"（`g_pre_rhet` 是剥壳前的句子、
 *     `g_charq` 是字符 bigram 备忘、`g_qidf_*` 是打分用的临时表、`g_shell_*` 是
 *     每个池的壳缓存……）。放进线程局部之后：
 *       · 单线程：和以前**逐字节一样**（同一个线程就一份）；
 *       · 多线程：各写各的，不再互相踩。
 *     ⚠ **不能**拿来放"加载期写一次、之后只读"的数据表（`[say]`/`[shape]`/`[reply]`
 *     那几桶）—— 那些是进程级只读，线程里必须看见同一份。分类见 `docs/concurrency.md`。
 *
 * 二、`AipLock`：标准 C11 `atomic_flag` 自旋锁，只给**共享可变结构**用
 *     （目前就一处：`share.h` 的引用计数表）。为什么不直接用 `pthread_mutex_t`：
 *     那会把"要不要链 pthread"塞进默认构建；自旋锁 20 行、无需链接、行为可测。
 *     临界区都很短（几次内存读写），自旋是合适的。
 *
 * 三、可重入的分词：`aip_split_sp` —— `strtok` 把游标存在**全局**里，
 *     两个线程同时用会互相踩（TSan 实测：一个线程读到另一个线程的栈）。
 * ==========================================================================*/
#ifndef CONC_H
#define CONC_H

#include <stddef.h>
#include <stdint.h>

/* ---- 线程局部存储 ----
 * C11 的 `_Thread_local`；老编译器（没有 C11）就退化成"普通全局"，
 * 那等于**回到单线程语义**（不静默改变行为，只是没有并发隔离）。 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(AIP_NO_TLS)
#  define AIP_TLS _Thread_local
#  define AIP_HAS_TLS 1
#else
#  define AIP_TLS
#  define AIP_HAS_TLS 0
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  include <stdatomic.h>
#  define AIP_HAS_ATOMICS 1
#else
#  define AIP_HAS_ATOMICS 0
#endif

/* ---- 自旋锁（C11 原子） ----
 * 用法：`AIP_LOCK_DECL(lk);` 声明，`aip_lock(&lk)` / `aip_unlock(&lk)` 包住临界区。
 * 单线程下这也只是两条原子指令，**不会**改变任何结果。 */
#if AIP_HAS_ATOMICS
typedef struct { atomic_flag f; } AipLock;
#define AIP_LOCK_INIT(field) { ATOMIC_FLAG_INIT }
#define AIP_LOCK_DECL(name) static AipLock name = AIP_LOCK_INIT(f)

/* 争用次数（诊断用：`::stats`/探针读它，证明锁真的被用上了、而不是"加了锁没人走"）*/
static _Atomic uint64_t g_aip_lock_wait = 0;

static inline void aip_lock(AipLock *l) {
    int spins = 0;
    while (atomic_flag_test_and_set_explicit(&l->f, memory_order_acquire)) {
        if (spins++ == 0) (void)atomic_fetch_add_explicit(&g_aip_lock_wait, 1u, memory_order_relaxed);
    }
}
static inline void aip_unlock(AipLock *l) {
    atomic_flag_clear_explicit(&l->f, memory_order_release);
}
static inline unsigned long long aip_lock_wait_count(void) {
    return (unsigned long long)atomic_load_explicit(&g_aip_lock_wait, memory_order_relaxed);
}
#else
typedef struct { int f; } AipLock;
#define AIP_LOCK_INIT(field) { 0 }
#define AIP_LOCK_DECL(name) static AipLock name = AIP_LOCK_INIT(f)
static inline void aip_lock(AipLock *l) { (void)l; }
static inline void aip_unlock(AipLock *l) { (void)l; }
static inline unsigned long long aip_lock_wait_count(void) { return 0; }
#endif

/* ---- 原子的"标量/指针"小包装 ----
 * 为什么要有它：有几处是"**一次写入、所有人读**"的东西（懒加载标志、函数指针钩子、
 * 环境变量缓存、机制表长度），既不适合 TLS（线程之间要看见同一个值），
 * 也不值得为它们各写一遍 `_Atomic`/回退两套代码。 */
#if AIP_HAS_ATOMICS
#  define AIP_ATOMIC_INT(name, initv)  static _Atomic int    name = (initv)
#  define AIP_ATOMIC_SIZE(name, initv) static _Atomic size_t name = (initv)
#  define AIP_ATOMIC_PTR(type, name, initv) static _Atomic(type) name = (initv)
#  define AIP_LD(x)      atomic_load(&(x))
#  define AIP_ST(x, v)   atomic_store(&(x), (v))
#else
#  define AIP_ATOMIC_INT(name, initv)  static int    name = (initv)
#  define AIP_ATOMIC_SIZE(name, initv) static size_t name = (initv)
#  define AIP_ATOMIC_PTR(type, name, initv) static type name = (initv)
#  define AIP_LD(x)      (x)
#  define AIP_ST(x, v)   ((x) = (v))
#endif

/* ---- 按空格切词（可重入，替代 `strtok`）----
 * 就地改 '\0'，把每段的起点写进 `out[]`，返回段数（最多 cap 段）。
 * 和 `strtok(s, " ")` 的差别：连续空格会被跳过（`strtok` 也是），返回段数而不是靠 NULL 游标。 */
static inline size_t aip_split_sp(char *s, char **out, size_t cap) {
    size_t n = 0;
    if (!s || !out || cap == 0) return 0;
    for (;;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (n >= cap) break;
        out[n++] = s;
        while (*s && *s != ' ') ++s;
        if (!*s) break;
        *s++ = 0;
    }
    return n;
}

#endif /* CONC_H */
