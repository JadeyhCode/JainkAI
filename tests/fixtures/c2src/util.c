/* 测试夹具（§6.710）：两个**自包含**的辅助函数（没有任何外部依赖）+ 一个 main（不该被收） */
#include <stdio.h>

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

long sum_to(long n)
{
    long s = 0;
    for (long i = 1; i <= n; i++) s += i;
    return s;
}

int main(void)
{
    printf("%d %ld\n", clamp_int(7, 0, 5), sum_to(10));
    return 0;
}
