/* tools/triples2c7b.c — **文本事实 → `.c7b` v2 分片**（流式，边到边写；§6.716）
 *
 * 为什么需要：`tools/src_harvest.sh` 抽出来的是**文本**（~54 B/条），7e9 条就是 378 GB，
 * 本机只剩 273 GB。转成 v2（~10 B/条）后 7B 只要 70–84 GB，才能把抓取持续下去。
 *
 * 输入格式（和 `::ingest` 一致）：
 *   `主体<TAB>关系<TAB>客体`；行尾 `\` 表示续行（代码片段那种多行记录），续行**裸写**。
 * 一个分片写满 `--max-edges`（默认 5 000 000）就落一个文件，接着写下一个：
 *   `<输出前缀>.000.c7b2`、`.001.c7b2`…
 *
 * 用法：triples2c7b <输入.triples> <输出前缀> [--max-edges N]
 */
#include "c7b2_writer.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: %s <输入.triples> <输出前缀> [--max-edges N]\n", argv[0]); return 2; }
    const char *in = argv[1], *prefix = argv[2];
    long max_edges = 5000000;
    const char *skip1 = NULL, *skip2 = NULL;
    for (int i = 3; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--max-edges")) max_edges = strtol(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--skip-rel")) { if (!skip1) skip1 = argv[i + 1]; else skip2 = argv[i + 1]; }
    }

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "读不开 %s\n", in); return 1; }
    C7B2W w;
    if (c7b2w_init(&w, 1 << 20) != 0) { fprintf(stderr, "内存不够\n"); fclose(f); return 1; }

    char *line = NULL, *obj = NULL;
    size_t lcap = 0, ocap = 0, olen = 0;
    ssize_t got;
    char *cur_s = NULL, *cur_p = NULL;
    int pending = 0;
    long shard = 0, total = 0;
    long long dropped = 0;

    while ((got = getline(&line, &lcap, f)) > 0) {
        while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) line[--got] = 0;
        if (got == 0) continue;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            /* 新记录：先把上一条落进去（关系在 skip 名单里的整条丢，含它的续行） */
            if (pending && cur_s && cur_p) {
                int skip = (skip1 && !strcmp(cur_p, skip1)) || (skip2 && !strcmp(cur_p, skip2));
                if (!skip && c7b2w_edge(&w, cur_s, cur_p, obj) == 0) total++;
            }
            *t1 = 0; *t2 = 0;
            free(cur_s); free(cur_p);
            cur_s = strdup(line); cur_p = strdup(t1 + 1);
            if ((skip1 && !strcmp(cur_p, skip1)) || (skip2 && !strcmp(cur_p, skip2))) pending = 0;
            const char *o = t2 + 1;
            size_t ol = strlen(o);
            int cont = (ol > 0 && o[ol - 1] == '\\');
            if (cont) ol--;
            if (ol + 1 > ocap) { ocap = ol + 256; obj = (char *)realloc(obj, ocap); }
            memcpy(obj, o, ol); obj[ol] = 0; olen = ol;
            pending = 1;
        } else if (pending) {
            /* 续行（裸写）：接上一行，补一个换行 */
            size_t ll = strlen(line);
            int cont = (ll > 0 && line[ll - 1] == '\\');
            if (cont) ll--;
            if (olen + ll + 2 > ocap) { ocap = olen + ll + 256; obj = (char *)realloc(obj, ocap); }
            obj[olen++] = '\n';
            memcpy(obj + olen, line, ll); olen += ll; obj[olen] = 0;
        }
        /* 分片写满：落盘、清空、继续 */
        if (w.edges >= max_edges) {
            char path[1024];
            snprintf(path, sizeof path, "%s.%03ld.c7b2", prefix, shard);
            long n = c7b2w_write(&w, path);
            if (n < 0) { fprintf(stderr, "写不进 %s\n", path); return 1; }
            fprintf(stderr, "  分片 %03ld：%ld 边 ⇒ %s\n", shard, n, path);
            shard++;
            dropped += (long long)w.dropped;
            c7b2w_free(&w);
            if (c7b2w_init(&w, 1 << 20) != 0) { fprintf(stderr, "内存不够\n"); return 1; }
        }
    }
    if (pending && cur_s && cur_p) {
        int skip = (skip1 && !strcmp(cur_p, skip1)) || (skip2 && !strcmp(cur_p, skip2));
        if (!skip && c7b2w_edge(&w, cur_s, cur_p, obj) == 0) total++;
    }
    if (w.edges > 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s.%03ld.c7b2", prefix, shard);
        long n = c7b2w_write(&w, path);
        if (n < 0) { fprintf(stderr, "写不进 %s\n", path); return 1; }
        fprintf(stderr, "  分片 %03ld：%ld 边 ⇒ %s\n", shard, n, path);
        shard++;
    }
    dropped += (long long)w.dropped;
    free(line); free(obj); free(cur_s); free(cur_p);
    c7b2w_free(&w);
    fclose(f);
    printf("  文本事实 %ld 条 ⇒ %ld 个分片（丢属性号超 u16 的 %lld 条）\n", total, shard, dropped);
    return shard > 0 ? 0 : 1;
}
