/* train.c — 训练语料生成器
 *
 * 编译：cc -O2 -o train train.c
 * 运行：./train > seed.txt
 *
 * 生成 4000+ 句原语序列。不依赖任何头文件。
 */
#include <stdio.h>

static const char *Nouns[] = {
    "猫", "狗", "老鼠", "鸟", "鱼", "人", "小孩", "大人", "男人", "女人",
    "老师", "学生", "朋友", "家人", "陌生人", "太阳", "月亮", "星星",
    "天", "地", "云", "雨", "雪", "风", "树", "花", "草", "山", "河", "海",
    "水", "火", "空气", "食物", "房子", "车", "书", "笔", "桌子", "椅子",
};
#define N_NOUNS (sizeof Nouns / sizeof Nouns[0])

static const char *Verbs[] = {
    "追", "跑", "跳", "走", "吃", "喝", "看", "听", "说", "想",
    "爱", "怕", "知道", "认识", "帮", "找", "学", "教", "玩", "做",
};
#define N_VERBS (sizeof Verbs / sizeof Verbs[0])

static const char *Adjs[] = {
    "好", "坏", "大", "小", "高", "矮", "快", "慢", "新", "旧",
    "冷", "热", "美", "丑", "强", "弱", "亮", "暗", "软", "硬",
};
#define N_ADJS (sizeof Adjs / sizeof Adjs[0])

static const char *Polite[] = {
    "你好", "再见", "谢谢", "请", "对不起",
    "没关系", "是的", "不是", "可能", "一定",
};
#define N_POLITE (sizeof Polite / sizeof Polite[0])

static int count = 0;

static void pat_is(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i + j) % 5 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] %s [原语:连接]\n", Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_belongs(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i * j) % 7 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] %s [原语:对象] 属于 [原语:连接]\n",
                    Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_has(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_ADJS; ++j) {
            if ((i + j) % 3 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] %s [原语:好]\n", Nouns[i], Adjs[j]);
            count++;
        }
}
static void pat_can(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_VERBS; ++j) {
            if ((i + j) % 3 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] %s [原语:对象] 会 [原语:连接]\n",
                    Nouns[i], Verbs[j]);
            count++;
        }
}
static void pat_do(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_VERBS; ++j) {
            if ((i + j) % 5 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] 做 [原语:对象] %s [原语:连接]\n",
                    Nouns[i], Verbs[j]);
            count++;
        }
}
static void pat_prefer(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i) {
        fprintf(f, "[原语:对象] 我 [原语:对象] %s [原语:对象] 喜欢 [原语:连接]\n", Nouns[i]);
        count++;
        fprintf(f, "[原语:对象] 我 [原语:对象] %s [原语:对象] 不喜欢 [原语:连接]\n", Nouns[i]);
        count++;
    }
}
static void pat_know(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i) {
        fprintf(f, "[原语:对象] 我 [原语:对象] %s [原语:对象] 知道 [原语:连接]\n", Nouns[i]);
        count++;
        fprintf(f, "[原语:对象] 我 [原语:对象] %s [原语:对象] 不知道 "
                   "[原语:对象] ？ [原语:请求]\n", Nouns[i]);
        count++;
    }
    fprintf(f, "[原语:对象] 我 [原语:对象] 不认识 [原语:对象] ？ [原语:请求]\n"); count++;
    fprintf(f, "[原语:对象] 我 [原语:对象] 不懂 [原语:对象] ？ [原语:请求]\n"); count++;
    fprintf(f, "[原语:对象] 什么 [原语:对象] 意思 [原语:请求]\n"); count++;
    fprintf(f, "[原语:对象] 请 [原语:对象] 你 [原语:对象] 告诉 [原语:对象] 我 [原语:请求]\n"); count++;
    fprintf(f, "[原语:对象] 我 [原语:对象] 想知道 [原语:对象] ？ [原语:请求]\n"); count++;
}
static void pat_questions(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i) {
        fprintf(f, "[原语:对象] %s [原语:对象] 是什么 [原语:请求]\n", Nouns[i]); count++;
        fprintf(f, "[原语:对象] %s [原语:对象] 在哪里 [原语:请求]\n", Nouns[i]); count++;
        fprintf(f, "[原语:对象] %s [原语:对象] 是谁 [原语:请求]\n", Nouns[i]); count++;
    }
}
static void pat_whyhow(FILE *f) {
    for (size_t i = 0; i < N_VERBS; ++i) {
        fprintf(f, "[原语:对象] 为什么 [原语:对象] %s [原语:请求]\n", Verbs[i]); count++;
        fprintf(f, "[原语:对象] 如何 [原语:对象] %s [原语:请求]\n", Verbs[i]); count++;
    }
}
static void pat_because(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i * j) % 6 != 0) continue;
            fprintf(f, "[原语:对象] 因为 [原语:对象] %s [原语:对象] 所以 "
                       "[原语:对象] %s [原语:连接]\n", Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_before(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i + j) % 7 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] 在 [原语:对象] %s "
                       "[原语:对象] 之前 [原语:连接]\n", Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_polite(FILE *f) {
    for (size_t i = 0; i < N_POLITE; ++i) {
        fprintf(f, "[原语:对象] %s [原语:分割]\n", Polite[i]); count++;
    }
    for (size_t i = 0; i < N_POLITE; ++i)
        for (size_t j = 0; j < N_POLITE; ++j) {
            if (i == j || (i + j) % 3 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] %s [原语:分割]\n", Polite[i], Polite[j]);
            count++;
        }
}
static void pat_please(FILE *f) {
    for (size_t i = 0; i < N_VERBS; ++i) {
        fprintf(f, "[原语:对象] 请 [原语:对象] 你 [原语:对象] %s [原语:请求]\n", Verbs[i]);
        count++;
    }
    for (size_t i = 0; i < N_NOUNS; ++i) {
        fprintf(f, "[原语:对象] 我 [原语:对象] 想 [原语:对象] %s [原语:连接]\n", Nouns[i]);
        count++;
    }
}
static void pat_if(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i + j) % 8 != 0) continue;
            fprintf(f, "[原语:对象] 如果 [原语:对象] %s [原语:对象] 那么 "
                       "[原语:对象] %s [原语:连接]\n", Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_like(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i * j) % 9 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] 像 [原语:对象] %s [原语:连接]\n",
                    Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_category(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_NOUNS; ++j) {
            if (i == j || (i + 2 * j) % 11 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] 是 [原语:对象] %s "
                       "[原语:对象] 一类 [原语:连接]\n", Nouns[i], Nouns[j]);
            count++;
        }
}
static void pat_opposite(FILE *f) {
    for (size_t i = 0; i < N_ADJS; ++i)
        for (size_t j = i + 1; j < N_ADJS; ++j) {
            if ((i + j) % 4 != 0) continue;
            fprintf(f, "[原语:对象] %s [原语:对象] 反义 [原语:对象] %s [原语:连接]\n",
                    Adjs[i], Adjs[j]);
            count++;
        }
}
static void pat_tense(FILE *f) {
    for (size_t i = 0; i < N_VERBS; ++i) {
        fprintf(f, "[原语:对象] 我 [原语:对象] 之前 [原语:对象] %s [原语:连接]\n", Verbs[i]); count++;
        fprintf(f, "[原语:对象] 我 [原语:对象] 现在 [原语:对象] %s [原语:连接]\n", Verbs[i]); count++;
        fprintf(f, "[原语:对象] 我 [原语:对象] 将来 [原语:对象] %s [原语:连接]\n", Verbs[i]); count++;
    }
}
static void pat_perspective(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i) {
        fprintf(f, "[原语:对象] 我 [原语:对象] 认为 [原语:对象] %s "
                   "[原语:对象] 好 [原语:认为]\n", Nouns[i]);
        count++;
        fprintf(f, "[原语:对象] 你 [原语:对象] 认为 [原语:对象] %s "
                   "[原语:对象] 好 [原语:认为]\n", Nouns[i]);
        count++;
    }
}
static void pat_svo(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i)
        for (size_t j = 0; j < N_VERBS; ++j)
            for (size_t k = 0; k < N_NOUNS; ++k) {
                if (i == k || (i + j + k) % 23 != 0) continue;
                fprintf(f, "[原语:对象] %s [原语:对象] %s [原语:对象] %s [原语:连接]\n",
                        Nouns[i], Verbs[j], Nouns[k]);
                count++;
            }
}
static void pat_qa(FILE *f) {
    for (size_t i = 0; i < N_NOUNS; ++i) {
        fprintf(f, "[原语:对象] %s [原语:对象] 是什么 [原语:请求] [原语:分割]\n", Nouns[i]);
        count++;
        fprintf(f, "[原语:对象] %s [原语:对象] 是 [原语:对象] 一种 "
                   "[原语:对象] 事物 [原语:连接] [原语:分割]\n", Nouns[i]);
        count++;
    }
}

int main(void) {
    FILE *f = stdout;
    pat_is(f);
    pat_belongs(f);
    pat_has(f);
    pat_can(f);
    pat_do(f);
    pat_prefer(f);
    pat_know(f);
    pat_questions(f);
    pat_whyhow(f);
    pat_because(f);
    pat_before(f);
    pat_polite(f);
    pat_please(f);
    pat_if(f);
    pat_like(f);
    pat_category(f);
    pat_opposite(f);
    pat_tense(f);
    pat_perspective(f);
    pat_svo(f);
    pat_qa(f);
    fprintf(stderr, "已生成 %d 句\n", count);
    return 0;
}