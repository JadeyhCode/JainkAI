/* 测试夹具（§6.710）：用了**不存在的类型名** ⇒ 编译门必须把它挡在图外
 * （⚠ 别用 `struct xxx *`：C 里"指向不完整类型的指针"是合法的，那种写法能编过，
 *   第一版夹具就是这么"假坏"的 —— 门没骗人，是夹具写错了）*/
not_a_real_type *make_thing(int n)
{
    not_a_real_type *t = 0;
    (void)n;
    return t;
}
