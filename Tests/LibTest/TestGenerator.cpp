/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibTest/Randomized/Generator.h>
#include <LibTest/TestCase.h>
#include <math.h>

using namespace Test::Randomized;

RANDOMIZED_TEST_CASE(number_u64_max_bounds)
{
    GEN(n, Gen::number_u64(10));
    EXPECT(n <= 10);
}

RANDOMIZED_TEST_CASE(number_u64_min_max_bounds)
{
    GEN(n, Gen::number_u64(3, 6));
    EXPECT(n >= 3 && n <= 6);
}

RANDOMIZED_TEST_CASE(assume)
{
    GEN(n, Gen::number_u64(10));
    ASSUME(n % 2 == 0); // This will try to generate until it finds an even number
    EXPECT(n % 2 == 0); // This will then succeed
    // It will give up if the value doesn't pass the ASSUME(...) predicate 15 times in a row.
}

// TODO find a way to test that a test "number_u64(3) can't reach 0" fails
// TODO find a way to test that a test "number_u64(3) can't reach 3" fails
// TODO find a way to test that a test "number_u64(3,6) can't reach 3" fails
// TODO find a way to test that a test "number_u64(3,6) can't reach 6" fails
// TODO find a way to test that a test "number_u64(10) can reach n>10" fails

RANDOMIZED_TEST_CASE(map_like)
{
    GEN(n1, Gen::number_u64(10));
    GEN(n2, n1 * 2);
    EXPECT(n2 % 2 == 0);
}

RANDOMIZED_TEST_CASE(bind_like)
{
    GEN(n1, Gen::number_u64(1, 9));
    GEN(n2, Gen::number_u64(n1 * 10, n1 * 100));
    EXPECT(n2 >= 10 && n2 <= 900);
}

// An example of an user-defined generator (for the test bind_vector_suboptimal).
//
// For why this is a suboptimal way to generate collections, see the comment in
// Shrink::shrink_delete().
//
// TL;DR: this makes the length non-local to the items we're trying to delete
// (except the first item).
//
// There's a better way: flip a (biased) coin to decide whether to generate
// a next item. That makes each item much better shrinkable, since its
// contribution to the sequence length (a boolean 0 or 1) is right next to its
// own data.
//
// Because it's a pretty natural way to do this, we take special care in the
// internal shrinker to work well on this style too.
template<typename FN>
Vector<InvokeResult<FN>> vector_suboptimal(FN item_gen)
{
    u32 length = Gen::number_u64(5);
    Vector<InvokeResult<FN>> acc;
    for (u32 i = 0; i < length; ++i) {
        acc.append(item_gen());
    }
    return acc;
}

RANDOMIZED_TEST_CASE(bind_vector_suboptimal)
{
    u32 max_item = 5;
    GEN(vec, vector_suboptimal([&]() { return Gen::number_u64(max_item); }));
    u32 sum = 0;
    for (u32 n : vec) {
        sum += n;
    }
    EXPECT(sum <= vec.size() * max_item);
}

RANDOMIZED_TEST_CASE(vector)
{
    u32 max_item = 5;
    GEN(vec, Gen::vector([&]() { return Gen::number_u64(max_item); }));
    EXPECT(vec.size() <= 32);
}

RANDOMIZED_TEST_CASE(vector_length)
{
    u32 max_item = 5;
    GEN(vec, Gen::vector(3, [&]() { return Gen::number_u64(max_item); }));
    EXPECT(vec.size() == 3);
}

RANDOMIZED_TEST_CASE(vector_min_max)
{
    u32 max_item = 5;
    GEN(vec, Gen::vector(1, 4, [&]() { return Gen::number_u64(max_item); }));
    EXPECT(vec.size() >= 1 && vec.size() <= 4);
}

RANDOMIZED_TEST_CASE(weighted_boolean_below0)
{
    GEN(b, Gen::weighted_boolean(-0.5));
    EXPECT(b == false);
}

RANDOMIZED_TEST_CASE(weighted_boolean_0)
{
    GEN(b, Gen::weighted_boolean(0));
    EXPECT(b == false);
}

RANDOMIZED_TEST_CASE(weighted_boolean_1)
{
    GEN(b, Gen::weighted_boolean(1));
    EXPECT(b == true);
}

RANDOMIZED_TEST_CASE(weighted_boolean_above1)
{
    GEN(b, Gen::weighted_boolean(1.5));
    EXPECT(b == true);
}

RANDOMIZED_TEST_CASE(weighted_boolean_fair_true)
{
    GEN(b, Gen::weighted_boolean(0.5));
    ASSUME(b == true);
    EXPECT(b == true);
}

RANDOMIZED_TEST_CASE(weighted_boolean_fair_false)
{
    GEN(b, Gen::weighted_boolean(0.5));
    ASSUME(b == false);
    EXPECT(b == false);
}

RANDOMIZED_TEST_CASE(boolean_true)
{
    GEN(b, Gen::boolean());
    ASSUME(b == true);
    EXPECT(b == true);
}

RANDOMIZED_TEST_CASE(boolean_false)
{
    GEN(b, Gen::boolean());
    ASSUME(b == false);
    EXPECT(b == false);
}

RANDOMIZED_TEST_CASE(one_of_int)
{
    GEN(x, Gen::one_of(1, 2));
    EXPECT(x == 1 || x == 2);
}

RANDOMIZED_TEST_CASE(frequency_int)
{
    GEN(x, Gen::frequency(Gen::Choice { 5, 'x' }, Gen::Choice { 1, 'o' }));
    ASSUME(x == 'x');
    EXPECT(x == 'x');
}

RANDOMIZED_TEST_CASE(percentage)
{
    GEN(x, Gen::percentage());
    EXPECT(x >= 0 && x <= 1);
}

RANDOMIZED_TEST_CASE(number_f64_max_bounds)
{
    GEN(x, Gen::number_f64(10));
    EXPECT(x <= 10);
}

RANDOMIZED_TEST_CASE(number_f64_min_max_bounds)
{
    GEN(x, Gen::number_f64(-10, 10));
    EXPECT(x >= -10 && x <= 10);
}

RANDOMIZED_TEST_CASE(number_f64_never_nan)
{
    GEN(x, Gen::number_f64());
    EXPECT(!isnan(x));
}

RANDOMIZED_TEST_CASE(number_f64_never_infinite)
{
    GEN(x, Gen::number_f64());
    EXPECT(!isinf(x));
}

RANDOMIZED_TEST_CASE(number_u32_max_bounds)
{
    GEN(n, Gen::number_u32(10));
    EXPECT(n <= 10);
}

RANDOMIZED_TEST_CASE(number_u32_min_max_bounds)
{
    GEN(n, Gen::number_u32(3, 6));
    EXPECT(n >= 3 && n <= 6);
}
