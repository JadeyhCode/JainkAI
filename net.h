/* net.h — **联网检索**（窄门：默认关 / 域名白名单 / 硬上限 / 每一次都记账；§6.680）
 *
 * 为什么这样设计（用户的边界：不许"随便就把网上说的当成自己的知识"）：
 *   ① **默认关**：不开 `::net on`（或 `AIP_NET=1`）就一步都不往外走 —— 和 `::act` 同一条纪律；
 *   ② **不用 shell**：`fork` + `execv` + argv 数组（`;` `|` `$()` 只是普通字符）；
 *   ③ **域名白名单**（`net/allow.txt`，缺省 `export.arxiv.org` / `arxiv.org`），
 *      主机名**精确匹配**（不认 `*.arxiv.org`：`evil.arxiv.org` 也必须过不了）；
 *   ④ **只走 https**（`--proto =https --proto-redir =https`），**不跟重定向**：
 *      重定向是"别人带你去别处"——那一步不在白名单的语义里（要跟就明写网址）；
 *   ⑤ **硬上限**：每次 ≤ 2 MB、≤ 15 s、每次进程 ≤ 8 次抓取；到点 `SIGKILL` 子进程；
 *   ⑥ **账**：每次抓取（成功/被拒/失败）都往 `papers/cache/_net.log` 追加一行
 *      （时间、主机、字节、退出码、理由）—— 这是"我到底看了什么"的唯一凭据；
 *   ⑦ 抓回来的东西**先进 `papers/cache/`，再走 `paper.h` 的"抽断言 ⇒ 待证"**：
 *      网上的一律算**待证**（来源 `网络`/`论文`），**绝不直接写事实边**。
 *
 * 明确不做：HTML 正文抽取（`::net get` 只处理 PDF 与纯文本）、robots.txt 抓取、
 * 并发抓取、站点地图、反爬/登录、Cookie、代理设置、断点续传；不做"搜索引擎"（只用 arXiv 的公开 API）。
 */
#ifndef NET_H
#define NET_H

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

#define NET_MAX_BYTES  (2u * 1024u * 1024u)   /* 单次抓取上限 */
#define NET_TIMEOUT_S  15                     /* 单次超时（curl --max-time 同值） */
#define NET_MAX_FETCH  8                      /* 每次进程最多抓几次 */
#define NET_HOSTS_MAX  24
#define NET_HOST_LEN   96
#define NET_RESULT_MAX 5
#define NET_CACHE_DIR  "papers/cache"
#define NET_AUDIT      "papers/cache/_net.log"
#define NET_ALLOW_FILE "net/allow.txt"
#define NET_PART       "papers/cache/.netpart"

typedef struct {
    int on;                                   /* 默认关 */
    char host[NET_HOSTS_MAX][NET_HOST_LEN];
    size_t nhost;
    int n_fetch, n_ok, n_fail, n_refused;     /* 账（进程级） */
    size_t bytes;
    char last_reason[192];
    /* 最近一次检索的结果（`::net get N` 用） */
    char hit_id[NET_RESULT_MAX][64];
    char hit_title[NET_RESULT_MAX][160];
    int n_hit;
} NetBook;

static NetBook g_net = { 0, {{0}}, 0, 0, 0, 0, 0, 0, "", {{0}}, {{0}}, 0 };

static inline long net_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static inline void net_reason(const char *why) {
    snprintf(g_net.last_reason, sizeof g_net.last_reason, "%s", why ? why : "");
}

/* 缺省白名单（没有 net/allow.txt 时用）：arXiv 的 API 与正文 */
static inline void net_defaults(void) {
    if (g_net.nhost) return;
    snprintf(g_net.host[0], NET_HOST_LEN, "export.arxiv.org");
    snprintf(g_net.host[1], NET_HOST_LEN, "arxiv.org");
    g_net.nhost = 2;
}

/* 读 net/allow.txt（一行一个主机名，`#` 注释、空行忽略）。读不到就用缺省。 */
static inline void net_load_allow(void) {
    g_net.nhost = 0;
    FILE *f = fopen(NET_ALLOW_FILE, "rb");
    if (!f) { net_defaults(); return; }
    char line[128];
    while (fgets(line, sizeof line, f) && g_net.nhost < NET_HOSTS_MAX) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        snprintf(g_net.host[g_net.nhost], NET_HOST_LEN, "%s", line);
        g_net.nhost++;
    }
    fclose(f);
    if (g_net.nhost == 0) net_defaults();      /* 文件是空的 ⇒ 别变成"黑名单" */
}

static inline int net_host_ok(const char *host) {
    if (!host || !host[0]) return 0;
    if (g_net.nhost == 0) net_load_allow();
    for (size_t i = 0; i < g_net.nhost; ++i) {
        size_t a = strlen(g_net.host[i]), b = strlen(host);
        if (a != b) continue;
        size_t k = 0;
        while (k < a) {
            unsigned char x = (unsigned char)g_net.host[i][k], y = (unsigned char)host[k];
            if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
            if (x != y) break;
            k++;
        }
        if (k == a) return 1;
    }
    return 0;
}

/* 只认 `https://host/path...`：http/其它协议、带 `@`（userinfo）、端口、裸主机 一律拒 */
static inline int net_parse_url(const char *url, char *host, size_t hcap,
                                char *rest, size_t rcap, char *why, size_t wcap) {
    host[0] = 0;
    rest[0] = 0;
    if (!url || strncmp(url, "https://", 8) != 0) {
        snprintf(why, wcap, "只认 https:// 开头的网址（别的协议一律不走）");
        return -1;
    }
    const char *p = url + 8;
    const char *hs = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') {
        if (*p == '@' || *p == ':') { snprintf(why, wcap, "网址里不许带用户名/端口"); return -1; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) {
            snprintf(why, wcap, "主机名里有不认的字符 `%c`", *p);
            return -1;
        }
        p++;
    }
    size_t hl = (size_t)(p - hs);
    if (hl == 0 || hl + 1 > hcap) { snprintf(why, wcap, "主机名空或太长"); return -1; }
    memcpy(host, hs, hl);
    host[hl] = 0;
    if (!strchr(host, '.')) { snprintf(why, wcap, "主机名里没有点（`%s` 不是完整域名）", host); return -1; }
    snprintf(rest, rcap, "%s", *p ? p : "/");
    return 0;
}

/* 找 curl（只认绝对路径，别让 PATH 玩花样） */
static inline const char *net_curl(void) {
    static const char *cand[] = { "/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", NULL };
    for (int i = 0; cand[i]; ++i) if (access(cand[i], X_OK) == 0) return cand[i];
    return NULL;
}

static inline void net_audit(const char *verb, const char *what, long nbytes, int rc, const char *why) {
    mkdir(NET_CACHE_DIR, 0755);
    FILE *f = fopen(NET_AUDIT, "ab");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d\t%s\t%.120s\t%ld\t%d\t%.120s\n",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, verb, what ? what : "", nbytes, rc,
            why ? why : "");
    fclose(f);
}

/* 抓一条 URL（不跟重定向）。成功：*bufp = malloc 的正文（调用方 free），*np = 字节数，返回 0。
 * 失败返回 -1，理由在 g_net.last_reason。 */
static inline int net_fetch(const char *url, unsigned char **bufp, size_t *np) {
    if (bufp) *bufp = NULL;
    if (np) *np = 0;
    if (!g_net.on) { net_reason("联网这一层默认关（::net on 打开）"); return -1; }
    if (g_net.n_fetch >= NET_MAX_FETCH) {
        net_reason("这次会话已经抓满上限了（8 次）—— 想再抓重开一个会话");
        return -1;
    }
    char host[NET_HOST_LEN], rest[512], why[128];
    if (net_parse_url(url, host, sizeof host, rest, sizeof rest, why, sizeof why) != 0) {
        g_net.n_refused++;
        net_reason(why);
        net_audit("REFUSE", url, 0, -1, why);
        return -1;
    }
    if (!net_host_ok(host)) {
        g_net.n_refused++;
        snprintf(why, sizeof why, "白名单里没有主机 `%s`（改 net/allow.txt）", host);
        net_reason(why);
        net_audit("REFUSE", url, 0, -1, why);
        return -1;
    }
    const char *curl = net_curl();
    if (!curl) { net_reason("本机没有 curl（/usr/bin/curl 找不到）"); net_audit("FAIL", url, 0, -1, "no curl"); return -1; }
    mkdir(NET_CACHE_DIR, 0755);
    unlink(NET_PART);
    g_net.n_fetch++;
    int pfd[2];
    if (pipe(pfd) != 0) { net_reason("pipe() 失败"); return -1; }
    long t0 = net_now_ms();
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); net_reason("fork() 失败"); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { (void)dup2(dn, 0); close(dn); }
        (void)dup2(pfd[1], 1);
        (void)dup2(pfd[1], 2);                 /* 诊断和写在一行里（见下面 NETW 标记） */
        close(pfd[0]); close(pfd[1]);
        char maxt[16], maxb[24];
        snprintf(maxt, sizeof maxt, "%d", NET_TIMEOUT_S);
        snprintf(maxb, sizeof maxb, "%u", NET_MAX_BYTES);
        char *av[24];
        int n = 0;
        av[n++] = (char *)curl;
        av[n++] = "-sS"; av[n++] = "-q"; av[n++] = "-f";
        av[n++] = "--proto"; av[n++] = "=https";
        av[n++] = "--proto-redir"; av[n++] = "=https";
        av[n++] = "--max-time"; av[n++] = maxt;
        av[n++] = "--max-filesize"; av[n++] = maxb;
        av[n++] = "--max-redirs"; av[n++] = "0";
        av[n++] = "-A"; av[n++] = "aip-kernel/1.0 (local experiment)";
        av[n++] = "-o"; av[n++] = NET_PART;
        av[n++] = "-w"; av[n++] = "\nNETW %{http_code} %{size_download} %{url_effective}\n";
        av[n++] = (char *)url;
        av[n] = NULL;
        execv(curl, av);
        _exit(127);
    }
    close(pfd[1]);
    size_t used = 0;
    char diag[2048] = {0};
    int reaped = 0, status = 0, killed = 0;
    while (!reaped) {
        if (!killed && net_now_ms() - t0 > (long)NET_TIMEOUT_S * 1000L + 2000L) {
            killed = 1;
            (void)kill(pid, SIGKILL);
        }
        struct pollfd p = { pfd[0], POLLIN, 0 };
        (void)poll(&p, 1, 20);
        for (;;) {
            char tmp[512];
            ssize_t got = read(pfd[0], tmp, sizeof tmp);
            if (got <= 0) break;
            if (used + (size_t)got < sizeof diag - 1) {
                memcpy(diag + used, tmp, (size_t)got);
                used += (size_t)got;
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) reaped = 1;
    }
    close(pfd[0]);
    diag[used < sizeof diag ? used : sizeof diag - 1] = 0;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -9;

    /* 从诊断里挑出 NETW 那一行（http 码 / 字节数 / 最终网址） */
    long code = 0, size = 0;
    char eff[512] = {0};
    const char *mark = strstr(diag, "NETW ");
    if (mark && sscanf(mark, "NETW %ld %ld %511s", &code, &size, eff) == 3) {
        /* ok */
    } else {
        mark = NULL;
    }
    if (rc != 0 || !mark || code != 200) {
        g_net.n_fail++;
        const char *why2 = mark ? "HTTP 不是 200" : "curl 没跑成";
        snprintf(g_net.last_reason, sizeof g_net.last_reason, "%s（curl rc=%d，http=%ld）%.120s",
                 why2, rc, code, diag[0] ? diag : "");
        net_audit("FAIL", url, size, rc, g_net.last_reason);
        unlink(NET_PART);
        return -1;
    }
    /* 重定向一律没跟（--max-redirs 0），但**还是核一遍**最终网址的主机：
     * 万一哪天 curl 的行为变了，这里能挡住"被带去别处"。 */
    if (eff[0]) {
        char eh[NET_HOST_LEN], er[512], ew[128];
        if (net_parse_url(eff, eh, sizeof eh, er, sizeof er, ew, sizeof ew) != 0 || !net_host_ok(eh)) {
            g_net.n_refused++;
            snprintf(g_net.last_reason, sizeof g_net.last_reason, "最终网址跑出白名单了（`%s`）", eff);
            net_audit("REFUSE", eff, size, rc, g_net.last_reason);
            unlink(NET_PART);
            return -1;
        }
    }
    if (size < 0 || (size_t)size > NET_MAX_BYTES) {
        g_net.n_fail++;
        snprintf(g_net.last_reason, sizeof g_net.last_reason, "正文 %ld 字节，超过 %u 上限", size, NET_MAX_BYTES);
        net_audit("FAIL", url, size, rc, g_net.last_reason);
        unlink(NET_PART);
        return -1;
    }
    struct stat st;
    if (stat(NET_PART, &st) != 0 || st.st_size <= 0) {
        g_net.n_fail++;
        net_reason("抓回来是空的（0 字节）");
        net_audit("FAIL", url, 0, rc, "empty");
        unlink(NET_PART);
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    if (sz > NET_MAX_BYTES) sz = NET_MAX_BYTES;
    unsigned char *buf = (unsigned char *)malloc(sz + 1);
    if (!buf) { net_reason("内存不够"); net_audit("FAIL", url, 0, rc, "nomem"); unlink(NET_PART); return -1; }
    FILE *f = fopen(NET_PART, "rb");
    if (!f) { free(buf); net_reason("打不开临时文件"); unlink(NET_PART); return -1; }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    unlink(NET_PART);
    buf[got] = 0;
    g_net.n_ok++;
    g_net.bytes += got;
    net_audit("OK", url, (long)got, 0, "");
    if (bufp) *bufp = buf; else free(buf);
    if (np) *np = got;
    return 0;
}

/* 把一段文字做 percent-encode（只留 `A-Za-z0-9-_.~`），给查询串用 */
static inline void net_urlencode(const char *in, char *out, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; p && *p; ++p) {
        unsigned char c = *p;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) {
            if (o + 1 < cap) out[o++] = (char)c;
        } else {
            if (o + 3 < cap) { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xF]; }
        }
    }
    out[o < cap ? o : cap - 1] = 0;
}

/* 把 HTML/XML 实体换成字符（标题里常见 `&amp;` `&#39;`） */
static inline void net_unescape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < cap;) {
        if (in[i] == '&') {
            if (!strncmp(in + i, "&amp;", 5)) { out[o++] = '&'; i += 5; continue; }
            if (!strncmp(in + i, "&lt;", 4)) { out[o++] = '<'; i += 4; continue; }
            if (!strncmp(in + i, "&gt;", 4)) { out[o++] = '>'; i += 4; continue; }
            if (!strncmp(in + i, "&quot;", 6)) { out[o++] = '"'; i += 6; continue; }
            if (!strncmp(in + i, "&#39;", 5)) { out[o++] = '\''; i += 5; continue; }
            if (!strncmp(in + i, "&apos;", 6)) { out[o++] = '\''; i += 6; continue; }
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

/* 把空白压成单空格（arXiv 的 title 是多行缩进的） */
static inline void net_collapse(const char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    int sp = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = (o > 0); continue; }
        if (sp) { out[o++] = ' '; sp = 0; }
        out[o++] = (char)c;
    }
    out[o] = 0;
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
}

/* 在 arXiv 的公开 API 上检索（没有 key、没有爬虫）：结果放进 g_net.hit_*。
 * 返回条目数（0 = 没搜到/没抓成，理由在 last_reason）。 */
static inline int net_search_arxiv(const char *query) {
    g_net.n_hit = 0;
    char q[256];
    net_urlencode(query, q, sizeof q);
    if (!q[0]) { net_reason("查询串是空的"); return 0; }
    char url[512];
    snprintf(url, sizeof url,
             "https://export.arxiv.org/api/query?search_query=all:%s&start=0&max_results=%d"
             "&sortBy=relevance", q, NET_RESULT_MAX);
    unsigned char *buf = NULL;
    size_t n = 0;
    if (net_fetch(url, &buf, &n) != 0) return 0;
    /* Atom：<entry> ... <id>http://arxiv.org/abs/XXXX</id> ... <title>...</title> ... </entry> */
    const char *p = (const char *)buf, *end = (const char *)buf + n;
    int got = 0;
    while (got < NET_RESULT_MAX) {
        const char *e = strstr(p, "<entry>");
        if (!e || e >= end) break;
        const char *ee = strstr(e, "</entry>");
        if (!ee || ee > end) break;
        const char *id = strstr(e, "<id>");
        const char *ti = strstr(e, "<title>");
        if (id && id < ee && ti && ti < ee) {
            const char *id0 = id + 4, *id1 = strstr(id0, "</id>");
            const char *t0 = ti + 7, *t1 = strstr(t0, "</title>");
            if (id1 && id1 <= ee && t1 && t1 <= ee) {
                char rawid[128] = {0};
                size_t il = (size_t)(id1 - id0);
                if (il > sizeof rawid - 1) il = sizeof rawid - 1;
                memcpy(rawid, id0, il);
                rawid[il] = 0;
                const char *slash = strrchr(rawid, '/');
                const char *aid = slash ? slash + 1 : rawid;
                char title[512] = {0}, clean[200] = {0};
                net_collapse(t0, (size_t)(t1 - t0), title, sizeof title);
                net_unescape(title, clean, sizeof clean);
                snprintf(g_net.hit_id[got], sizeof g_net.hit_id[0], "%s", aid);
                snprintf(g_net.hit_title[got], sizeof g_net.hit_title[0], "%s", clean);
                got++;
            }
        }
        p = ee + 8;
    }
    g_net.n_hit = got;
    if (got == 0) {
        net_reason("arXiv 的 API 返回里没有 <entry>（可能没搜到，也可能格式变了 —— 我不猜）");
    }
    free(buf);
    return got;
}

/* `::net get` 的参数：`1`..`5` 用上次检索的第 N 条；否则必须是完整 https 网址。
 * 返回 0 = 解出来了（url 里是最终网址，名字里是建议的文件名）。 */
static inline int net_resolve_target(const char *arg, char *url, size_t ucap, char *name, size_t ncap) {
    if (!arg || !arg[0]) return -1;
    url[0] = 0;
    name[0] = 0;
    if (arg[0] >= '1' && arg[0] <= '9' && arg[1] == 0) {
        int k = arg[0] - '1';
        if (k >= g_net.n_hit) return -1;
        snprintf(url, ucap, "https://arxiv.org/pdf/%s", g_net.hit_id[k]);
        snprintf(name, ncap, "%s.pdf", g_net.hit_id[k]);
        return 0;
    }
    /* 不是网址（连 `://` 都没有）就当参数错；`http://` 之类照样往下传，
     * 让 net_fetch 用它那句精确的理由拒（明说「只认 https」，而不是含糊的用法提示）。 */
    if (!strstr(arg, "://")) return -1;
    snprintf(url, ucap, "%s", arg);
    /* 文件名：网址最后一段，只留 `[A-Za-z0-9._-]`，其余换成 `_` */
    const char *slash = strrchr(arg, '/');
    const char *base = slash ? slash + 1 : arg;
    size_t o = 0;
    for (const char *p = base; *p && o + 1 < ncap; ++p) {
        char c = *p;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        name[o++] = keep ? c : '_';
    }
    name[o] = 0;
    if (!name[0] || !strcmp(name, "_")) snprintf(name, ncap, "net_download.bin");
    return 0;
}

#endif /* NET_H */
