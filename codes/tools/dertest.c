/* tools/dertest.c — 验证器自身的测试：**伪造的链必须被拒**
 *
 * 「可以正确思考」这句话不能只靠声称。这个程序故意构造几条**坏链**，
 * 看验证器（der_verify）能不能把它们打回缺口；再构造一条好链，看它是否放行。
 *
 * 用法: ./build_dertest [model.bin]
 */
#include "../agent.h"
#include "../model_io.h"
#include <stdio.h>

static int fails = 0;
static void ck(const char *what, int cond) {
    printf("  %s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) fails = 1;
}

static Token *T(AI *ai, const char *n) { return pool_find(ai->pool, n); }

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "model.bin";
    AI *ai = ai_load_file(path);
    if (!ai) { printf("加载失败 %s\n", path); return 1; }

    Token *cat = T(ai, "猫"), *dog = T(ai, "狗"), *animal = T(ai, "动物");
    Token *fur = T(ai, "毛"), *bird = T(ai, "鸟"), *fly = T(ai, "飞");
    Token *fish = T(ai, "鱼");
    if (!cat || !animal || !fur || !bird || !fly) { printf("模型里缺测试要用的词\n"); return 1; }

    printf("① 伪造一个「图里不存在的事实」→ 必须变成缺口\n");
    {
        Derivation d;
        der_reset(&d);
        d.has_claim = 1;
        der_goal(&d, dog, REL_IS_A, fish);            /* 狗 属于 鱼 —— 知识库没有 */
        der_fact(&d, dog, REL_IS_A, fish, 0, +1);
        der_conclude(&d, +1, 90);
        int gaps = der_verify(ai, &d);
        ck("伪造事实被识破（gaps>0）", gaps > 0);
        ck("那一步被降级成 DER_GAP", d.step[1].kind == DER_GAP);
    }

    printf("② 用错误的规则（「有」不支持传递，却按传递推）→ 必须变成缺口\n");
    {
        Derivation d;
        der_reset(&d);
        d.has_claim = 1;
        der_goal(&d, cat, REL_HAS, fur);
        der_rule(&d, 1 /*传递*/, cat, REL_HAS, animal, +1);   /* 「有」不是可传递关系 */
        der_rule(&d, 1 /*传递*/, animal, REL_HAS, fur, +1);
        der_conclude(&d, +1, 90);
        int gaps = der_verify(ai, &d);
        ck("非法规则被识破（gaps>0）", gaps > 0);
    }

    printf("③ 链断掉（上一环终点 ≠ 这一环起点）→ 必须变成缺口\n");
    {
        Derivation d;
        der_reset(&d);
        d.has_claim = 1;
        der_goal(&d, cat, REL_IS_A, animal);
        der_fact(&d, cat, REL_IS_A, animal, 0, +1);
        der_fact(&d, bird, REL_IS_A, animal, 0, +1);   /* 接不上：animal ≠ bird */
        der_conclude(&d, +1, 90);
        int gaps = der_verify(ai, &d);
        ck("断链被识破（gaps>0）", gaps > 0);
    }

    printf("④ 推测却没有真凭据（依据词对这件事没态度）→ 必须变成缺口\n");
    {
        Derivation d;
        der_reset(&d);
        d.has_claim = 1;
        der_goal(&d, cat, REL_CAN, fly);
        der_default(&d, cat, fish, REL_CAN, fly, 99, +1);   /* 鱼 并不知道「猫会飞」 */
        der_fact(&d, fish, REL_CAN, fly, 0, +1);
        der_conclude(&d, +1, 45);
        int gaps = der_verify(ai, &d);
        ck("假依据被识破（gaps>0）", gaps > 0);
    }

    printf("⑤ 结论推不出来 → 必须变成缺口\n");
    {
        Derivation d;
        der_reset(&d);
        d.has_claim = 1;
        der_goal(&d, cat, REL_CAN, fur);              /* 猫 会 毛 —— 无意义 */
        der_fact(&d, cat, REL_IS_A, animal, 0, +1);
        der_conclude(&d, +1, 90);
        int gaps = der_verify(ai, &d);
        ck("重推不出来的结论被识破（gaps>0）", gaps > 0);
    }

    printf("⑥ 一条真链 → 必须放行（gaps==0）\n");
    {
        Proof pf;
        Derivation d;
        rel_solve(ai, cat, REL_HAS, fur, 0, &pf);
        if (pf.status == 0) { printf("  （模型里没有「猫 有 毛」的推导，跳过）\n"); }
        else {
            der_from_proof(ai, &d, cat, REL_HAS, fur, &pf);
            d.has_claim = 1;
            der_conclude(&d, +1, 85);
            int gaps = der_verify(ai, &d);
            ck("真链放行（gaps==0）", gaps == 0);
            char line[400];
            der_chain_text(ai, &d, line, sizeof line);
            printf("       链: %s\n", line);
            /* 同一份链四种说法，真值必须一致 */
            char a0[400], a1[400], a2[400], a3[400];
            der_say(ai, &d, DER_STYLE_TERSE, a0, sizeof a0);
            der_say(ai, &d, DER_STYLE_PLAIN, a1, sizeof a1);
            der_say(ai, &d, DER_STYLE_CHAIN, a2, sizeof a2);
            der_say(ai, &d, DER_STYLE_SYMBOL, a3, sizeof a3);
            printf("       0:%s\n       1:%s\n       2:%s\n       3:%s\n", a0, a1, a2, a3);
            ck("风格 0 是纯结论（对）", strcmp(a0, "对") == 0);
            ck("风格 1/3 都以「对」开头", strncmp(a1, "对", 3) == 0 && strncmp(a3, "对", 3) == 0);
            ck("风格 2 不给结论（不含「对」）", strstr(a2, "对") == NULL);
        }
    }

    printf("⑦ 有缺口的链，渲染时绝不下断言\n");
    {
        Derivation d;
        der_reset(&d);
        d.has_claim = 1;
        der_goal(&d, cat, REL_CAN, fur);
        der_fact(&d, cat, REL_CAN, fur, 0, +1);       /* 假事实 → 缺口 */
        der_conclude(&d, +1, 90);
        der_verify(ai, &d);
        char o[400];
        der_say(ai, &d, DER_STYLE_PLAIN, o, sizeof o);
        printf("       有缺口时的输出: %s\n", o);
        ck("不下断言（不含「对 ，」）", strstr(o, "对 ，") == NULL);
        ck("明说推不出来", strstr(o, "推不出来") != NULL);
    }

    ai_free(ai);
    printf("\n%s\n", fails ? "FAIL dertest" : "PASS dertest（验证器能识破伪造/非法/断链/假依据，放行真链）");
    return fails ? 1 : 0;
}
