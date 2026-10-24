/* ptrtag.h — **带标签的指针**（多智能体 / 局部加载的地基）
 *
 * 用户 2026-10-24 的目标：
 *   「打开多个智能体，每个智能体只维护自己的修改，并且可以暂时只加载一部分图 ——
 *     至于怎么实现，就是指针不是 8 字节对齐吗，不是 0 就是在磁盘或其他地方」
 *
 * 这句话的技术内容就是**低 3 位空闲**：malloc 返回的指针至少 8 字节对齐 ⇒ 低 3 位恒 0。
 * 我们把"这块数据现在在哪儿"塞进那 3 位：
 *
 *   tag = 0  RAM        —— 老路径，指针原样用（**默认值就是 0**，所以全部旧代码语义不变）
 *   tag = 1  DISK       —— 内容在磁盘上（某个 .part 文件 / 区段），用时才读
 *   tag = 2  ELSEWHERE  —— 在别处（别的智能体的池 / 父进程 / 压缩块……），**不是本进程的堆**
 *   tag = 3..7          保留（后续可以放 "共享只读" / "已释放待回收" 等）
 *
 * 三条纪律（写在代码里，不靠记性）：
 *   ① **只有 8 字节对齐的指针才能打标签**。非对齐指针打标签会丢数据 —— 所以 `aip_ptr_tagged`
 *      在 debug 下断言，正常路径由调用点保证（我们的对象都是 union/malloc 出来的）。
 *   ② **读指针前必须 strip**。`(Token*)p` 直接解引用会崩；一律走 `aip_ptr_strip`。
 *   ③ tag=0 必须与"没有标签"逐字节等价 —— 这是"先接上、后改语义"的前提：
 *      今天没有任何调用点依赖 tag≠0，所以这套东西接进来时**行为零变化**。
 */
#ifndef AIP_PTRTAG_H
#define AIP_PTRTAG_H

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#define AIP_PTRTAG_BITS   3u
#define AIP_PTRTAG_MASK   ((uintptr_t)7u)
#define AIP_PTRTAG_ALIGN  ((uintptr_t)8u)

/* 现在在哪儿 */
#define AIP_PTR_RAM        0u
#define AIP_PTR_DISK       1u
#define AIP_PTR_ELSEWHERE  2u
#define AIP_PTR_RESERVED   3u   /* 3..7 都算保留 */

static inline const char *aip_ptr_tag_name(unsigned tag) {
    switch (tag) {
    case AIP_PTR_RAM:       return "内存";
    case AIP_PTR_DISK:      return "磁盘";
    case AIP_PTR_ELSEWHERE: return "别处";
    default:                return "保留";
    }
}

/* 这个指针能不能打标签（8 字节对齐）？ */
static inline int aip_ptr_taggable(const void *p) {
    return (((uintptr_t)p) & AIP_PTRTAG_MASK) == 0;
}

/* 取标签。**注意**：打了标签的指针本身就不再"对齐"了，所以这里**不能**先查对齐 ——
 * 直接掩码。（"这个指针能不能打标签"问的是 `aip_ptr_taggable`，两件事别混。） */
static inline unsigned aip_ptr_tag(const void *p) {
    if (!p) return 0u;
    return (unsigned)(((uintptr_t)p) & AIP_PTRTAG_MASK);
}

/* 去掉标签，拿到真指针。 */
static inline void *aip_ptr_strip(const void *p) {
    if (!p) return NULL;
    return (void *)(((uintptr_t)p) & ~(uintptr_t)AIP_PTRTAG_MASK);
}

/* 打标签。非对齐指针**原样返回**（并断言）—— 绝不悄悄丢位。 */
static inline void *aip_ptr_tagged(void *p, unsigned tag) {
    assert(tag < (1u << AIP_PTRTAG_BITS));
    if (!p) return NULL;
    if (!aip_ptr_taggable(p)) {
        assert(0 && "aip_ptr_tagged: 指针不是 8 字节对齐，不能打标签");
        return p;
    }
    return (void *)(((uintptr_t)p) | (uintptr_t)tag);
}

/* tag==0 的等价性自查：任何"没打标签"的指针，打 0 标签后必须还是同一个值。
 * 这条是"接进来零行为变化"的机器检查（探针 tools/multiagent.c 用）。 */
static inline int aip_ptr_untagged_identity(const void *p) {
    return aip_ptr_strip(p) == p && aip_ptr_tag(p) == 0u;
}

#endif /* AIP_PTRTAG_H */
