/* tools/partbench.c — **真实图上量局部加载**（C 级）
 *
 * §6.566 只用一个小图（8 段）证明了"能只加载一部分"。这一条拿**出厂模型**（4 万条边）量：
 *   ① 把所有边写成分段文件，报段数与文件大小；
 *   ② 卸下（内存里一段都不留）；
 *   ③ 只"问"前 N 个有边的词（走 `rel_direct` ⇒ 走 `token_assocs` ⇒ 触发按需读回），
 *   ④ 报：读回了几段 / 几字节 / 占整个段文件的百分比 / 平均每次读多少字节。
 *
 * 判据（必须真的成立，不能只看"跑完了"）：
 *   · 段数 > 0；卸下后 `n_loads == 0`、`n_bytes == 0`；
 *   · 问 N 个词后 **`n_loads <= N` 且 `n_loads < 段数`**（只读了问到的那些）；
 *   · 读回字节数 **< 段文件总大小**（真的只读了一部分）。
 *
 * 用法：gcc -std=c11 -O2 -o build_partbench tools/partbench.c -lm && ./build_partbench [model.bin]
 */
#include "../agent.h"      /* 与 main.c 同一套顺序：agent.h 里带 frame_io/rel/derive */
#include "../parts.h"
#include "../model_io.h"   /* ai_load_file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails = 0;
static void ok(int cond, const char *what) {
    if (cond) printf("  [OK]   %s\n", what);
    else { printf("  [FAIL] %s\n", what); fails++; }
}

int main(int argc, char **argv) {
    const char *model = argc > 1 ? argv[1] : "model.bin";
    char path[256];
    snprintf(path, sizeof path, "/tmp/aip_bench_%d.parts", (int)getpid());

    AI *A = ai_load_file(model);
    if (!A) { printf("FAIL partbench（读不到 %s）\n", model); return 1; }
    size_t ntok = A->pool->count, with_edges = 0, nedges = 0;
    for (size_t i = 0; i < ntok; ++i) {
        Token *t = A->pool->tokens[i];
        if (t && t->count) { with_edges++; nedges += t->count; }
    }
    printf("  图：%zu 个词 / %zu 个有边 / %zu 条边\n", ntok, with_edges, nedges);
    ok(with_edges > 1000, "① 这是**真实图**（有边的词 > 1000）");

    int segs = aip_parts_save(A, path);
    ok(segs > 0 && (size_t)segs == with_edges, "② 段文件：每个有边的词一段");
    struct stat st;
    long long fsize = (stat(path, &st) == 0) ? (long long)st.st_size : 0;
    ok(fsize > 0, "② 段文件在磁盘上，大小 > 0");
    printf("  段文件：%d 段 / %.1f MB\n", segs, (double)fsize / (1024.0 * 1024.0));

    AIParts *pt = aip_parts_open(path);
    ok(pt != NULL, "③ 只打开索引");
    size_t l0 = 0, b0 = 0, r0 = 0, e0 = 0;
    aip_parts_stats(pt, &l0, &b0, &r0, &e0);
    ok(l0 == 0 && b0 == 0, "③ 打开索引：0 段、0 字节");

    aip_parts_attach(A, pt);          /* §6.570 先装索引：detach 要知道"哪些词有段" */
    size_t det = aip_parts_detach(A);
    ok(det == with_edges, "④ 卸下：全部段的边离开内存");

    /* 只问前 N 个"有边的词"：每次问都会触发一次按需读回 */
    const size_t N = 50;
    uint8_t rid = rel_id_of(A, "属于", 0);
    size_t asked = 0;
    for (size_t i = 0; i < ntok && asked < N; ++i) {
        Token *t = A->pool->tokens[i];
        if (!t || t->count == 0) continue;
        /* 读它的边（rel_direct 会走 token_assocs ⇒ 触发读回） */
        if (rid) {
            for (size_t k = 0; k < t->count; ++k) {
                const AssocEntry *e = &token_assocs_ro(t)[k];
                if (e->main) { (void)rel_direct(t, rid, e->main); break; }
            }
        } else {
            (void)token_assocs(t);
        }
        asked++;
    }
    size_t l1 = 0, b1 = 0, r1 = 0, e1 = 0;
    aip_parts_stats(pt, &l1, &b1, &r1, &e1);
    printf("  问了 %zu 个词：读回 %zu 段 / %zu 字节（占段文件 %.2f%%）/ 平均每次 %zu 字节\n",
           asked, l1, b1, fsize ? 100.0 * (double)b1 / (double)fsize : 0.0,
           l1 ? b1 / l1 : 0);
    ok(l1 > 0, "⑤ 真的发生了按需读回");
    ok(l1 <= asked, "⑤ 读回段数 ≤ 问过的词数（没顺手把全图读回来）");
    ok(l1 < (size_t)segs, "⑤ **读回段数 < 总段数** —— 只加载了一部分");
    ok(b1 < (size_t)fsize, "⑤ **读回字节 < 段文件大小** —— 真的只读了一部分");
    ok(r1 > 0 && r1 <= l1, "⑤ 内存里现在驻留的段数可信（>0 且 ≤ 读回数）");

    aip_parts_close(pt);
    remove(path);
    ai_free(A);
    printf("\n%s（真实图：%d 段 / 问 %zu 词读回 %zu 段）\n",
           fails == 0 ? "PASS" : "FAIL", segs, asked, l1);
    return fails == 0 ? 0 : 1;
}
