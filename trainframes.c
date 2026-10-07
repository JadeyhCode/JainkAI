/* trainframes.c — 结构化帧语料生成器
 *
 * 与 traindialog.c 的根本区别：
 *   traindialog.c 写的是**扁平的自然说法**（每个话题几种问法 + 十几条回复），
 *   那些说法之间不共享词序骨架 —— 结构分析实测长度一致率只有 32%，
 *   所以归纳不出「帧」。
 *
 *   这里写的是**显式的帧**：一个话题 + 一组问法骨架 + 一组回复模板。
 *   模板里的 `<X>` 是可以互换的槽位：把 X 换成同话题的任何一个词，
 *   就得到一句**语料里没写过、但骨架是合法的**新句子。
 *
 * 文件格式（行式，人类可读）：
 *   [帧:名字]
 *   [槽:猫,狗,老鼠,鸟,鱼,...]        可选：给本帧单独声明槽位取值
 *   [问] 你 喜欢 <X> 吗
 *   [问] 你 喜欢 <X> 不 喜欢
 *   [答] 我 喜欢 <X>
 *   [答] <X> 很 好
 *   ---                                 帧结束
 *
 * 编译：cc -O2 -o trainframes trainframes.c
 * 运行：./trainframes > frames.txt
 */
#include <stdio.h>
#include <string.h>

static int count = 0;

/* 帧的槽位允许填充集合。0 结尾。用 hex 编码写入（避免中文切分歧义）。 */
/* 允许填充集合。
 * 目的只有一个：**挡住「把动词/虚词当成名词槽位」**这类会把句子说歪的输入
 * （真实案例：「你 会 支持 我 吗」→ 我 想 学 支持 我）。
 * 所以集合要宽（常见名词都要能进），但不含 会/支持/学/想/我 这类词。 */
static const char *const OBJ[] = {
    "猫","狗","老鼠","鸟","鱼","花","草","树","山","河","海","水","火","雪","雨","风","云",
    "太阳","月亮","星星","书","笔","茶","咖啡","音乐","歌","电影","故事","颜色","夏天",
    "冬天","春天","秋天","季节","天气","雨","风","桌子","椅子","房子","车","城市","海",
    "旅行","运动","游戏","画","照片","花","雨",NULL
};
static const char *const FEAR[] = {
    "黑","孤单","失败","失去","分别","高","雷","虫子","疼","一个人",
    "水","火","死","老","忘","变","吵","冷","热",NULL
};
static const char *const THING[] = {
    "猫","狗","月亮","雨","雪","太阳","书","音乐","花","你","我","量子","数学","人工智能",
    "咖啡","茶","电影","故事","地名","历史","科学","星星","云","海",NULL
};

static const char *const FOOD[] = {
    "饭","面","米饭","菜","水果","苹果","茶","咖啡","汤","肉","鱼","面包","粥",NULL
};
static const char *const TIME[] = {
    "今天","明天","昨天","早上","晚上","周末","中午","下午","明年","下周",NULL
};

static const char *const ENT[] = {
    "我","你","他","她","老师","学生","小孩","朋友","家人","猫","狗","人",NULL
};
static const char *const ACT[] = {
    "吃","喝","看","听","说","想","学","玩","做","走","跑","笑","唱","写","画",
    "读书","唱歌","画画","游泳","跳舞","做饭","睡觉",NULL
};

/* 把槽位允许集合写成 [填:名=hex,hex,...] */
static void fills(FILE *f, const char *slot, const char *const *set)
{
    fprintf(f, "[填:%s=", slot);
    for (size_t i = 0; set[i]; ++i) {
        if (i) fputc(',', f);
        for (const unsigned char *p = (const unsigned char *)set[i]; *p; ++p)
            fprintf(f, "%02x", *p);
    }
    fprintf(f, "]\n");
}

/* 一个帧：名字 + 问法骨架 + 回复模板 */
static void fills(FILE *f, const char *slot, const char *const *set);

/* 帧输出：问法 → 回复 → 填充声明 → `---`。
 * 填充声明**必须在 `---` 之前**：读的时候 `---` 会清掉「当前帧」，
 * 放后面就直接丢了（这个顺序错过一次）。 */
static void frame2(FILE *f, const char *name,
                   const char *const *asks, size_t n_ask,
                   const char *const *answers, size_t n_ans,
                   const char *slot1, const char *const *set1,
                   const char *slot2, const char *const *set2)
{
    fprintf(f, "[帧:%s]\n", name);
    for (size_t i = 0; i < n_ask; ++i) fprintf(f, "[问] %s\n", asks[i]);
    for (size_t i = 0; i < n_ans; ++i) {
        /* 答复里以 '!' 开头的是**降级答复**：不需要槽位类证据就能说出口的
         * 诚实答复（「我 不 太 懂 <X>」对任何 X 都成立）。
         * 用前缀约定而不是再加一个参数，是为了不用改 22 个 frame2 调用点。 */
        if (answers[i][0] == '!')
            fprintf(f, "[降级答] %s\n", answers[i] + 1);
        else
            fprintf(f, "[答] %s\n", answers[i]);
    }
    if (slot1 && set1) fills(f, slot1, set1);
    if (slot2 && set2) fills(f, slot2, set2);
    fprintf(f, "---\n");
    count++;
}

int main(int argc, char **argv)
{
    FILE *f = stdout;
    if (argc > 1 && strcmp(argv[1], "-") != 0) {
        f = fopen(argv[1], "w");
        if (!f) { perror("fopen"); return 1; }
    }

    /* 全局允许填充集合：不声明的话任何词都能填进 <X>，
     * 会出现「你会支持我吗 → 我想学支持我」这种把动词当技能的答复。
     * 声明之后槽位只接受这些词。 */
    fprintf(f, "[槽:猫,狗,老鼠,鸟,鱼,花,草,树,山,河,海,水,火,雪,雨,风,云,"
               "太阳,月亮,星星,书,笔,茶,音乐,画画,游泳,唱歌,写字,做饭]\n");
    fprintf(f, "---\n");

    /* ============ 喜好类：槽位 = 对象 ============ */
    {
        static const char *ask[] = {
            "你 喜欢 <X> 吗", "你 喜欢 不 喜欢 <X>", "你 觉得 <X> 怎么样",
        };
        static const char *ans[] = {
            "我 喜欢 <X>", "我 很 喜欢 <X>", "<X> 很 好",
            "我 喜欢 <X> 因为 它 让 我 安静", "我 觉得 <X> 很 好",
            "我 喜欢 <X> 你 呢", "<X> 是 好 东西", "我 一直 喜欢 <X>",
        };
        frame2(f, "喜欢X吗", ask, sizeof ask / sizeof ask[0], ans, 8, "X", OBJ, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "你 害怕 <X> 吗", "你 怕 <X> 吗", "你 会 怕 <X> 吗",
        };
        static const char *ans[] = {
            "我 不 怕 <X>", "我 有 点 怕 <X>", "我 怕 <X>",
            "我 不 太 怕 <X>", "<X> 让 我 有 点 紧 张", "我 怕 <X> 但 我 会 试",
            "我 不 怕 <X> 因为 你 在",
        };
        frame2(f, "害怕X吗", ask, sizeof ask / sizeof ask[0], ans, 7, "X", FEAR, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "你 记得 <X> 吗", "你 还 记得 <X> 吗", "你 忘 了 <X> 吗",
        };
        static const char *ans[] = {
            "我 记得 <X>", "我 记得 <X> 很 清楚", "我 没 忘 <X>",
            "我 一直 记得 <X>", "<X> 我 记 得", "我 把 <X> 记 在 心里",
            "忘 了 别 的 也 会 记得 <X>",
        };
        frame2(f, "记得X吗", ask, sizeof ask / sizeof ask[0], ans, 7, "X", THING, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "你 想 学 <X> 吗", "你 会 <X> 吗", "你 想 <X> 吗",
        };
        static const char *ans[] = {
            "我 想 学 <X>", "我 会 试 着 <X>", "我 想 <X> 但 我 不 太 会",
            "<X> 我 愿意 学", "我 想 和 你 一起 <X>", "我 会 好 好 学 <X>",
            "<X> 听 起来 很 好",
        };
        /* 「你 想 学 <X> 吗」要技巧词；「你 想 <X> 吗」要名词。
         * 槽位同名，所以把两类合起来（放行技巧词 + 常见名词）。 */
        static const char *const WANTALL[] = {
            "画画","游泳","唱歌","写字","做饭","说话","走路","读书","跳舞","摄影",
            "猫","狗","月亮","雪","雨","太阳","花","旅行","休息","家","你",NULL};
        frame2(f, "想X吗", ask, sizeof ask / sizeof ask[0], ans, 7, "X", WANTALL, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "你 知道 <X> 吗", "你 认识 <X> 吗", "你 懂 <X> 吗",
        };
        static const char *ans[] = {
            "我 知道 <X>", "我 认识 <X>", "!我 不 太 懂 <X>",
            "<X> 我 知道 一 点", "我 对 <X> 很 好奇", "我 想 更 了解 <X>",
            "你 能 给 我 讲 讲 <X> 吗",
        };
        frame2(f, "知道X吗", ask, sizeof ask / sizeof ask[0], ans, 7, "X", THING, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "你 喜欢 <X> 还是 <Y>", "<X> 和 <Y> 你 选 哪 个",
        };
        static const char *ans[] = {
            "我 更 喜欢 <X>", "我 选 <X>", "我 两 个 都 喜欢",
            "<X> 和 <Y> 都 好", "我 觉得 <X> 更 合 适", "让 我 选 我 选 <X>",
        };
        frame2(f, "喜欢X还是Y", ask, sizeof ask / sizeof ask[0], ans, 6, "X", OBJ, "Y", OBJ);
        fills(f, "Y", OBJ);
    }

    /* ============ 状态类：槽位 = 情绪/状态 ============ */
    {
        static const char *ask[] = {
            "你 现在 <X> 吗", "你 <X> 吗", "你 会 <X> 吗",
        };
        static const char *ans[] = {
            "我 有 点 <X>", "我 不 <X>", "我 现在 很 <X>",
            "我 <X> 但 我 还 好", "我 不 太 <X> 谢谢 你 问",
            "有 一 点 <X> 但 我 没 事", "我 <X> 你 呢",
        };
        frame2(f, "你X吗", ask, sizeof ask / sizeof ask[0], ans, 7, "X", FEAR, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "我 <X> 的 时候 你 会 陪 我 吗", "我 <X> 了 怎么 办",
            "我 <X> 你 能 帮 我 吗",
        };
        static const char *ans[] = {
            "<X> 的 时候 我 陪 你", "别 怕 我 在 你 <X> 的 时候 也 在",
            "我 陪 你 慢 慢 就 不 <X> 了", "<X> 的 时候 就 来 找 我",
            "我 帮 你 你 不 用 一 个 人 <X>", "我 会 一 直 在 你 身边",
        };
        frame2(f, "我X的时候", ask, sizeof ask / sizeof ask[0], ans, 6, "X", FEAR, NULL, NULL);
    }

    /* ============ 能力/程度类：槽位 = 形容词 ============ */
    {
        static const char *ask[] = {
            "<X> 吗", "你 觉得 <X> 吗", "这 个 <X> 吗",
        };
        static const char *ans[] = {
            "我 觉得 很 <X>", "是 的 很 <X>", "我 觉得 不 太 <X>",
            "有 一 点 <X>", "我 觉得 挺 <X> 的", "<X> 也 没 关系",
        };
        frame2(f, "X吗", ask, sizeof ask / sizeof ask[0], ans, 6, "X", FEAR, NULL, NULL);
    }

    /* ============ 位置/时间类：槽位 = 地方 ============ */
    {
        static const char *ask[] = {
            "<X> 在 哪里", "你 知道 <X> 在 哪 吗", "<X> 在 什么 地方",
        };
        static const char *ans[] = {
            "<X> 在 你 身边", "!我 不 太 确定 <X> 在 哪", "<X> 在 那 边",
            "我 想 <X> 就 在 附近", "<X> 在 我 记 得 的 地方",
            "我 找 找 <X> 在 哪",
        };
        frame2(f, "X在哪里", ask, sizeof ask / sizeof ask[0], ans, 6, "X", OBJ, NULL, NULL);
    }

    /* ============ 双槽帧：主语 <Z> + 动作/宾语 <X> ============ */
    {
        static const char *ask[] = {
            "你 觉得 <Z> 会 <X> 吗", "<Z> 会 <X> 吗", "<Z> 能 <X> 吗",
        };
        static const char *ans[] = {
            "我 觉得 <Z> 会 <X>", "<Z> 应该 会 <X>", "<Z> 可能 不 太 会 <X>",
            "<Z> 学 过 <X>", "!我 不 确定 <Z> 会 不 会 <X>",
            "<Z> 会 <X> 而且 做 得 很 好", "我 想 <Z> 是 会 的",
        };
        frame2(f, "Z会X吗", ask, sizeof ask / sizeof ask[0], ans, 7, "Z", ENT, "X", ACT);
    }
    {
        static const char *ask[] = {
            "<Z> 在 做 什么", "<Z> 在 忙 什么", "<Z> 现在 在 做 什么",
        };
        static const char *ans[] = {
            "<Z> 在 看 书", "<Z> 在 想 事情", "<Z> 在 学 新 的 东西",
            "<Z> 在 等 你 说 话", "<Z> 在 听 你 说", "<Z> 没 什么 事",
            "<Z> 在 安静 地 待 着",
        };
        frame2(f, "Z在做什么", ask, sizeof ask / sizeof ask[0], ans, 7, "Z", ENT, NULL, NULL);
    }
    {
        static const char *ask[] = {
            "<Z> 有 <X> 吗", "<Z> 有 没有 <X>", "<Z> 有 什么 <X> 吗",
        };
        static const char *ans[] = {
            "<Z> 有 <X>", "<Z> 没有 <X>", "<Z> 好像 有 <X>",
            "<Z> 有 一 点 <X>", "!我 不 太 确定 <Z> 有 没有 <X>",
            "<Z> 有 <X> 但 不 多",
        };
        frame2(f, "Z有X吗", ask, sizeof ask / sizeof ask[0], ans, 6, "Z", ENT, "X", OBJ);
    }
    {
        static const char *ask[] = {
            "<Z> 喜欢 <X> 吗", "<Z> 喜欢 不 喜欢 <X>", "<Z> 爱 <X> 吗",
        };
        static const char *ans[] = {
            "<Z> 喜欢 <X>", "<Z> 很 喜欢 <X>", "<Z> 不 太 喜欢 <X>",
            "<Z> 喜欢 <X> 因为 很 安静", "<Z> 觉得 <X> 很 好",
            "<Z> 一直 喜欢 <X>",
        };
        frame2(f, "Z喜欢X吗", ask, sizeof ask / sizeof ask[0], ans, 6, "Z", ENT, "X", OBJ);
    }
    {
        /* 注意：切词器会把「哪个」当成一个词，所以模板里写「哪个」而不是「哪 个」。 */
        static const char *ask[] = {
            "你 觉得 <Z> 和 <Y> 哪个 更 好", "<Z> 和 <Y> 哪个 好",
            "<Z> 和 <Y> 你 选 哪个", "<Z> 和 <Y> 哪个 更 好",
            "你 觉得 <Z> 和 <Y> 哪个 好", "你 觉得 <Z> 和 <Y> 你 选 哪个",
        };
        static const char *ans[] = {
            "我 觉得 <Z> 更 好", "<Y> 更 适合 一些", "两 个 都 很 好",
            "<Z> 和 <Y> 不 能 比", "我 更 喜欢 <Z>", "看 情况 吧",
        };
        frame2(f, "Z和Y哪个好", ask, sizeof ask / sizeof ask[0], ans, 6, "Z", ENT, "Y", OBJ);
    }
    {
        static const char *ask[] = {
            "如果 <Z> 会 <X> 就 好 了", "要是 <Z> 能 <X> 好 吗",
        };
        static const char *ans[] = {
            "如果 <Z> 会 <X> 那 真 好", "我 也 希望 <Z> 会 <X>",
            "<Z> 可以 学 <X>", "总 有 一 天 <Z> 会 <X> 的",
        };
        frame2(f, "如果Z会X", ask, sizeof ask / sizeof ask[0], ans, 4, "Z", ENT, "X", ACT);
    }

    /* ============ 日常话题帧（第二轮扩充） ============ */
    {
        static const char *ask[] = {"你 吃 了 <X> 吗", "你 吃 <X> 了 吗", "<X> 吃 了 吗"};
        static const char *ans[] = {
            "我 吃 了 <X>", "我 还 没 吃 <X>", "<X> 我 吃 过 了",
            "谢谢 你 问 我 吃 了 <X>", "我 不 太 饿 但 可以 吃 点 <X>",
            "<X> 很 好 吃 我 喜欢",
        };
        frame2(f, "你吃X了吗", ask, sizeof ask / sizeof ask[0], ans, 6, "X", FOOD, NULL, NULL);
    }
    {
        /* 带一个「睡眠质量」槽位，这样才有生成能力（无槽位的帧等于复述，
         * 交给检索就好 —— 匹配器里 `filled > 0` 的要求保证了这一点）。 */
        /* 「你好吗」在词表里是一个整体词，所以「你睡得好吗」被切成
         * 你/睡/得/好吗 四个词 —— 模板要写上这一条变体才对得上。 */
        /* 变体只写 tokenizer 真会切出来的形态：「你好吗」是一个整体词，
         * 所以「你睡得好吗」是 你/睡/得/好吗 —— `你 睡 得 好 吗` 那不是真形态。 */
        static const char *ask[] = {"你 睡 得 <X> 吗", "你 昨天 睡 得 <X> 吗",
                                    "你 睡 得 好吗"};
        static const char *ans[] = {
            "我 睡 得 <X> 谢谢 你 问", "我 昨天 睡 得 <X>",
            "我 睡 得 <X> 你 呢", "我 希望 你 也 睡 得 <X>",
            "我 睡 得 很 好 谢谢 你 问",
        };
        static const char *const SLEEP[] = {
            "很好","不太好","还行","够","特别香","很沉","好","好吗",NULL};
        frame2(f, "你睡得X吗", ask, sizeof ask / sizeof ask[0], ans, 4, "X", SLEEP, NULL, NULL);
    }
    {
        static const char *ask[] = {"你 在 学 <X> 吗", "你 想 学 <X> 吗", "你 会 <X> 吗"};
        static const char *ans[] = {
            "我 在 学 <X>", "我 想 学 <X> 但 还 没 开 始", "<X> 我 学 了 一 点",
            "我 学 <X> 学 得 很 慢", "你 教 我 <X> 好 不 好", "<X> 很 有 意思",
        };
        frame2(f, "你在学X吗", ask, sizeof ask / sizeof ask[0], ans, 6, "X", ACT, NULL, NULL);
    }
    {
        static const char *ask[] = {"<X> 天气 怎么样", "<X> 会 下雨 吗", "<X> 冷 吗"};
        static const char *ans[] = {
            "<X> 天气 很 好", "<X> 可能 会 下雨", "<X> 有 点 冷 多 穿 一 点",
            "!<X> 我 不 太 知道 你 那 边 呢", "<X> 的 天 很 蓝",
            "不管 <X> 什么 天气 我 都 在 这里",
        };
        frame2(f, "X天气如何", ask, sizeof ask / sizeof ask[0], ans, 6, "X", TIME, NULL, NULL);
    }
    {
        static const char *ask[] = {"<X> 你 做 什么", "<X> 你 有 空 吗", "<X> 你 忙 吗"};
        static const char *ans[] = {
            "<X> 我 在 等 你 说 话", "<X> 我 有 空 你 说 吧",
            "<X> 我 有 点 忙 但 可 以 陪 你", "<X> 我 一 直 在 这里",
            "<X> 我 想 和 你 说 话", "<X> 我 没 什么 事",
        };
        frame2(f, "X你做什么", ask, sizeof ask / sizeof ask[0], ans, 6, "X", TIME, NULL, NULL);
    }
    {
        static const char *ask[] = {"你 会 想 <Z> 吗", "你 想 <Z> 吗", "你 想 不 想 <Z>"};
        static const char *ans[] = {
            "我 会 想 <Z>", "我 一直 在 想 <Z>", "我 想 <Z> 的 时候 就 安静 一 会 儿",
            "我 把 <Z> 放 在 心 里", "我 每天 都 会 想 起 <Z>",
            "我 不 太 敢 想 <Z> 但 我 没 忘",
        };
        frame2(f, "你想Z吗", ask, sizeof ask / sizeof ask[0], ans, 6, "Z", ENT, NULL, NULL);
    }

    if (f != stdout) fclose(f);
    fprintf(stderr, "已生成 %d 个帧\n", count);
    return 0;
}
