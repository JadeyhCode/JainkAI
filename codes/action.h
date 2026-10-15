/* action.h — **受审的外部执行 + 写文件**（在内核之外，和 drive.h 同一层）
 *
 * 用户 2026-10-08 拍板：
 *   · 白名单 = `echo / cat / wc / gcc` **再加 `grep / head / tail / ls`**（**不含 `sh`**）
 *     ⚠ **§6.126：`sed` 已移出白名单**（外部审计 2026-10-15 的 P0，本机复现过）——
 *     GNU sed 的 `e` 命令把**缓冲区内容**交给 /bin/sh 执行，而边界检查只看 argv 表面
 *     （首词白名单 / 参数不以 `/` 开头 / 不含 `..`），payload 藏在**文件内容**里 ⇒ 全部绕过。
 *     实测：`::act write pwn.txt echo … > /tmp/x` 后 `::act run sed e pwn.txt`，
 *     工作区外的 `/tmp/x` 真的被写出来了。cat+echo+gcc 已覆盖它的场景。
 *   · 写文件固定在 `_t/work/`；**不联网**（白名单里没有网络工具）
 *   · `AIP_ACT=1` / `::act on` 打开，**默认关**
 *
 * 设计（audit §6.49 六步，其中两处按拍板改过）：
 *   ① **不用 shell**：`fork` + `execvp` + `argv` 白名单 ——
 *      `;` `|` `&` `>` 反引号 `$()` **只是普通参数**（注入问题自消，所以不再需要"元字符一律拒"）。
 *   ② **超时自己 `fork`/`kill`**（`popen` 做不到超时）：到点 `SIGKILL`，退出码约定 **-9**。
 *   ③ 子进程的 **cwd 固定 = `_t/work/`**，stdin = `/dev/null`，stdout+stderr 合并进管道；
 *      **参数里不许有绝对路径、不许有 `..`** ⇒ 读/写都被关在工作区里（连 `sed -i` 也一样）。
 *   ④ 输出算**观测**：经意图队列落一个 `[运行:<rc>]` 标记节点
 *      （与 `[草稿:…]`/`[自问:…]` 同一套约定）—— **不建关系边、不进我的知识**；
 *      进图这件事仍然只走安全点（这里是 `micro_drain_queue`：与 `micro_step` 同一份实现）。
 *   ⑤ 每一次拒绝**都留理由**（`last_reason`），并且账要能对上：
 *      `n_run / n_ok / n_failed / n_timeout / n_refused / n_write`。
 *
 * 边界（照实说）：
 *   · 账（`ActionBook`）是**进程级**的（层里的 static），不随 AI 实例走、不进存档；
 *   · 白名单是**首词精确匹配**，所以 `./gcc`、`/usr/bin/gcc`、`gcc-13` 都不放行；
 *   · 工作区里的**符号链接**不管（白名单里没有 `ln`，工作区也是我们自己建的）；
 *   · `sed -i` 能改的也只是 `_t/work/` 里的文件（cwd 在那儿、路径不许越界）；
 *   · 输出只留最后 4 KB（截断处写明），全量输出要留档就 `::act run … > 文件` 这条路**没有**
 *     （重定向是 shell 的事）—— 想要落盘就用 `::act write` 或让命令自己 `-o` 到工作区。
 * 默认**关**，要改行为先 A/B（和别处同一条纪律）。 */
#ifndef ACTION_H
#define ACTION_H

#include "ai.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ACTION_WORK_DIR      "_t/work"     /* 子进程 cwd + 写文件根（唯一一处） */
#define ACTION_OUT_CAP       4096          /* 最近一次输出留多少字节 */
#define ACTION_PATH_CAP      256
#define ACTION_ARG_MAX       32
#define ACTION_TIMEOUT_EXIT  (-9)          /* 超时被杀的约定退出码 */
#define ACTION_CMD_CAP       512

/* 白名单：一行一条（首词 / 超时 / 为什么允许）。加命令只加一行。 */
typedef struct {
    const char *cmd;
    long        timeout_ms;
    const char *why;
} ActionAllow;

static const ActionAllow ACTION_ALLOW[] = {
    { "echo",  2000,  "把文本原样打出来（看编码/换行/她写的字长什么样）" },
    { "cat",   2000,  "读工作区里的文件（纯读；管道/FIFO 也走它）" },
    { "wc",    2000,  "数行/词/字节（她写出来的东西有多大）" },
    { "gcc",  20000,  "编译：**这条是「会写代码」的核心** —— 编译错误就是观测" },
    { "grep",  5000,  "在工作区里找一行" },
    { "head",  2000,  "看开头几行" },
    { "tail",  2000,  "看结尾几行" },
    { "ls",    2000,  "列工作区目录（只看一眼自己写了什么）" },
    { NULL, 0, NULL }
};
#define ACTION_ALLOW_N ((sizeof ACTION_ALLOW / sizeof ACTION_ALLOW[0]) - 1)

/* 审计账 + 最近一次的样子（进程级；不进存档） */
typedef struct {
    size_t n_run;        /* 真跑起来的次数（不含被拒） */
    size_t n_ok;         /* 退出码 0 */
    size_t n_failed;     /* 非零退出码（**不是错误**：编译报错就是要看的观测） */
    size_t n_timeout;    /* 超时被杀 */
    size_t n_refused;    /* 被拒（白名单/路径/层关着/建不出工作区） */
    size_t n_write;      /* 写文件尝试次数 */
    size_t n_write_failed;   /* 其中写失败/被拒之外的 IO 失败 */
    int    last_rc;      /* -1 = 还没跑过 */
    long   last_ms;
    char   last_cmd[ACTION_CMD_CAP];
    char   last_reason[192];     /* 拒绝/失败的理由（**必须有**） */
    char   last_out[ACTION_OUT_CAP];
    char   last_mark[48];        /* 落的观测标记名（如 `[运行:0]`） */
} ActionBook;

static ActionBook g_act = { 0, 0, 0, 0, 0, 0, 0, -1, 0, {0}, {0}, {0}, {0} };
static long g_act_timeout_ms = 0;   /* 0 = 用表里的；`::act timeout <ms>` 可覆盖（也给看护用） */

static inline ActionBook *action_book(void) { return &g_act; }
/* 超时覆盖（毫秒；0 = 用白名单表里的）。给 `::act timeout <ms>` 与看护用 ——
 * 单独一个口，别处不许直接摸 g_act_timeout_ms。 */
#define ACTION_TIMEOUT_MAX 120000L
static inline void action_set_timeout(long ms) {
    if (ms < 0) ms = 0;
    if (ms > ACTION_TIMEOUT_MAX) ms = ACTION_TIMEOUT_MAX;
    g_act_timeout_ms = ms;
}
static inline long action_timeout_override(void) { return g_act_timeout_ms; }
/* 打印最近一次输出（缩进由调用方给；`::act` 与 `::act run` 共用这一处） */
static inline void action_print_out(const char *indent) {
    const char *pad = indent ? indent : "";
    if (!g_act.last_out[0]) { printf("%s（还没有输出）\n", pad); return; }
    for (const char *q = g_act.last_out; *q; ) {
        const char *nl = strchr(q, '\n');
        size_t len = nl ? (size_t)(nl - q) : strlen(q);
        printf("%s%.*s\n", pad, (int)len, q);
        if (!nl) break;
        q = nl + 1;
    }
}
static inline long action_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static inline const ActionAllow *action_allow_find(const char *cmd) {
    if (!cmd) return NULL;
    for (size_t i = 0; i < ACTION_ALLOW_N; ++i)
        if (strcmp(ACTION_ALLOW[i].cmd, cmd) == 0) return &ACTION_ALLOW[i];
    return NULL;
}
/* 白名单名字串（打印/理由里用；只有这一处拼） */
static inline void action_allow_names(char *out, size_t cap) {
    size_t k = 0;
    if (!out || !cap) return;
    out[0] = 0;
    for (size_t i = 0; i < ACTION_ALLOW_N && k + 2 < cap; ++i) {
        const char *q = ACTION_ALLOW[i].cmd;
        while (*q && k + 2 < cap) out[k++] = *q++;
        if (i + 1 < ACTION_ALLOW_N && k + 2 < cap) out[k++] = ' ';
    }
    out[k] = 0;
}

/* mkdir -p（不走 shell） */
static inline int action_mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    char buf[ACTION_PATH_CAP + 32];
    size_t n = strlen(path);
    if (n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (buf[i] == '/' || buf[i] == 0) {
            char keep = buf[i];
            buf[i] = 0;
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) { buf[i] = keep; return -1; }
            buf[i] = keep;
        }
    }
    return 0;
}
static inline int action_ensure_workdir(void) {
    struct stat st;
    if (stat(ACTION_WORK_DIR, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return action_mkdir_p(ACTION_WORK_DIR);
}

/* 把一行拆成 argv（**不用 shell**：只在空白处切，不做引号/转义/通配）。
 * 返回 argc（0 = 空命令）。 */
static inline int action_split(char *buf, char *argv[], int max) {
    int argc = 0;
    if (!buf) return 0;
    for (char *q = buf; *q && argc < max; ) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) break;
        argv[argc++] = q;
        while (*q && *q != ' ' && *q != '\t') q++;
        if (*q) *q++ = 0;
    }
    /* ⚠ 必须 NULL 结尾：execvp 要 NULL 终止的 argv，少了这一句的症状是
     * "每条命令都 rc=127"（execvp 顺着栈上的垃圾读下去就失败）。 */
    argv[argc < max ? argc : max] = NULL;      /* 永远 NULL 结尾（见上） */
    return argc;
}

/* 安全边界（**唯一一份**）：首词必须在白名单里；参数不许绝对路径、不许 `..`。
 * 元字符不用管 —— 没有 shell，它们只是普通参数。 */
static inline int action_check(const char *const *argv, int argc, char *why, size_t cap) {
    if (argc <= 0 || !argv[0] || !argv[0][0]) {
        snprintf(why, cap, "空命令（用法：::act run <命令> [参数…]）");
        return -1;
    }
    if (strchr(argv[0], '/')) {
        snprintf(why, cap, "首词不许带路径（%s）—— 白名单只认命令名", argv[0]);
        return -1;
    }
    const ActionAllow *al = action_allow_find(argv[0]);
    if (!al) {
        char names[128];
        action_allow_names(names, sizeof names);
        snprintf(why, cap, "首词 %s 不在白名单里（只许：%s）", argv[0], names);
        return -1;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '/') {
            snprintf(why, cap, "不许绝对路径：%s（子进程 cwd 已经是 %s）", argv[i], ACTION_WORK_DIR);
            return -1;
        }
        if (strstr(argv[i], "..")) {
            snprintf(why, cap, "不许 ..（爬出工作区）：%s", argv[i]);
            return -1;
        }
        /* §6.126（外部审计 P1-1，本机复现过）：**`@响应文件`** 会把参数从文件里读出来，
         * 这里的 argv 检查一个字符都看不到（实测 `gcc @args.txt` 里写 `-o /tmp/x`，
         * 二进制就落在工作区外）。一律拒。 */
        if (argv[i][0] == '@') {
            snprintf(why, cap, "不许 @响应文件（参数会从文件里读出来，绕过这里的检查）：%s", argv[i]);
            return -1;
        }
        /* **开关参数里也不许夹带路径**：`-o/tmp/x`、`-I/etc`、`-specs=/x` 都是一个参数，
         * 不以 `/` 开头 ⇒ 老检查放行。规矩：以 `-` 开头的参数里不许出现 `/`。 */
        if (argv[i][0] == '-' && strchr(argv[i], '/')) {
            snprintf(why, cap, "开关参数里不许带路径（%s）—— 例如 -o/tmp/x、-I/etc 会把结果写到工作区外",
                     argv[i]);
            return -1;
        }
    }
    return 0;
}

/* §6.126（P1-1 读越界）：**源码里的 include 也要查** —— argv 检查看不到文件内容。
 * 审计实测：源文件里写 `#include "/etc/passwd"`（或 `../../../../etc/passwd`），
 * `gcc -E` 就能把工作区外的内容打到 stdout、跟着进观测标记。
 * 只扫工作区里的 `.c`/`.h`：`<stdio.h>` 这类系统头（相对 include 路径）照旧放行，
 * 只拦**绝对路径**和带 `..` 的。 */
static inline int action_scan_includes(const char *const *argv, int argc, char *why, size_t cap) {
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!a || a[0] == '-' || a[0] == '@') continue;
        size_t len = strlen(a);
        if (len < 2 || (strcmp(a + len - 2, ".c") != 0 && strcmp(a + len - 2, ".h") != 0)) continue;
        char path[ACTION_PATH_CAP];
        snprintf(path, sizeof path, "%s/%s", ACTION_WORK_DIR, a);
        FILE *f = fopen(path, "r");
        if (!f) continue;                        /* 不存在的文件交给 gcc 自己报错 */
        char line[512];
        int bad = 0;
        char target[256];
        target[0] = 0;
        while (fgets(line, sizeof line, f)) {
            const char *h = strstr(line, "#include");
            if (!h) continue;
            h += 8;
            while (*h == ' ' || *h == '\t') h++;
            if (*h != '"' && *h != '<') continue;
            char close = (*h == '"') ? '"' : '>';
            const char *e = strchr(h + 1, close);
            if (!e) continue;
            size_t n = (size_t)(e - (h + 1));
            if (n >= sizeof target) n = sizeof target - 1;
            memcpy(target, h + 1, n);
            target[n] = 0;
            if (target[0] == '/' || strstr(target, "..")) { bad = 1; break; }
        }
        fclose(f);
        if (bad) {
            snprintf(why, cap, "源码里的 #include 越界（%s → %s）—— 编译期会把工作区外的内容读进来",
                     a, target);
            return -1;
        }
    }
    return 0;
}

/* 写文件路径的边界（同一套规矩，单独一处） */
static inline int action_path_check(const char *rel, char *why, size_t cap) {
    if (!rel || !rel[0]) { snprintf(why, cap, "没给路径"); return -1; }
    if (rel[0] == '/') { snprintf(why, cap, "不许绝对路径：%s", rel); return -1; }
    if (strstr(rel, "..")) { snprintf(why, cap, "不许 ..（爬出工作区）：%s", rel); return -1; }
    if (strlen(rel) >= ACTION_PATH_CAP - 16) { snprintf(why, cap, "路径太长（上限 %d）", ACTION_PATH_CAP - 16); return -1; }
    return 0;
}

/* 落一个**观测标记**节点 `[<prefix><rc>]`（不建关系边、不进知识）。
 * 进图只走安全点：micro_drain_queue()（与 micro_step 同一份实现）。
 * **唯一一份**实现：动作层（`[运行:rc]`）与写代码闭环（`[代码:rc]`）都走它。 */
static inline int action_observe_mark(AI *ai, const char *prefix, int rc, const char *source) {
    if (!ai || !ai->pool || !prefix) return -1;
    char nm[48];
    snprintf(nm, sizeof nm, "%s%d]", prefix, rc);
    ModIntent m;
    memset(&m, 0, sizeof m);
    m.type = MOD_ADD_TOKEN;
    /* 挂回"我"；没有"我"（单元测试里的空脑子）就退回池里第一个普通符号节点 ——
     * 这个挂靠点只是归属，不参与语义（确定性：按池子下标取第一个）。 */
    Token *host = ai->self;
    if (!host)
        for (size_t i = 0; i < ai->pool->count; ++i) {
            Token *t = ai->pool->tokens[i];
            if (t && !t->is_garbage && t->name && t->name[0] != '[') { host = t; break; }
        }
    m.target = host;
    snprintf(m.name, sizeof m.name, "%s", nm);
    snprintf(m.source, sizeof m.source, "%s", source ? source : "观测");
    Micro *mk = micro_of(ai);
    if (!mk) return -1;
    /* 一次动作算内核眼里的**一小步**：重开这一步的预算，否则第一条标记就把
     * 「一步最多造几个符号节点」用光，后面的标记会被安全内核按预算拒掉（实测过：
     * 账上"观测标记 1 个"而实际跑了三条命令）。 */
    safety_step_begin(ai);
    if (!mod_propose(&mk->queue, &m)) return -1;
    (void)micro_drain_queue(ai);
    snprintf(g_act.last_mark, sizeof g_act.last_mark, "%s", nm);
    g_act.last_rc = rc;
    return 0;
}
static inline int action_observe(AI *ai, int rc) {
    return action_observe_mark(ai, MICRO_RUN_TOKEN_PREFIX, rc, "动作:运行");
}

/* 拒绝的**统一出口**：记账 + 写理由（别的路径不许自己拼理由） */
static inline int action_refuse(const char *what, const char *why) {
    g_act.n_refused++;
    g_act.last_rc = -1;
    g_act.last_out[0] = 0;
    g_act.last_mark[0] = 0;
    snprintf(g_act.last_cmd, sizeof g_act.last_cmd, "%s", what ? what : "");
    snprintf(g_act.last_reason, sizeof g_act.last_reason, "%s", why ? why : "（没写理由）");
    return -2;
}

/* 跑一条外部命令。返回：子进程退出码（正常）/ -9（超时被杀）/ -2（被拒）。 */
static inline int action_run(AI *ai, const char *line) {
    /* 注意：`action_check` 只查 argv 表面；`#include` 要看**文件内容** ⇒ 另扫一遍。 */
    if (!ai) return -1;
    g_act.last_reason[0] = 0; g_act.last_out[0] = 0; g_act.last_mark[0] = 0;
    g_act.last_rc = -1; g_act.last_ms = 0;
    if (!ai->act_on)
        return action_refuse(line, "这一层**默认关**（::act on 打开 / AIP_ACT=1）—— 没打开就绝不动外部世界");
    char buf[ACTION_CMD_CAP];
    snprintf(buf, sizeof buf, "%s", line ? line : "");
    /* §6.126（外部审计 P2-8）：**多留一格**给 NULL —— 正好 32 个参数时
     * `if (argc < max)` 那个保护不成立，execvp 会顺着栈上的垃圾往下读（症状 rc=127）。 */
    char *argv[ACTION_ARG_MAX + 1];
    int argc = action_split(buf, argv, ACTION_ARG_MAX);
    char why[192];
    if (action_check((const char *const *)argv, argc, why, sizeof why) != 0)
        return action_refuse(line, why);
    if (action_scan_includes((const char *const *)argv, argc, why, sizeof why) != 0)
        return action_refuse(line, why);
    if (action_ensure_workdir() != 0)
        return action_refuse(line, "工作目录建不出来（" ACTION_WORK_DIR "）");

    const ActionAllow *al = action_allow_find(argv[0]);
    long tmo = (g_act_timeout_ms > 0) ? g_act_timeout_ms : al->timeout_ms;

    int pfd[2];
    if (pipe(pfd) != 0) return action_refuse(line, "pipe() 失败");
    long t0 = action_now_ms();
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        return action_refuse(line, "fork() 失败");
    }
    if (pid == 0) {
        /* 子进程：stdin = /dev/null（不许读我们的输入），stdout+stderr 进管道，
         * cwd = 工作区（相对路径只能落在里面）。 */
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { (void)dup2(dn, 0); close(dn); }
        (void)dup2(pfd[1], 1);
        (void)dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        if (chdir(ACTION_WORK_DIR) != 0) _exit(126);
        execvp(argv[0], argv);
        _exit(127);                      /* 127 = 命令没起来（execvp 失败） */
    }
    close(pfd[1]);
    int fl = fcntl(pfd[0], F_GETFL, 0);
    if (fl >= 0) (void)fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);

    size_t used = 0;
    int reaped = 0, status = 0, timed_out = 0;
    while (!reaped) {
        if (!timed_out && action_now_ms() - t0 > tmo) {
            timed_out = 1;
            (void)kill(pid, SIGKILL);     /* 超时**自己**杀：popen 做不到这件事 */
        }
        struct pollfd p = { pfd[0], POLLIN, 0 };
        (void)poll(&p, 1, 20);
        for (;;) {
            char tmp[1024];
            ssize_t got = read(pfd[0], tmp, sizeof tmp);
            if (got <= 0) break;
            if (used + (size_t)got < sizeof g_act.last_out) {
                memcpy(g_act.last_out + used, tmp, (size_t)got);
                used += (size_t)got;
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) reaped = 1;
    }
    /* 收尾：把管道里剩的取干净（子进程已退出，读到 EOF 为止） */
    for (;;) {
        char tmp[1024];
        ssize_t got = read(pfd[0], tmp, sizeof tmp);
        if (got <= 0) break;
        if (used + (size_t)got < sizeof g_act.last_out) {
            memcpy(g_act.last_out + used, tmp, (size_t)got);
            used += (size_t)got;
        }
    }
    close(pfd[0]);
    g_act.last_out[used < sizeof g_act.last_out ? used : sizeof g_act.last_out - 1] = 0;

    int rc;
    if (timed_out)                    rc = ACTION_TIMEOUT_EXIT;
    else if (WIFEXITED(status))       rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))     rc = -WTERMSIG(status);
    else                              rc = -1;

    snprintf(g_act.last_cmd, sizeof g_act.last_cmd, "%s", line ? line : "");
    g_act.last_ms = action_now_ms() - t0;
    g_act.last_rc = rc;
    g_act.n_run++;
    if (timed_out) {
        g_act.n_timeout++;
        snprintf(g_act.last_reason, sizeof g_act.last_reason,
                 "超过 %ld ms 没结束 → SIGKILL（退出码约定 %d）", tmo, ACTION_TIMEOUT_EXIT);
    } else if (rc != 0) {
        g_act.n_failed++;
        snprintf(g_act.last_reason, sizeof g_act.last_reason, "%s 退出码 %d（非零 = 观测，不是崩溃）", argv[0], rc);
    } else {
        g_act.n_ok++;
    }
    (void)action_observe(ai, rc);
    return rc;
}

/* 往 `_t/work/` 里写一个文本文件。返回 0 / -1（失败）/ -2（被拒）。 */
static inline int action_write_file(AI *ai, const char *rel, const char *text) {
    if (!ai) return -1;
    g_act.last_reason[0] = 0; g_act.last_out[0] = 0; g_act.last_mark[0] = 0;
    g_act.last_rc = -1; g_act.last_ms = 0;
    if (!ai->act_on)
        return action_refuse(rel, "这一层**默认关**（::act on 打开 / AIP_ACT=1）");
    char why[192];
    if (action_path_check(rel, why, sizeof why) != 0) return action_refuse(rel, why);
    if (action_ensure_workdir() != 0) return action_refuse(rel, "工作目录建不出来（" ACTION_WORK_DIR "）");

    char path[ACTION_PATH_CAP + 48];
    snprintf(path, sizeof path, "%s/%s", ACTION_WORK_DIR, rel);
    /* 父目录：逐个建（不走 shell） */
    {
        char dir[sizeof path];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) { *slash = 0; if (action_mkdir_p(dir) != 0) return action_refuse(rel, "建不出父目录"); }
    }
    const char *body = text ? text : "";
    size_t want = strlen(body);
    long t0 = action_now_ms();
    FILE *f = fopen(path, "wb");
    if (!f) {
        g_act.n_write++; g_act.n_write_failed++;
        g_act.last_rc = -1;
        /* 理由里只放**有界的**短路径：小缓冲别用 snprintf("%s") 收长名字
         * （`-Werror=format-truncation` 会拦，这条坑项目里踩过）。 */
        char short_path[64];
        { const char *q = path; size_t k = 0; while (*q && k + 1 < sizeof short_path) short_path[k++] = *q++; short_path[k] = 0; }
        snprintf(g_act.last_reason, sizeof g_act.last_reason, "写不开 %s：%s", short_path, strerror(errno));
        snprintf(g_act.last_cmd, sizeof g_act.last_cmd, "write %s", rel);
        return -1;
    }
    size_t wr = fwrite(body, 1, want, f);
    int bad = (fclose(f) != 0);
    g_act.n_write++;
    g_act.last_ms = action_now_ms() - t0;
    snprintf(g_act.last_cmd, sizeof g_act.last_cmd, "write %s（%zu 字节）", rel, want);
    if (wr != want || bad) {
        g_act.n_write_failed++; g_act.last_rc = -1;
        snprintf(g_act.last_reason, sizeof g_act.last_reason, "只写了 %zu/%zu 字节", wr, want);
    } else {
        g_act.last_rc = 0;
    }
    (void)action_observe(ai, g_act.last_rc);
    return g_act.last_rc;
}

/* 池子里有几个 `[运行:…]` 观测标记（看护用：钉住"输出变成了观测"） */
static inline size_t action_marks(const AI *ai) {
    if (!ai || !ai->pool) return 0;
    size_t n = 0;
    for (size_t i = 0; i < ai->pool->count; ++i) {
        const Token *t = ai->pool->tokens[i];
        if (t && !t->is_garbage && t->name &&
            strncmp(t->name, MICRO_RUN_TOKEN_PREFIX, strlen(MICRO_RUN_TOKEN_PREFIX)) == 0) n++;
    }
    return n;
}

/* `::act` 的状态面板（理由、账、白名单都在这里 —— 别处不许再拼一份） */
static inline void action_print(const AI *ai) {
    ActionBook *b = &g_act;
    printf("[外部执行 + 写文件 = %s]（%s）\n",
           (ai && ai->act_on) ? "开" : "关（默认）",
           (ai && ai->act_on) ? "改行为前先 A/B" : "没打开就绝不动外部世界；::act on / AIP_ACT=1 打开");
    printf("  边界：**不用 shell**（fork/exec + argv；`;|&` 只是普通参数）｜子进程 cwd = %s"
           "｜不许绝对路径 / 不许 `..` / 不许 `@响应文件` / 开关参数不许夹带路径｜stdin = /dev/null｜不联网\n", ACTION_WORK_DIR);
    printf("  白名单（首词精确匹配；::act timeout <ms> 覆盖超时，0 = 用表里的）：\n");
    for (size_t i = 0; i < ACTION_ALLOW_N; ++i) {
        long t = (g_act_timeout_ms > 0) ? g_act_timeout_ms : ACTION_ALLOW[i].timeout_ms;
        printf("    %-5s 超时 %5ld ms  %s\n", ACTION_ALLOW[i].cmd, t, ACTION_ALLOW[i].why);
    }
    printf("  账：跑 %zu（成功 %zu / 非零退出 %zu / 超时 %zu）｜拒 %zu｜"
           "写文件 %zu（失败 %zu）｜观测标记 %zu 个\n",
           b->n_run, b->n_ok, b->n_failed, b->n_timeout, b->n_refused,
           b->n_write, b->n_write_failed, action_marks(ai));
    if (b->last_cmd[0])
        printf("  最近一次：%s → rc=%d（%ld ms）\n", b->last_cmd, b->last_rc, b->last_ms);
    if (b->last_reason[0]) printf("  理由：%s\n", b->last_reason);
    if (b->last_mark[0])   printf("  落图：%s（观测标记；不是知识 —— 不建关系边、不进关系表）\n", b->last_mark);
    if (b->last_out[0]) {
        printf("  输出（最多留 %d 字节）：\n", ACTION_OUT_CAP);
        for (const char *q = b->last_out; *q; ) {
            const char *nl = strchr(q, '\n');
            size_t len = nl ? (size_t)(nl - q) : strlen(q);
            printf("    %.*s\n", (int)len, q);
            if (!nl) break;
            q = nl + 1;
        }
    } else {
        printf("  输出：（还没有）\n");
    }
}

#endif /* ACTION_H */
