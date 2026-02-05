/*
 * Copyright (c) 2020, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/DistinctNumeric.h>

template<typename T>
class ForType {
public:
    static void check_size()
    {
        AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(T, TheNumeric);
        EXPECT_EQ(sizeof(T), sizeof(TheNumeric));
    }
};

TEST_CASE(check_size)
{
#define CHECK_SIZE_FOR_SIGNABLE(T)         \
    do {                                   \
        ForType<signed T>::check_size();   \
        ForType<unsigned T>::check_size(); \
    } while (false)
    CHECK_SIZE_FOR_SIGNABLE(char);
    CHECK_SIZE_FOR_SIGNABLE(short);
    CHECK_SIZE_FOR_SIGNABLE(int);
    CHECK_SIZE_FOR_SIGNABLE(long);
    CHECK_SIZE_FOR_SIGNABLE(long long);
    ForType<float>::check_size();
    ForType<double>::check_size();
}

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, BareNumeric);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, IncrNumeric, Increment);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, CmpNumeric, Comparison);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, BoolNumeric, CastToBool);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, FlagsNumeric, Flags);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, ShiftNumeric, Shift);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, ArithNumeric, Arithmetic);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, UnderlyingNumeric, CastToUnderlying);
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, GeneralNumeric, Arithmetic, CastToBool, CastToUnderlying, Comparison, Flags, Increment, Shift);

TEST_CASE(address_identity)
{
    BareNumeric a = 4;
    BareNumeric b = 5;
    EXPECT_EQ(&a == &a, true);
    EXPECT_EQ(&a == &b, false);
    EXPECT_EQ(&a != &a, false);
    EXPECT_EQ(&a != &b, true);
}

TEST_CASE(operator_identity)
{
    BareNumeric a = 4;
    BareNumeric b = 5;
    EXPECT_EQ(a == a, true);
    EXPECT_EQ(a == b, false);
    EXPECT_EQ(a != a, false);
    EXPECT_EQ(a != b, true);
}

TEST_CASE(operator_incr)
{
    IncrNumeric a = 4;
    IncrNumeric b = 5;
    IncrNumeric c = 6;
    EXPECT_EQ(++a, b);
    EXPECT_EQ(a++, b);
    EXPECT_EQ(a, c);
    EXPECT_EQ(--a, b);
    EXPECT_EQ(a--, b);
    EXPECT(a != b);
}

TEST_CASE(operator_cmp)
{
    CmpNumeric a = 4;
    CmpNumeric b = 5;
    CmpNumeric c = 5;
    EXPECT_EQ(a > b, false);
    EXPECT_EQ(a < b, true);
    EXPECT_EQ(a >= b, false);
    EXPECT_EQ(a <= b, true);
    EXPECT_EQ(b > a, true);
    EXPECT_EQ(b < a, false);
    EXPECT_EQ(b >= a, true);
    EXPECT_EQ(b <= a, false);
    EXPECT_EQ(b > c, false);
    EXPECT_EQ(b < c, false);
    EXPECT_EQ(b >= c, true);
    EXPECT_EQ(b <= c, true);
}

TEST_CASE(operator_bool)
{
    BoolNumeric a = 0;
    BoolNumeric b = 42;
    BoolNumeric c = 1337;
    EXPECT_EQ(!a, true);
    EXPECT_EQ(!b, false);
    EXPECT_EQ(!c, false);
}

TEST_CASE(operator_underlying)
{
    UnderlyingNumeric a = 0;
    UnderlyingNumeric b = 42;
    EXPECT_EQ(static_cast<int>(a), 0);
    EXPECT_EQ(static_cast<int>(b), 42);
}

TEST_CASE(operator_flags)
{
    FlagsNumeric a = 0;
    FlagsNumeric b = 0xA60;
    FlagsNumeric c = 0x03B;
    EXPECT_EQ(~a, FlagsNumeric(~0x0));
    EXPECT_EQ(~b, FlagsNumeric(~0xA60));
    EXPECT_EQ(~c, FlagsNumeric(~0x03B));

    EXPECT_EQ(a & b, b & a);
    EXPECT_EQ(a & c, c & a);
    EXPECT_EQ(b & c, c & b);
    EXPECT_EQ(a | b, b | a);
    EXPECT_EQ(a | c, c | a);
    EXPECT_EQ(b | c, c | b);
    EXPECT_EQ(a ^ b, b ^ a);
    EXPECT_EQ(a ^ c, c ^ a);
    EXPECT_EQ(b ^ c, c ^ b);

    EXPECT_EQ(a & b, FlagsNumeric(0x000));
    EXPECT_EQ(a & c, FlagsNumeric(0x000));
    EXPECT_EQ(b & c, FlagsNumeric(0x020));
    EXPECT_EQ(a | b, FlagsNumeric(0xA60));
    EXPECT_EQ(a | c, FlagsNumeric(0x03B));
    EXPECT_EQ(b | c, FlagsNumeric(0xA7B));
    EXPECT_EQ(a ^ b, FlagsNumeric(0xA60));
    EXPECT_EQ(a ^ c, FlagsNumeric(0x03B));
    EXPECT_EQ(b ^ c, FlagsNumeric(0xA5B));

    EXPECT_EQ(a &= b, FlagsNumeric(0x000));
    EXPECT_EQ(a, FlagsNumeric(0x000));
    EXPECT_EQ(a |= b, FlagsNumeric(0xA60));
    EXPECT_EQ(a, FlagsNumeric(0xA60));
    EXPECT_EQ(a &= c, FlagsNumeric(0x020));
    EXPECT_EQ(a, FlagsNumeric(0x020));
    EXPECT_EQ(a ^= b, FlagsNumeric(0xA40));
    EXPECT_EQ(a, FlagsNumeric(0xA40));

    EXPECT_EQ(b, FlagsNumeric(0xA60));
    EXPECT_EQ(c, FlagsNumeric(0x03B));
}

TEST_CASE(operator_shift)
{
    ShiftNumeric a = 0x040;
    EXPECT_EQ(a << ShiftNumeric(0), ShiftNumeric(0x040));
    EXPECT_EQ(a << ShiftNumeric(1), ShiftNumeric(0x080));
    EXPECT_EQ(a << ShiftNumeric(2), ShiftNumeric(0x100));
    EXPECT_EQ(a >> ShiftNumeric(0), ShiftNumeric(0x040));
    EXPECT_EQ(a >> ShiftNumeric(1), ShiftNumeric(0x020));
    EXPECT_EQ(a >> ShiftNumeric(2), ShiftNumeric(0x010));

    EXPECT_EQ(a <<= ShiftNumeric(5), ShiftNumeric(0x800));
    EXPECT_EQ(a, ShiftNumeric(0x800));
    EXPECT_EQ(a >>= ShiftNumeric(8), ShiftNumeric(0x008));
    EXPECT_EQ(a, ShiftNumeric(0x008));
}

TEST_CASE(operator_arith)
{
    ArithNumeric a = 12;
    ArithNumeric b = 345;
    EXPECT_EQ(a + b, ArithNumeric(357));
    EXPECT_EQ(b + a, ArithNumeric(357));
    EXPECT_EQ(a - b, ArithNumeric(-333));
    EXPECT_EQ(b - a, ArithNumeric(333));
    EXPECT_EQ(+a, ArithNumeric(12));
    EXPECT_EQ(-a, ArithNumeric(-12));
    EXPECT_EQ(a * b, ArithNumeric(4140));
    EXPECT_EQ(b * a, ArithNumeric(4140));
    EXPECT_EQ(a / b, ArithNumeric(0));
    EXPECT_EQ(b / a, ArithNumeric(28));
    EXPECT_EQ(a % b, ArithNumeric(12));
    EXPECT_EQ(b % a, ArithNumeric(9));

    EXPECT_EQ(a += a, ArithNumeric(24));
    EXPECT_EQ(a, ArithNumeric(24));
    EXPECT_EQ(a *= a, ArithNumeric(576));
    EXPECT_EQ(a, ArithNumeric(576));
    EXPECT_EQ(a /= a, ArithNumeric(1));
    EXPECT_EQ(a, ArithNumeric(1));
    EXPECT_EQ(a %= a, ArithNumeric(0));
    EXPECT_EQ(a, ArithNumeric(0));

    a = ArithNumeric(12);
    EXPECT_EQ(a -= a, ArithNumeric(0));
    EXPECT_EQ(a, ArithNumeric(0));
}

TEST_CASE(composability)
{
    GeneralNumeric a = 0;
    GeneralNumeric b = 1;
    // Ident
    EXPECT_EQ(a == a, true);
    EXPECT_EQ(a == b, false);
    // Incr
    EXPECT_EQ(++a, b);
    EXPECT_EQ(a--, b);
    EXPECT_EQ(a == b, false);
    // Cmp
    EXPECT_EQ(a < b, true);
    EXPECT_EQ(a >= b, false);
    // Bool
    EXPECT_EQ(!a, true);
    // Flags
    EXPECT_EQ(a & b, GeneralNumeric(0));
    EXPECT_EQ(a | b, GeneralNumeric(1));
    // Shift
    EXPECT_EQ(b << GeneralNumeric(4), GeneralNumeric(0x10));
    EXPECT_EQ(b >> b, GeneralNumeric(0));
    // Arith
    EXPECT_EQ(-b, GeneralNumeric(-1));
    EXPECT_EQ(a + b, b);
    EXPECT_EQ(b * GeneralNumeric(42), GeneralNumeric(42));
    // Underlying
    EXPECT_EQ(static_cast<int>(a), 0);
    EXPECT_EQ(static_cast<int>(b), 1);
}

/*
 * FIXME: These `negative_*` tests should cause precisely one compilation error
 * each, and always for the specified reason. Currently we do not have a harness
 * for that, so in order to run the test you need to set the #define to 1, compile
 * it, and check the error messages manually.
 */
#define COMPILE_NEGATIVE_TESTS 0
#if COMPILE_NEGATIVE_TESTS
TEST_CASE(negative_incr)
{
    BareNumeric a = 12;
    a++;
    // error: static assertion failed: 'a++' is only available for DistinctNumeric types with 'Increment'.
}

TEST_CASE(negative_cmp)
{
    BareNumeric a = 12;
    [[maybe_unused]] auto res = (a < a);
    // error: static assertion failed: 'a<b' is only available for DistinctNumeric types with 'Comparison'.
}

TEST_CASE(negative_bool)
{
    BareNumeric a = 12;
    [[maybe_unused]] auto res = !a;
    // error: static assertion failed: '!a', 'a&&b', 'a||b' and similar operators are only available for DistinctNumeric types with 'CastToBool'.
}

TEST_CASE(negative_flags)
{
    BareNumeric a = 12;
    [[maybe_unused]] auto res = (a & a);
    // error: static assertion failed: 'a&b' is only available for DistinctNumeric types with 'Flags'.
}

TEST_CASE(negative_shift)
{
    BareNumeric a = 12;
    [[maybe_unused]] auto res = (a << a);
    // error: static assertion failed: 'a<<b' is only available for DistinctNumeric types with 'Shift'.
}

TEST_CASE(negative_arith)
{
    BareNumeric a = 12;
    [[maybe_unused]] auto res = (a + a);
    // error: static assertion failed: 'a+b' is only available for DistinctNumeric types with 'Arithmetic'.
}

TEST_CASE(negative_underlying)
{
    BareNumeric a = 12;
    [[maybe_unused]] int res = static_cast<int>(a);
    // error: static assertion failed: Cast to underlying type is only available for DistinctNumeric types with 'CastToUnderlying'.
}

TEST_CASE(negative_incompatible)
{
    GeneralNumeric a = 12;
    ArithNumeric b = 345;
    // And this is the entire point of `DistinctNumeric`:
    // Theoretically, the operation *could* be supported, but we declared those int types incompatible.
    [[maybe_unused]] auto res = (a + b);
    // error: no match for ‘operator+’ (operand types are ‘GeneralNumeric’ {aka ‘AK::DistinctNumeric<int, true, true, true, true, true, true, 64, 64>’} and ‘ArithNumeric’ {aka ‘AK::DistinctNumeric<int, false, false, false, false, false, true, 64, 63>’})
    //    313 |     [[maybe_unused]] auto res = (a + b);
    //        |                                  ~ ^ ~
    //        |                                  |   |
    //        |                                  |   DistinctNumeric<[...],false,false,false,false,false,[...],[...],63>
    //        |                                  DistinctNumeric<[...],true,true,true,true,true,[...],[...],64>
}
#endif /* COMPILE_NEGATIVE_TESTS */
