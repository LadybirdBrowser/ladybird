/*
 * Copyright (c) 2020, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Checked.h>
#include <AK/NumericLimits.h>

// These tests only check whether the usual operator semantics work.
// TODO: Add tests about the actual `Check`ing itself!

TEST_CASE(address_identity)
{
    Checked<int> a = 4;
    Checked<int> b = 5;
    EXPECT_EQ(&a == &a, true);
    EXPECT_EQ(&a == &b, false);
    EXPECT_EQ(&a != &a, false);
    EXPECT_EQ(&a != &b, true);
}

TEST_CASE(operator_identity)
{
    Checked<int> a = 4;
    EXPECT_EQ(a == 4, true);
    EXPECT_EQ(a == 5, false);
    EXPECT_EQ(a != 4, false);
    EXPECT_EQ(a != 5, true);
}

TEST_CASE(operator_incr)
{
    Checked<int> a = 4;
    EXPECT_EQ(++a, 5);
    EXPECT_EQ(++a, 6);
    EXPECT_EQ(++a, 7);
    EXPECT_EQ(a++, 7);
    EXPECT_EQ(a++, 8);
    EXPECT_EQ(a++, 9);
    EXPECT_EQ(a, 10);
}

TEST_CASE(operator_decr)
{
    Checked<u32> a = 5;
    EXPECT_EQ(--a, 4u);
    EXPECT_EQ(--a, 3u);
    EXPECT_EQ(a--, 3u);
    EXPECT_EQ(a--, 2u);
    EXPECT_EQ(a--, 1u);
    EXPECT_EQ(a, 0u);
    EXPECT(!a.has_overflow());
    a--;
    EXPECT(a.has_overflow());
}

TEST_CASE(operator_cmp)
{
    Checked<int> a = 4;
    EXPECT_EQ(a > 3, true);
    EXPECT_EQ(a < 3, false);
    EXPECT_EQ(a >= 3, true);
    EXPECT_EQ(a <= 3, false);
    EXPECT_EQ(a > 4, false);
    EXPECT_EQ(a < 4, false);
    EXPECT_EQ(a >= 4, true);
    EXPECT_EQ(a <= 4, true);
    EXPECT_EQ(a > 5, false);
    EXPECT_EQ(a < 5, true);
    EXPECT_EQ(a >= 5, false);
    EXPECT_EQ(a <= 5, true);
}

TEST_CASE(operator_arith)
{
    Checked<int> a = 12;
    Checked<int> b = 345;
    EXPECT_EQ(a + b, 357);
    EXPECT_EQ(b + a, 357);
    EXPECT_EQ(a - b, -333);
    EXPECT_EQ(b - a, 333);
    EXPECT_EQ(a * b, 4140);
    EXPECT_EQ(b * a, 4140);
    EXPECT_EQ(a / b, 0);
    EXPECT_EQ(b / a, 28);
}

TEST_CASE(detects_signed_overflow)
{
    EXPECT(!(Checked<int>(0x40000000) + Checked<int>(0x3fffffff)).has_overflow());
    EXPECT((Checked<int>(0x40000000) + Checked<int>(0x40000000)).has_overflow());
    EXPECT(!(Checked<int>(-0x40000000) + Checked<int>(-0x40000000)).has_overflow());
    EXPECT((Checked<int>(-0x40000001) + Checked<int>(-0x40000000)).has_overflow());

    EXPECT(!(Checked<int>(0x40000000) - Checked<int>(-0x3fffffff)).has_overflow());
    EXPECT((Checked<int>(0x40000000) - Checked<int>(-0x40000000)).has_overflow());
    EXPECT(!(Checked<int>(-0x40000000) - Checked<int>(0x40000000)).has_overflow());
    EXPECT((Checked<int>(-0x40000000) - Checked<int>(0x40000001)).has_overflow());

    EXPECT(!(Checked<i64>(0x4000000000000000) + Checked<i64>(0x3fffffffffffffff)).has_overflow());
    EXPECT((Checked<i64>(0x4000000000000000) + Checked<i64>(0x4000000000000000)).has_overflow());
    EXPECT(!(Checked<i64>(-0x4000000000000000) + Checked<i64>(-0x4000000000000000)).has_overflow());
    EXPECT((Checked<i64>(-0x4000000000000001) + Checked<i64>(-0x4000000000000000)).has_overflow());

    EXPECT(!(Checked<i64>(0x4000000000000000) - Checked<i64>(-0x3fffffffffffffff)).has_overflow());
    EXPECT((Checked<i64>(0x4000000000000000) - Checked<i64>(-0x4000000000000000)).has_overflow());
    EXPECT(!(Checked<i64>(-0x4000000000000000) - Checked<i64>(0x4000000000000000)).has_overflow());
    EXPECT((Checked<i64>(-0x4000000000000000) - Checked<i64>(0x4000000000000001)).has_overflow());

    EXPECT((Checked<i32>(0x80000000) / Checked<i32>(-1)).has_overflow());
    EXPECT((Checked<i64>(0x8000000000000000) / Checked<i64>(-1)).has_overflow());
}

TEST_CASE(detects_unsigned_overflow)
{
    EXPECT(!(Checked<u32>(0x40000000) + Checked<u32>(0x3fffffff)).has_overflow());
    EXPECT(!(Checked<u32>(0x40000000) + Checked<u32>(0x40000000)).has_overflow());
    EXPECT(!(Checked<u32>(0xf0000000) + Checked<u32>(0x0fffffff)).has_overflow());
    EXPECT((Checked<u32>(0xf0000000) + Checked<u32>(0x10000000)).has_overflow());

    EXPECT(!(Checked<u32>(0x40000000) - Checked<u32>(0x3fffffff)).has_overflow());
    EXPECT(!(Checked<u32>(0x40000000) - Checked<u32>(0x40000000)).has_overflow());
    EXPECT((Checked<u32>(0x40000000) - Checked<u32>(0x40000001)).has_overflow());

    EXPECT(!(Checked<u64>(0x4000000000000000) + Checked<u64>(0x3fffffffffffffff)).has_overflow());
    EXPECT(!(Checked<u64>(0x4000000000000000) + Checked<u64>(0x4000000000000000)).has_overflow());
    EXPECT(!(Checked<u64>(0xf000000000000000) + Checked<u64>(0x0fffffffffffffff)).has_overflow());
    EXPECT((Checked<u64>(0xf000000000000000) + Checked<u64>(0x1000000000000000)).has_overflow());

    EXPECT(!(Checked<u64>(0x4000000000000000) - Checked<u64>(0x3fffffffffffffff)).has_overflow());
    EXPECT(!(Checked<u64>(0x4000000000000000) - Checked<u64>(0x4000000000000000)).has_overflow());
    EXPECT((Checked<u64>(0x4000000000000000) - Checked<u64>(0x4000000000000001)).has_overflow());
}

TEST_CASE(operations_with_overflowed)
{
    {
        Checked<u32> overflowed(0x100000000);
        EXPECT((Checked<u32>(0x100000000) + Checked<u32>(0xff)).has_overflow());
        EXPECT((Checked<u32>(0xff) + Checked<u32>(0x100000000)).has_overflow());
        EXPECT((overflowed += Checked<u32>(0xff)).has_overflow());
        EXPECT((overflowed += Checked<u32>(0x100000000)).has_overflow());
    }

    {
        Checked<u32> overflowed(0x100000000);
        EXPECT((Checked<u32>(0x100000000) - Checked<u32>(0xff)).has_overflow());
        EXPECT((Checked<u32>(0xff) - Checked<u32>(0x100000000)).has_overflow());
        EXPECT((overflowed -= Checked<u32>(0xff)).has_overflow());
        EXPECT((overflowed -= Checked<u32>(0x100000000)).has_overflow());
    }

    {
        Checked<u32> overflowed(0x100000000);
        EXPECT((Checked<u32>(0x100000000) * Checked<u32>(0xff)).has_overflow());
        EXPECT((Checked<u32>(0xff) * Checked<u32>(0x100000000)).has_overflow());
        EXPECT((overflowed *= Checked<u32>(0xff)).has_overflow());
        EXPECT((overflowed *= Checked<u32>(0x100000000)).has_overflow());
    }

    {
        Checked<u32> overflowed(0x100000000);
        EXPECT((Checked<u32>(0x100000000) / Checked<u32>(0xff)).has_overflow());
        EXPECT((Checked<u32>(0xff) / Checked<u32>(0x100000000)).has_overflow());
        EXPECT((overflowed /= Checked<u32>(0xff)).has_overflow());
        EXPECT((overflowed /= Checked<u32>(0x100000000)).has_overflow());
    }

    {
        Checked<u32> overflowed(0x100000000);
        EXPECT((Checked<u32>(0x100000000) % Checked<u32>(0xff)).has_overflow());
        EXPECT((Checked<u32>(0xff) % Checked<u32>(0x100000000)).has_overflow());
        EXPECT((overflowed %= Checked<u32>(0xff)).has_overflow());
        EXPECT((overflowed %= Checked<u32>(0x100000000)).has_overflow());
    }
}

TEST_CASE(should_constexpr_default_construct)
{
    constexpr Checked<int> checked_value {};
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == int {});
}

TEST_CASE(should_constexpr_value_construct)
{
    constexpr Checked<int> checked_value { 42 };
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_convert_construct)
{
    constexpr Checked<int> checked_value { 42u };
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_copy_construct)
{
    constexpr auto checked_value = [] {
        Checked<int> const old_value { 42 };
        Checked<int> value(old_value);
        return value;
    }();
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_move_construct)
{
    constexpr auto checked_value = [] {
        Checked<int> value(Checked<int> { 42 });
        return value;
    }();
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_copy_assign)
{
    constexpr auto checked_value = [] {
        Checked<int> const old_value { 42 };
        Checked<int> value {};
        value = old_value;
        return value;
    }();
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_move_assign)
{
    constexpr auto checked_value = [] {
        Checked<int> value {};
        value = Checked<int> { 42 };
        return value;
    }();
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_convert_and_assign)
{
    constexpr auto checked_value = [] {
        Checked<int> value {};
        value = 42;
        return value;
    }();
    static_assert(!checked_value.has_overflow());
    static_assert(checked_value == 42);
}

TEST_CASE(should_constexpr_not_operator)
{
    constexpr Checked<int> value {};
    static_assert(!value);
}

TEST_CASE(should_constexpr_value_accessor)
{
    constexpr Checked<int> value { 42 };
    static_assert(value.value() == 42);
}

TEST_CASE(should_constexpr_add)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value.add(3);
        return value;
    }();
    static_assert(checked_value == 45);
}

TEST_CASE(should_constexpr_sub)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value.sub(3);
        return value;
    }();
    static_assert(checked_value == 39);
}

TEST_CASE(should_constexpr_mul)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value.mul(2);
        return value;
    }();
    static_assert(checked_value == 84);
}

TEST_CASE(should_constexpr_div)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value.div(3);
        return value;
    }();
    static_assert(checked_value == 14);
}

TEST_CASE(should_constexpr_assignment_by_sum)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value += 3;
        return value;
    }();
    static_assert(checked_value == 45);
}

TEST_CASE(should_constexpr_assignment_by_diff)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value -= 3;
        return value;
    }();
    static_assert(checked_value == 39);
}

TEST_CASE(should_constexpr_assignment_by_product)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value *= 2;
        return value;
    }();
    static_assert(checked_value == 84);
}

TEST_CASE(should_constexpr_assignment_by_quotient)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value /= 3;
        return value;
    }();
    static_assert(checked_value == 14);
}

TEST_CASE(should_constexpr_prefix_increment)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        ++value;
        return value;
    }();
    static_assert(checked_value == 43);
}

TEST_CASE(should_constexpr_postfix_increment)
{
    constexpr auto checked_value = [] {
        Checked<int> value { 42 };
        value++;
        return value;
    }();
    static_assert(checked_value == 43);
}

TEST_CASE(should_constexpr_check_for_overflow_addition)
{
    static_assert(Checked<int>::addition_would_overflow(NumericLimits<int>::max(), 1));
}

TEST_CASE(should_constexpr_check_for_overflow_multiplication)
{
    static_assert(Checked<int>::multiplication_would_overflow(NumericLimits<int>::max(), 2));
}

TEST_CASE(should_constexpr_add_checked_values)
{
    constexpr Checked<int> a { 42 };
    constexpr Checked<int> b { 17 };
    constexpr Checked<int> expected { 59 };
    static_assert(expected == (a + b).value());
}

TEST_CASE(should_constexpr_subtract_checked_values)
{
    constexpr Checked<int> a { 42 };
    constexpr Checked<int> b { 17 };
    constexpr Checked<int> expected { 25 };
    static_assert(expected == (a - b).value());
}

TEST_CASE(should_constexpr_multiply_checked_values)
{
    constexpr Checked<int> a { 3 };
    constexpr Checked<int> b { 5 };
    constexpr Checked<int> expected { 15 };
    static_assert(expected == (a * b).value());
}

TEST_CASE(should_constexpr_divide_checked_values)
{
    constexpr Checked<int> a { 10 };
    constexpr Checked<int> b { 2 };
    constexpr Checked<int> expected { 5 };
    static_assert(expected == (a / b).value());
}

TEST_CASE(should_constexpr_compare_checked_values_lhs)
{
    constexpr Checked<int> a { 10 };

    static_assert(a > 5);
    static_assert(a >= 10);
    static_assert(a >= 5);

    static_assert(a < 20);
    static_assert(a <= 30);
    static_assert(a <= 20);

    static_assert(a == 10);
    static_assert(a != 20);
}

TEST_CASE(should_constexpr_compare_checked_values_rhs)
{
    constexpr Checked<int> a { 10 };

    static_assert(5 < a);
    static_assert(10 <= a);
    static_assert(5 <= a);

    static_assert(20 > a);
    static_assert(30 >= a);
    static_assert(30 >= a);

    static_assert(10 == a);
    static_assert(20 != a);
}

TEST_CASE(should_constexpr_make_via_factory)
{
    [[maybe_unused]] constexpr auto value = make_checked(42);
}

TEST_CASE(is_within_range_float_to_int)
{
    static_assert(AK::is_within_range<int>(0.0));
    static_assert(AK::is_within_range<int>(0.0f));
    static_assert(AK::is_within_range<int>(((long double)0.0)));

    static_assert(!AK::is_within_range<int>(NumericLimits<float>::max()));
    static_assert(!AK::is_within_range<int>(NumericLimits<double>::max()));
    static_assert(!AK::is_within_range<int>(NumericLimits<long double>::max()));
    static_assert(!AK::is_within_range<int>(NumericLimits<float>::lowest()));

    static_assert(AK::is_within_range<long>(0.0));
    static_assert(AK::is_within_range<long long>(0.0));

    static_assert(!AK::is_within_range<int>(NAN));

    // Boundary checks: implicit float<->integer conversion can round the integer up to a value that's one past the
    // actual destination range — so naive 'value <= max()' is too permissive. These cases must report false.
    static_assert(!AK::is_within_range<int>(2147483648.0));  // INT_MAX + 1, exact in double
    static_assert(!AK::is_within_range<int>(2147483648.0f)); // INT_MAX + 1 = 2^31, exact in float
    static_assert(AK::is_within_range<int>(-2147483648.0));  // INT_MIN, exact in double
    static_assert(AK::is_within_range<int>(-2147483648.0f)); // INT_MIN, exact in float
    static_assert(!AK::is_within_range<int>(-2147483649.0)); // INT_MIN - 1

    static_assert(AK::is_within_range<unsigned>(4294967295.0));   // UINT_MAX, exact in double
    static_assert(!AK::is_within_range<unsigned>(4294967296.0));  // UINT_MAX + 1, exact in double
    static_assert(!AK::is_within_range<unsigned>(4294967296.0f)); // UINT_MAX + 1 = 2^32, exact in float
    static_assert(!AK::is_within_range<unsigned>(-1.0));

    static_assert(AK::is_within_range<signed char>(127.0f));
    static_assert(!AK::is_within_range<signed char>(128.0f));
    static_assert(AK::is_within_range<signed char>(-128.0f));
    static_assert(!AK::is_within_range<signed char>(-129.0f));

    static_assert(AK::is_within_range<unsigned char>(255.0f));
    static_assert(!AK::is_within_range<unsigned char>(256.0f));

    // For 64-bit destinations the boundary is at the precision limit of double.
    static_assert(!AK::is_within_range<long long>(9223372036854775808.0));           // INT64_MAX + 1 = 2^63
    static_assert(AK::is_within_range<long long>(-9223372036854775808.0));           // INT64_MIN
    static_assert(!AK::is_within_range<unsigned long long>(18446744073709551616.0)); // UINT64_MAX + 1 = 2^64

    // Fractional floats are rejected. A value like 4294967295.5 isn't representable as an unsigned int (it has a
    // fractional part) — even though its truncation would fit.
    static_assert(!AK::is_within_range<unsigned>(4294967295.5));  // 4294967295.5 > UINT_MAX
    static_assert(!AK::is_within_range<unsigned>(4294967295.5f)); // literal also rounds to 2^32, OOB
    static_assert(!AK::is_within_range<int>(2147483647.5));       // 2147483647.5 > INT_MAX
    static_assert(!AK::is_within_range<int>(2147483647.5f));      // literal rounds to 2^31, OOB
    static_assert(!AK::is_within_range<int>(2.5));                // fractional, even well within INT bounds
    static_assert(!AK::is_within_range<int>(-2.5));               // negative fractional, same reason
    static_assert(!AK::is_within_range<unsigned>(-0.5));          // negative, can't be unsigned

    // Integer-valued floats well within range remain in range.
    static_assert(AK::is_within_range<int>(0.0));
    static_assert(AK::is_within_range<int>(-2147483648.0));     // INT_MIN
    static_assert(AK::is_within_range<int>(2147483647.0));      // INT_MAX
    static_assert(AK::is_within_range<unsigned>(4294967295.0)); // UINT_MAX
}
