/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>

static constexpr int constexpr_sum(ReadonlySpan<int> const span)
{
    int sum = 0;
    for (auto value : span)
        sum += value;

    return sum;
}

TEST_CASE(compile_time_constructible)
{
    constexpr Array array = { 0, 1, 2, 3 };
    static_assert(array.size() == 4);
}

TEST_CASE(conforms_to_iterator_protocol)
{
    static_assert(std::random_access_iterator<Array<int, 8>::Iterator>);
    static_assert(std::random_access_iterator<Array<int, 8>::ConstIterator>);
}

TEST_CASE(compile_time_iterable)
{
    constexpr Array<int, 8> array = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static_assert(constexpr_sum(array) == 28);
}

TEST_CASE(contains_slow)
{
    constexpr Array<int, 8> array = { 0, 1, 2, 3, 4, 5, 6, 7 };
    EXPECT(array.contains_slow(0));
    EXPECT(array.contains_slow(4));
    EXPECT(array.contains_slow(7));
    EXPECT(!array.contains_slow(42));
}

TEST_CASE(first_index_of)
{
    constexpr Array<int, 8> array = { 0, 1, 2, 3, 4, 5, 6, 7 };
    EXPECT(array.first_index_of(0) == 0u);
    EXPECT(array.first_index_of(4) == 4u);
    EXPECT(array.first_index_of(7) == 7u);
    EXPECT(!array.first_index_of(42).has_value());
}

TEST_CASE(to_array)
{
    constexpr auto array = to_array<u8>({ 0, 2, 1 });
    static_assert(array.size() == 3);
    static_assert(array[0] == 0);
    static_assert(array[1] == 2);
    static_assert(array[2] == 1);
}
