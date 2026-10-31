/* code.h — **"写-跑-看错-改"的机械闭环**（降级版；在内核之外，和 action.h 同一层）
 *
 * 用户 2026-10-07 的目标线 A：「让它从'会对话'走到'会写代码'」。
 * 这一步**不假装**它已经会写代码 —— 它是一个**机械**闭环，四件事：
 *     写文件 → 跑编译器 → 看错误 → 按**一张规则表**改一次 → 再跑一次
 * 所以验收是"**闭环真的走了一遍**"，不是"它把程序改对了"。降级版的边界写在下面"局限"里。
 *
 * 三个入口（同一份实现）：
 *   · `::code try <相对路径>` —— 对 `_t/work/` 里的一个源文件走一遍闭环；
 *   · `AIP_CODE=<相对路径>`   —— **代理循环**（`agent_step` 末尾）自己走一遍（默认关，只走一次）；
 *   · `::code`                —— 看这一轮的痕（编译几次 / 首次 rc / 最终 rc / 用了哪条修法）。
 *
 * 修法表：**一行一条**（错误的特征 → 改什么 → 为什么）。只有两种，
 * 都是"编译器指哪儿、规则就改哪儿"的机械动作：
 *   ① 报「expected … ;」   → 在它指的**上一行末尾**补一个 `;`
 *      （gcc 报的位置是"缺分号之后的第一处"，分号该补在上一行 —— 这条规则只认这一种形状）
 *   ② 报「implicit declaration of function」 → 按函数名补对应头文件
 *      （printf/puts… → stdio.h；str…/mem… → string.h；malloc/free/exit… → stdlib.h）
 * 认不出来的错误就**停在原地并说明**（"没有已知的机械修法"）—— 降级版不猜。
 *
 * 局限（照实说）：
 *   · 只会这两类修法；**不生成代码**（源文件是外面写好的，或者记事本的草稿）；
 *   · 最多改**一次**（`CODE_MAX_FIX`）：改完还错就报出来，不进入无限重试；
 *   · 编译命令固定 `gcc -std=c11 -Wall -Werror=implicit-function-declaration`
 *     （把"没声明的函数"从警告提成错误，否则规则②永远没机会触发 —— GCC 11 只给警告）；
 *   · 产物是 `<源文件>.out`，就落在 `_t/work/` 里（和 action.h 的边界一致）。 */
#ifndef CODE_H
#define CODE_H

#include "ai.h"
#include <dirent.h>
#include "lex.h"        /* ASCII 代码词法（::ascii on 时把"逐字保留"记进痕；见下） */

#define CODE_MAX_FIX   1
#define CODE_SRC_CAP   16384      /* 源文件读进来的上限（够写个小程序） */
#define CODE_ERR_CAP   192
#define CODE_OUT_NAME  ".out"

/* 修法表：一行一条（错误的特征 / 这条规则改什么 / 为什么） */
typedef struct {
    const char *err;     /* 错误行里出现这个子串就认它 */
    const char *fix;     /* 规则名（进痕、进日志） */
    const char *why;
} CodeFixRule;

static const CodeFixRule CODE_FIXES[] = {
    { "expected … ;",         "补分号",
      "gcc 说这一行缺 `;` → 在它指的**上一行末尾**补一个（它报的是缺分号之后的第一处）" },
    { "implicit declaration", "补 #include",
      "用了没声明的函数 → 按函数名补对应的头（printf/puts→stdio.h；str*/mem*→string.h；malloc/free/exit→stdlib.h）" },
    /* §6.622 **这条规则在 gcc 上以前是死的**：表里写的是 `"' undeclared"`（clang 的 ASCII 引号），
     * 而 gcc 报的是 `‘x’ undeclared`（UTF-8 弯引号）⇒ 永远匹配不上，规则 ③ 形同不存在。
     * 现在只要求出现 `undeclared`（规则 ② 的 `implicit declaration` 排在前面，函数那类仍走 ②）。 */
    { "undeclared",           "补声明",
      "用了没声明的**名字**（不是函数）→ 在出错行前补一句 `int <名字>;`（类型只能猜 int，"
      "错了它会再来一轮 —— 这就是「机械降级」的代价，照实说）" },
    { NULL, NULL, NULL }
};
#define CODE_FIX_N ((sizeof CODE_FIXES / sizeof CODE_FIXES[0]) - 1)

/* 一轮闭环的痕（`::code` 看的就是它；不进存档） */
typedef struct {
    int  attempts;                 /* 编译了几次（0 = 没跑起来） */
    int  first_rc, final_rc;
    int  rule;                     /* 用了哪条修法（-1 = 没用上） */
    char path[ACTION_PATH_CAP];    /* 源文件（相对 _t/work/） */
    char where[80];                /* 第一条错误的位置（如 `草稿.c:3:5`） */
    char err[CODE_ERR_CAP];        /* 第一条错误原文（截断） */
    char note[CODE_ERR_CAP];       /* 改了什么 / 为什么没改 */
    char lex[CODE_ERR_CAP];        /* 词法往返（只在 `::ascii on` 时算；空 = 这一层关着） */
} CodeRun;

static CodeRun g_code = { 0, -1, -1, -1, {0}, {0}, {0}, {0}, {0} };
static int g_code_auto_done = 0;   /* `AIP_CODE` 那条路只走一次（别每轮都编译） */
static inline CodeRun *code_last(void) { return &g_code; }

/* ---- 两条机械修法（都改**内存里的源文本**，改不动就返回 -1） ---- */

/* ① 在第 line 行（1 起）末尾补一个 `;` */
static inline int code_fix_semicolon(char *src, size_t cap, int line) {
    if (!src || line <= 0) return -1;
    size_t n = strlen(src), ln = 1, i = 0;
    while (i < n && ln < (size_t)line) { if (src[i] == '\n') ln++; i++; }
    if (ln != (size_t)line) return -1;
    size_t end = i;
    while (end < n && src[end] != '\n') end++;
    /* 末尾是 ';' 或 '{' 就不补（那说明规则认错了形状） */
    size_t k = end;
    while (k > i && (src[k - 1] == ' ' || src[k - 1] == '\t' || src[k - 1] == '\r')) k--;
    if (k == i || src[k - 1] == ';' || src[k - 1] == '{' || src[k - 1] == '}') return -1;
    if (n + 2 > cap) return -1;
    memmove(src + k + 1, src + k, n - k + 1);
    src[k] = ';';
    return 0;
}

/* ② 在文件开头补一个 `#include <…>`（已经有就不补） */
static inline const char *code_header_for(const char *err) {
    static const struct { const char *fn; const char *hdr; } M[] = {
        { "printf",   "stdio.h" }, { "puts",     "stdio.h" }, { "putchar", "stdio.h" },
        { "fprintf",  "stdio.h" }, { "snprintf", "stdio.h" }, { "fputs",   "stdio.h" },
        { "strlen",   "string.h" }, { "strcmp",  "string.h" }, { "strcpy",  "string.h" },
        { "strncpy",  "string.h" }, { "memcpy",  "string.h" }, { "memset",  "string.h" },
        { "strstr",   "string.h" }, { "strcat",  "string.h" },
        { "malloc",   "stdlib.h" }, { "calloc",  "stdlib.h" }, { "realloc", "stdlib.h" },
        { "free",     "stdlib.h" }, { "exit",    "stdlib.h" }, { "atoi",    "stdlib.h" },
        { "atof",     "stdlib.h" }, { "abs",     "stdlib.h" },
        { NULL, NULL }
    };
    if (!err) return NULL;
    for (size_t i = 0; M[i].fn; ++i)
        if (strstr(err, M[i].fn)) return M[i].hdr;
    return NULL;   /* 认不出函数名 → 不猜 */
}
static inline int code_fix_include(char *src, size_t cap, const char *hdr) {
    if (!src || !hdr) return -1;
    char probe[64];
    snprintf(probe, sizeof probe, "#include <%s>", hdr);
    if (strstr(src, probe)) return -1;            /* 已经有了 */
    size_t need = strlen(probe) + 1 + strlen(src) + 1;
    if (need > cap) return -1;
    char tmp[CODE_SRC_CAP];
    snprintf(tmp, sizeof tmp, "%s\n%s", probe, src);
    snprintf(src, cap, "%s", tmp);
    return 0;
}

/* **按错误文本选修法**（抽成一处：这样能用假错误串直接单测，不用真跑 gcc） */
/* §6.622 抠引号里的名字（两种引号：ASCII `'x'` / gcc 的 `‘x’`）。 */
static inline const char *code_quote_at(const char *p);

static inline const CodeFixRule *code_rule_for(const char *err) {
    if (!err || !err[0]) return NULL;
    /* 规则 0（补分号）要**真的在说分号**，而且不能是"文件到头了"那种 ——
     * `expected declaration or statement at end of input` 也含 expected/;，
     * 但补分号救不了它（认错形状就会瞎改，实测踩过）。这条判据以前散在 `code_round` 里，
     * 现在收进这个 helper：**一处放**，另一处只调它（也就能用假错误串直接单测）。 */
    if (strstr(err, "expected") && strstr(err, ";") && !strstr(err, "end of input"))
        return &CODE_FIXES[0];
    for (size_t i = 1; i < CODE_FIX_N; ++i) {
        if (!strstr(err, CODE_FIXES[i].err)) continue;
        /* §6.622 规则 ③（补声明）**只对合法的 ASCII 标识符**成立：
         * `return 不存在的东西;` 报 `‘不存在的东西’ undeclared` —— 给它补一句
         * `int 不存在的东西;` 不是"机械修法"，是瞎猜（而且 gcc 真会收下 Unicode 标识符）。
         * 认不出合法名字就当"没有已知修法"，照实说停手。 */
        if (strcmp(CODE_FIXES[i].fix, "补声明") == 0) {
            const char *q = code_quote_at(err);
            const char *n = q ? q + (q[0] == '\'' ? 1 : 3) : NULL;
            const char *e = n ? code_quote_at(n) : NULL;   /* ⚠ 必须停在**闭合引号**，不能扫到串尾 */
            int ascii_ok = (n && e && e > n);
            for (const char *t = n; ascii_ok && t && t < e; ++t)
                if (!((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z') ||
                      (*t >= '0' && *t <= '9') || *t == '_')) { ascii_ok = 0; break; }
            if (!ascii_ok) return NULL;
        }
        return &CODE_FIXES[i];
    }
    return NULL;
}

/* 补声明：在 line 前插一句 `int <name>;`（名字从 `'X' undeclared` 里抠）。
 * 类型只能猜 int —— 猜错就靠下一轮编译错误再修（这就是降级版的代价，不装懂）。 */
/* §6.622 **引号有两种**：clang 用 ASCII `'name'`，gcc 用 UTF-8 弯引号 `‘name’`。
 * 只认 ASCII 的话，gcc 上的「补声明」永远抠不出名字（实测 `x = 1;` ⇒ 认得出结论但名字抠不出来）。*/
static inline const char *code_quote_at(const char *p) {
    if (!p) return NULL;
    for (; *p; ++p) {
        if (*p == '\'') return p;
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 &&
            ((unsigned char)p[2] == 0x98 || (unsigned char)p[2] == 0x99)) return p;
    }
    return NULL;
}
static inline size_t code_quote_len(const char *q) { return (q && *q == '\'') ? 1u : 3u; }

static inline int code_fix_declare(char *src, size_t cap, const char *err, int line) {
    if (!src || !err) return -1;
    const char *q = code_quote_at(err);
    if (!q) return -1;
    q += code_quote_len(q);
    const char *e = code_quote_at(q);
    if (!e || e <= q) return -1;
    char name[64];
    size_t k = 0;
    while (q < e && k + 1 < sizeof name) name[k++] = *q++;
    name[k] = 0;
    if (k == 0 || k >= sizeof name - 8) return -1;
    char ins[96];
    snprintf(ins, sizeof ins, "int %s;\n", name);
    /* 在出错行之前插入（按行数定位；行号越界就插到开头） */
    size_t off = 0; int ln = 1;
    while (src[off] && ln < line) { if (src[off] == '\n') ln++; off++; }
    size_t il = strlen(ins);
    if (strlen(src) + il + 1 > cap) return -1;
    memmove(src + off + il, src + off, strlen(src + off) + 1);
    memcpy(src + off, ins, il);
    return 0;
}


/* 有界拼字符串：小缓冲别用 `snprintf("%s")` 收长名字/长理由 ——
 * `-Werror=format-truncation` 会拦下来（项目里这个坑踩过好几次）。 */
static inline void code_cat(char *dst, size_t cap, const char *a, const char *b, const char *c) {
    const char *parts[3] = { a, b, c };
    size_t k = 0;
    if (!dst || !cap) return;
    for (int i = 0; i < 3; ++i) {
        if (!parts[i]) continue;
        for (const char *q = parts[i]; *q && k + 1 < cap; ++q) dst[k++] = *q;
    }
    dst[k] = 0;
}

/* 闭环的痕也落一个观测标记（`[代码:<最终rc>]`；不是知识）——
 * 和 `[运行:rc]` 共用 action.h 里那**同一处**落标记的实现。 */
static inline int code_observe(AI *ai, int rc) {
    return action_observe_mark(ai, MICRO_CODE_TOKEN_PREFIX, rc, "写代码:闭环");
}

/* ---- 闭环本体 ---- */

/* 从编译器输出里挑**第一条 error**，并取出它的位置串（`文件:行:列`）。 */
static inline int code_first_error(const char *out, char *where, size_t wcap,
                                   char *msg, size_t mcap, int *line_out) {
    if (where && wcap) where[0] = 0;
    if (msg && mcap) msg[0] = 0;
    if (line_out) *line_out = 0;
    if (!out) return 0;
    for (const char *q = out; *q; ) {
        const char *nl = strchr(q, '\n');
        size_t len = nl ? (size_t)(nl - q) : strlen(q);
        const char *hit = strstr(q, ": error: ");
        if (hit && (size_t)(hit - q) < len) {
            /* 位置串：从行首到 ": error:" 之前的最后一个冒号前的部分（`f.c:3:5`） */
            if (where && wcap) {
                size_t k = (size_t)(hit - q);
                if (k >= wcap) k = wcap - 1;
                memcpy(where, q, k);
                where[k] = 0;
            }
            if (msg && mcap) {
                const char *m = hit + strlen(": error: ");
                size_t k = (nl ? (size_t)(nl - m) : strlen(m));
                if (k >= mcap) k = mcap - 1;
                memcpy(msg, m, k);
                msg[k] = 0;
            }
            if (line_out && where) {          /* 位置串的最后一个字段前一个字段 = 行号 */
                const char *last = strrchr(where, ':');
                if (last) {
                    const char *prev = last;
                    while (prev > where && prev[-1] != ':') prev--;
                    *line_out = atoi(prev);
                }
            }
            return 1;
        }
        if (!nl) break;
        q = nl + 1;
    }
    return 0;
}

/* 走一遍闭环：`rel` 是 `_t/work/` 里的相对路径。
 * 返回最终退出码（0 = 编过）/ -2 = 层关着或参数不合法（理由在 note 里）。 */
/* §6.642 **多文件工程**：把 _t/work/<dir>/ 下所有 .c 一次编出来（还是走受审的 gcc）。
 * 只做"编"，不做"改" —— 多文件的自动修法还没设计（照实说，见审计）。
 * 返回 0 = 编过；非 0 = gcc 的 rc；-2 = 没跑成（没开 ::act / 目录里没 .c）。 */
static inline int code_build_dir(AI *ai, const char *dir, char *note, size_t cap) {
    if (!ai || !dir || !dir[0]) return -2;
    if (!ai->act_on) {
        if (note && cap) snprintf(note, cap, "外部执行层关着（::act on）—— 编不了");
        return -2;
    }
    /* 自己列目录拼命令行（**不用通配符**）：受审执行层不保证过 shell，通配符可能原样传给 gcc
     * ⇒ "no such file"、而错误提取又抓不到（实测：报 rc=1 但错误字段是空的）。 */
    char cmd[ACTION_CMD_CAP];
    snprintf(cmd, sizeof cmd, "gcc -std=c11 -Wall -o %s/a.out", dir);
    char full[ACTION_PATH_CAP + 64];
    snprintf(full, sizeof full, "%s/%s", ACTION_WORK_DIR, dir);
    DIR *dp = opendir(full);
    if (!dp) {
        if (note && cap) snprintf(note, cap, "打不开目录 %s", full);
        return -2;
    }
    size_t nsrc = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if (l < 3 || strcmp(de->d_name + l - 2, ".c")) continue;
        size_t cl = strlen(cmd);
        int w = snprintf(cmd + cl, sizeof cmd - cl, " %s/%s", dir, de->d_name);
        if (w < 0 || cl + (size_t)w >= sizeof cmd) break;
        nsrc++;
    }
    closedir(dp);
    if (nsrc == 0) {
        if (note && cap) snprintf(note, cap, "%s 里没有 .c（编什么？）", dir);
        return -2;
    }
    int rc = action_run(ai, cmd);
    if (note && cap) {
        if (rc == 0) snprintf(note, cap, "%s：%zu 个 .c 一次编过（产物 %s/a.out）", dir, nsrc, dir);
        else {
            char where[CODE_ERR_CAP], err[CODE_ERR_CAP];
            int line = 0;
            code_first_error(action_book()->last_out, where, sizeof where, err, sizeof err, &line);
            snprintf(note, cap, "%s 编不过（rc=%d）：%s %s", dir, rc, where[0] ? where : "", err);
        }
    }
    (void)code_observe(ai, rc);
    return rc;
}

static inline int code_round(AI *ai, const char *rel);   /* §6.646：单文件测试要先走它 */

/* §6.646 **单文件产物也能跑**：`::code test <文件.c>` —— 先走一遍"写-编-看错-改"闭环，
 * 然后跑 `<文件>.out`（单文件闭环的产物名）。判据同上：产物退出码。
 * 只跑 `.out`（和 §6.645 同一条窄入口）。 */
static inline int code_test_file(AI *ai, const char *rel, char *note, size_t cap) {
    if (!ai || !rel || !rel[0]) return -2;
    int rc = code_round(ai, rel);
    if (rc != 0) {
        if (note && cap) snprintf(note, cap, "%s：**没编过**（rc=%d）—— 不跑", rel, rc);
        return rc;
    }
    char prog[ACTION_PATH_CAP + 32];
    snprintf(prog, sizeof prog, "%s%s", rel, CODE_OUT_NAME);   /* <文件>.out */
    int trc = action_run_product(ai, prog);
    if (note && cap) {
        if (trc == 0) snprintf(note, cap, "%s：编过、**测试全绿**（%s.out rc=0）", rel, rel);
        else {
            const char *o = action_book()->last_out;
            size_t ol = o ? strlen(o) : 0;
            if (ol > 200) o += ol - 200;
            snprintf(note, cap, "%s：编过但**测试不过**（rc=%d）：%.200s", rel, trc, o ? o : "");
        }
    }
    return trc;
}

/* §6.645 **测试驱动**：编 + **跑**（`_t/work/<dir>/a.out`）。
 * 判据是**产物退出码**：0 = 绿；非 0 = 红（把输出尾巴带出来）。
 * 只跑 `.out` 产物（`action_run_product` 那条窄入口），不碰别的命令。 */
static inline int code_test_dir(AI *ai, const char *dir, char *note, size_t cap) {
    if (!ai || !dir || !dir[0]) return -2;
    int rc = code_build_dir(ai, dir, note, cap);
    if (rc != 0) return rc;
    char prog[ACTION_PATH_CAP + 32];
    snprintf(prog, sizeof prog, "%s/a.out", dir);
    int trc = action_run_product(ai, prog);
    if (note && cap) {
        if (trc == 0) snprintf(note, cap, "%s：编过、**测试全绿**（a.out rc=0）", dir);
        else {
            char tail[256] = {0};
            const char *o = action_book()->last_out;
            size_t ol = o ? strlen(o) : 0;
            if (ol > 200) o += ol - 200;
            snprintf(tail, sizeof tail, "%.200s", o ? o : "");
            snprintf(note, cap, "%s：编过但**测试不过**（rc=%d）：%s", dir, trc, tail);
        }
    }
    return trc;
}

static inline int code_round(AI *ai, const char *rel) {
    CodeRun *cr = &g_code;
    cr->attempts = 0; cr->first_rc = cr->final_rc = -1; cr->rule = -1;
    cr->where[0] = cr->err[0] = cr->note[0] = cr->lex[0] = 0;
    snprintf(cr->path, sizeof cr->path, "%s", rel ? rel : "");
    if (!ai || !rel || !rel[0]) { snprintf(cr->note, sizeof cr->note, "没给源文件"); return -2; }
    if (!ai->act_on) {
        snprintf(cr->note, sizeof cr->note,
                 "外部执行层关着（::act on / AIP_ACT=1）—— 闭环不走它就没有'受审'可言");
        return -2;
    }
    char full[ACTION_PATH_CAP + 48];
    snprintf(full, sizeof full, "%s/%s", ACTION_WORK_DIR, rel);
    FILE *f = fopen(full, "rb");
    if (!f) { code_cat(cr->note, sizeof cr->note, "读不开 ", full, "（先 ::act write 或让它写出来）"); return -2; }
    char *src = (char *)malloc(CODE_SRC_CAP);
    if (!src) { fclose(f); snprintf(cr->note, sizeof cr->note, "内存不够"); return -2; }
    size_t got = fread(src, 1, CODE_SRC_CAP - 1, f);
    fclose(f);
    src[got] = 0;
    /* §6.641 **回滚用的原件**：在**任何修改之前**留一份（第一版放晚了：拷贝的是已经改过的 src，
     * 于是"还原"还原成了改后的样子 —— 实测文件里分号还在）。 */
    char *orig = (char *)malloc(CODE_SRC_CAP);
    if (orig) memcpy(orig, src, got + 1);

    /* 代码词法（`::ascii on` 才走）：源码先过一遍 **ASCII 逐字保留** 判据 ——
     * "我写出去的字节，词法层一个都没改" 是写代码这件事的前置条件（线 A ③）。
     * 判据只有一处实现：lex.h 的 lex_roundtrip()。默认关 → 这一行不算、输出逐字节同今天。 */
    if (ai->lex_ascii) {
        char why[160];
        if (lex_roundtrip(src, why, sizeof why))
            snprintf(cr->lex, sizeof cr->lex, "ASCII 逐字保留 ✓（canonical 幂等）");
        else
            snprintf(cr->lex, sizeof cr->lex, "ASCII 逐字保留 ✗：%s", why);
    }

    char cmd[ACTION_CMD_CAP];
    snprintf(cmd, sizeof cmd,
             "gcc -std=c11 -Wall -Werror=implicit-function-declaration -o %s%s %s",
             rel, CODE_OUT_NAME, rel);
    int rc = action_run(ai, cmd);
    cr->attempts = 1;
    cr->first_rc = rc;
    if (rc == 0) {
        snprintf(cr->note, sizeof cr->note, "一次就编过（%s%s 出来了）", rel, CODE_OUT_NAME);
        cr->final_rc = 0;
        (void)code_observe(ai, 0);
        free(src);
        return 0;
    }
    /* 看错：第一条 error + 位置 */
    int line = 0;
    code_first_error(action_book()->last_out, cr->where, sizeof cr->where,
                     cr->err, sizeof cr->err, &line);
    /* 改：按表认规则。规则①要**真的在说分号**，而且不能是"文件到头了"那种形状
     * （`expected declaration or statement at end of input` 也含 "expected"，
     *  但补分号救不了它 —— 认错形状就会瞎改，实测踩过）。 */
    const CodeFixRule *rule = code_rule_for(cr->err);   /* 判据在 helper 里（可单测） */
    if (!rule) {
        snprintf(cr->note, sizeof cr->note,
                 "没有已知的机械修法（降级版只认表里那三类错）—— 停在原地，不猜");
        cr->final_rc = rc;
        (void)code_observe(ai, rc);
        free(src);
        return rc;
    }
    cr->rule = (int)(rule - CODE_FIXES);
    int fixed = -1;
    if (cr->rule == 0) {
        /* 「expected … ;」→ 补在报错位置的**上一行**（gcc 报的是缺分号之后的第一处） */
        fixed = code_fix_semicolon(src, CODE_SRC_CAP, line > 1 ? line - 1 : line);
        if (fixed != 0) snprintf(cr->note, sizeof cr->note, "认出了「补分号」但那一行补不进去（第 %d 行）", line > 1 ? line - 1 : line);
    } else if (cr->rule == 2) {
        /* §6.97 第三条修法：**补声明**（在出错行前插 `int <名字>;`）—— 与另两条同构：
         * 只改 `src`，后面统一写回/再编；改不动就把 `fixed` 留成非 0，走"停在这"那条。 */
        fixed = code_fix_declare(src, CODE_SRC_CAP, cr->err, line > 1 ? line : 1);
        if (fixed != 0) snprintf(cr->note, sizeof cr->note, "认出了「补声明」但名字抠不出来 —— 停在这");
        else snprintf(cr->note, sizeof cr->note, "按规则补了声明（类型只能猜 `int`；错了下一轮再修）");
    } else {
        const char *hdr = code_header_for(cr->err);
        if (!hdr) snprintf(cr->note, sizeof cr->note, "认出了「补 #include」但函数名不在表里 —— 不猜");
        else {
            fixed = code_fix_include(src, CODE_SRC_CAP, hdr);
            if (fixed != 0) snprintf(cr->note, sizeof cr->note, "`#include <%s>` 已经有了或者加不进去", hdr);
            else snprintf(cr->note, sizeof cr->note, "按规则补了 `#include <%s>`", hdr);
        }
    }
    if (fixed != 0) {
        cr->final_rc = rc;
        (void)code_observe(ai, rc);
        free(src);
        return rc;
    }
    if (cr->rule == 0)
        snprintf(cr->note, sizeof cr->note, "按规则在**上一行**末尾补了一个 `;`（第 %d 行）",
                 line > 1 ? line - 1 : line);
    /* 写回去（还是走受审的写文件通道）→ 再跑一次 */
    if (action_write_file(ai, rel, src) != 0) {
        code_cat(cr->note, sizeof cr->note, "改完写不回去：", action_book()->last_reason, NULL);
        cr->final_rc = rc;
        (void)code_observe(ai, rc);
        free(src);
        return rc;
    }
    int rc2 = action_run(ai, cmd);
    cr->attempts = 2;
    cr->final_rc = rc2;
    /* §6.641 **还编不过 ⇒ 回滚**（只在"不再多轮"的默认路径上做；多轮开着时交给多轮） */
    if (rc2 != 0 && !ai->code_multi && orig) {
        if (action_write_file(ai, rel, orig) == 0) {
            code_cat(cr->note, sizeof cr->note,
                     "；**改完还编不过 ⇒ 已把文件还原**（不给你留半坏的源码）", NULL, NULL);
        }
    }
    free(orig);
    /* **C③ 第二件：多轮自我修正**（默认关，`::code multi on` / `AIP_CODEMULTI=1`）：
     * 第二次还编不过时，**用新的错误再认一遍规则**、再改一处、再编一次（总次数 ≤ 3）。
     * 老行为 = 只改一次（默认关 ⇒ 逐字节相同）；这里只用多出来的那一轮，不改已有那一轮。 */
    if (rc2 != 0 && ai->code_multi && cr->attempts < 3) {
        char where2[CODE_ERR_CAP];
        int line2 = 0;
        code_first_error(action_book()->last_out, where2, sizeof where2, cr->err, sizeof cr->err, &line2);
        const CodeFixRule *r2 = code_rule_for(cr->err);
        if (r2) {
            int idx2 = (int)(r2 - CODE_FIXES);
            char *src2 = (char *)malloc(CODE_SRC_CAP);
            FILE *f2 = fopen(full, "rb");
            size_t got2 = 0;
            if (src2 && f2) { got2 = fread(src2, 1, CODE_SRC_CAP - 1, f2); }
            if (f2) fclose(f2);
            if (src2) {
                src2[got2] = 0;
                int fx2 = -1;
                if (idx2 == 0) fx2 = code_fix_semicolon(src2, CODE_SRC_CAP, line2 > 1 ? line2 - 1 : line2);
                else if (idx2 == 2) fx2 = code_fix_declare(src2, CODE_SRC_CAP, cr->err, line2 > 1 ? line2 : 1);
                else {
                    const char *h2 = code_header_for(cr->err);
                    if (h2) fx2 = code_fix_include(src2, CODE_SRC_CAP, h2);
                }
                if (fx2 == 0) {
                    FILE *w2 = fopen(full, "wb");
                    if (w2) { (void)fwrite(src2, 1, strlen(src2), w2); fclose(w2); }
                    cr->rule = idx2;
                    int rc3 = action_run(ai, cmd);
                    cr->attempts = 3;
                    cr->final_rc = rc3;
                    char keep3[CODE_ERR_CAP];
                    snprintf(keep3, sizeof keep3, "%s", cr->note);
                    code_cat(cr->note, sizeof cr->note, keep3,
                             rc3 == 0 ? " —— **第三轮改过之后编过了**（多轮修正生效）"
                                      : " —— 第三轮还是没过（够三轮了，停）", NULL);
                    if (rc3 == 0) (void)code_observe(ai, 0);
                }
                free(src2);
            }
        }
    }
    if (rc2 == 0) {
        char keep[CODE_ERR_CAP];
        snprintf(keep, sizeof keep, "%s", cr->note);
        code_cat(cr->note, sizeof cr->note, keep, " —— 再编**过了**", NULL);
    } else {
        char keep[CODE_ERR_CAP];
        snprintf(keep, sizeof keep, "%s", cr->note);
        code_cat(cr->note, sizeof cr->note, keep,
                 " —— 再编**还是没过**（降级版最多改一次，停在这里）", NULL);
    }
    (void)code_observe(ai, rc2);
    free(src);
    return rc2;
}

/* `::code` 的痕（就这一处拼） */
static inline void code_print(void) {
    CodeRun *cr = &g_code;
    if (cr->attempts == 0 && !cr->note[0]) {
        printf("[写代码闭环] 还没走过。用法：::code try <相对路径>（文件在 " ACTION_WORK_DIR "/ 里）\n");
        printf("  修法表（一行一条）：\n");
        for (size_t i = 0; i < CODE_FIX_N; ++i)
            printf("    %-22s %-10s %s\n", CODE_FIXES[i].err, CODE_FIXES[i].fix, CODE_FIXES[i].why);
        printf("  降级版边界：只这两类修法、最多改一次、**不生成代码**（源文件是外面写的）\n");
        return;
    }
    printf("[写代码闭环] %s：编译 %d 次，首次 rc=%d → 最终 rc=%d\n",
           cr->path[0] ? cr->path : "?", cr->attempts, cr->first_rc, cr->final_rc);
    if (cr->where[0]) printf("  第一条错误：%s：%s\n", cr->where, cr->err);
    if (cr->lex[0])   printf("  词法：%s\n", cr->lex);
    if (cr->note[0])  printf("  改了什么：%s\n", cr->note);
    if (cr->attempts >= 2 || cr->first_rc == 0)
        printf("  产物：%s/%s%s（在 " ACTION_WORK_DIR "/ 里）\n", ACTION_WORK_DIR, cr->path, CODE_OUT_NAME);
}

/* 代理循环那条路：`AIP_CODE=<相对路径>` 时**自己**走一遍（默认关；一个进程只走一次） */
static inline int code_auto_step(AI *ai) {
    if (g_code_auto_done) return 0;
    const char *rel = getenv("AIP_CODE");
    if (!rel || !rel[0]) return 0;
    g_code_auto_done = 1;
    (void)code_round(ai, rel);
    /* 把痕打出来：这条路是**测量/诊断钩子**（只在设了 AIP_CODE 时活），
     * 不打出来的话"跑过了"在日志里看不见（实测踩过：文件改了、产物有了、日志是空的）。 */
    printf("[代理循环自己走了一遍写代码闭环：AIP_CODE=%s]\n", rel);
    code_print();
    return 1;
}

#endif /* CODE_H */
