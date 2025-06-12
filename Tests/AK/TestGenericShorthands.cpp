/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/GenericShorthands.h>

TEST_CASE(first_is_one_of)
{
    // Finds item if in list of arguments
    static_assert(first_is_one_of(3, 1, 2, 3, 4, 5));
    EXPECT(first_is_one_of(3, 1, 2, 3, 4, 5));

    // Finds item when single value in list
    static_assert(first_is_one_of(42, 42));
    EXPECT(first_is_one_of(42, 42));

    // Finds item when duplicate values in list
    static_assert(first_is_one_of(42, 42, 42));
    EXPECT(first_is_one_of(42, 42, 42));

    // Doesn't find items not in list
    static_assert(!first_is_one_of(10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_one_of(10, 1, 2, 3, 4, 5));

    // Doesn't find item when single value in list
    static_assert(!first_is_one_of(10, 1));
    EXPECT(!first_is_one_of(10, 1));

    // Doesn't find items when duplicate values in list
    static_assert(!first_is_one_of(10, 1, 1));
    EXPECT(!first_is_one_of(10, 1, 1));

    // Doesn't find item when empty list
    static_assert(!first_is_one_of(10));
    EXPECT(!first_is_one_of(10));
}

TEST_CASE(first_is_smaller_or_equal_than_one_of)
{
    // Finds smaller items
    static_assert(first_is_smaller_or_equal_than_one_of(1, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_or_equal_than_one_of(1, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_or_equal_than_one_of(-10, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_or_equal_than_one_of(-10, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_or_equal_than_one_of(42, 43));
    EXPECT(first_is_smaller_or_equal_than_one_of(42, 43));

    // Find equal items
    static_assert(first_is_smaller_or_equal_than_one_of(1, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_or_equal_than_one_of(1, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_or_equal_than_one_of(-2, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_or_equal_than_one_of(-2, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_or_equal_than_one_of(42, 42));
    EXPECT(first_is_smaller_or_equal_than_one_of(42, 42));

    // Doesn't find larger items
    static_assert(!first_is_smaller_or_equal_than_one_of(10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_smaller_or_equal_than_one_of(10, 1, 2, 3, 4, 5));

    // Doesn't find item when empty list
    static_assert(!first_is_smaller_or_equal_than_one_of(10));
    EXPECT(!first_is_smaller_or_equal_than_one_of(10));
}

TEST_CASE(first_is_smaller_than_one_of)
{
    // Finds smaller items
    static_assert(first_is_smaller_than_one_of(1, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_than_one_of(1, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_than_one_of(-10, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_than_one_of(-10, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_than_one_of(42, 43));
    EXPECT(first_is_smaller_than_one_of(42, 43));

    // Doesn't find equal items
    static_assert(!first_is_smaller_than_one_of(5, 1, -2, 3, -4, 5));
    EXPECT(!first_is_smaller_than_one_of(5, 1, -2, 3, -4, 5));

    // Doesn't find larger items
    static_assert(!first_is_smaller_than_one_of(10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_smaller_than_one_of(10, 1, 2, 3, 4, 5));

    // Doesn't find item when empty list
    static_assert(!first_is_smaller_than_one_of(10));
    EXPECT(!first_is_smaller_than_one_of(10));
}

TEST_CASE(first_is_smaller_or_equal_than_all_of)
{
    // Finds smaller than all items
    static_assert(first_is_smaller_or_equal_than_all_of(-10, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_or_equal_than_all_of(-10, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_or_equal_than_all_of(42, 43));
    EXPECT(first_is_smaller_or_equal_than_all_of(42, 43));

    // Find equal items
    static_assert(first_is_smaller_or_equal_than_all_of(42, 42));
    EXPECT(first_is_smaller_or_equal_than_all_of(42, 42));

    // Match on empty list
    static_assert(first_is_smaller_or_equal_than_all_of(10));
    EXPECT(first_is_smaller_or_equal_than_all_of(10));

    // Doesn't find smaller than some items
    static_assert(!first_is_smaller_or_equal_than_all_of(1, 1, -2, 3, -4, 5));
    EXPECT(!first_is_smaller_or_equal_than_all_of(1, 1, -2, 3, -4, 5));

    // Doesn't find larger items
    static_assert(!first_is_smaller_or_equal_than_all_of(10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_smaller_or_equal_than_all_of(10, 1, 2, 3, 4, 5));
}

TEST_CASE(first_is_smaller_than_all_of)
{
    // Finds smaller than all items
    static_assert(first_is_smaller_than_all_of(-10, 1, -2, 3, -4, 5));
    EXPECT(first_is_smaller_than_all_of(-10, 1, -2, 3, -4, 5));

    static_assert(first_is_smaller_than_all_of(42, 43));
    EXPECT(first_is_smaller_than_all_of(42, 43));

    // Match on empty list
    static_assert(first_is_smaller_than_all_of(10));
    EXPECT(first_is_smaller_than_all_of(10));

    // Doesn't find equal items
    static_assert(!first_is_smaller_than_all_of(42, 42));
    EXPECT(!first_is_smaller_than_all_of(42, 42));

    // Doesn't find smaller than some items
    static_assert(!first_is_smaller_than_all_of(1, 1, -2, 3, -4, 5));
    EXPECT(!first_is_smaller_than_all_of(1, 1, -2, 3, -4, 5));

    // Doesn't find larger items
    static_assert(!first_is_smaller_than_all_of(10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_smaller_than_all_of(10, 1, 2, 3, 4, 5));
}

TEST_CASE(first_is_larger_or_equal_than_one_of)
{
    // Finds larger items
    static_assert(first_is_larger_or_equal_than_one_of(1, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_or_equal_than_one_of(1, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_or_equal_than_one_of(10, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_or_equal_than_one_of(10, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_or_equal_than_one_of(44, 43));
    EXPECT(first_is_larger_or_equal_than_one_of(44, 43));

    // Finds equal items
    static_assert(first_is_larger_or_equal_than_one_of(1, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_or_equal_than_one_of(1, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_or_equal_than_one_of(-2, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_or_equal_than_one_of(-2, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_or_equal_than_one_of(42, 42));
    EXPECT(first_is_larger_or_equal_than_one_of(42, 42));

    // Doesn't find smaller items
    static_assert(!first_is_larger_or_equal_than_one_of(-10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_larger_or_equal_than_one_of(-10, 1, 2, 3, 4, 5));

    // Doesn't find item when empty list
    static_assert(!first_is_larger_or_equal_than_one_of(10));
    EXPECT(!first_is_larger_or_equal_than_one_of(10));
}

TEST_CASE(first_is_larger_than_one_of)
{
    // Finds larger items
    static_assert(first_is_larger_than_one_of(1, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_than_one_of(1, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_than_one_of(10, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_than_one_of(10, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_than_one_of(44, 43));
    EXPECT(first_is_larger_than_one_of(44, 43));

    // Doesn't find equal items
    static_assert(!first_is_larger_than_one_of(-4, 1, -2, 3, -4, 5));
    EXPECT(!first_is_larger_than_one_of(-4, 1, -2, 3, -4, 5));

    // Doesn't find smaller items
    static_assert(!first_is_larger_than_one_of(-10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_larger_than_one_of(-10, 1, 2, 3, 4, 5));

    // Doesn't find item when empty list
    static_assert(!first_is_larger_than_one_of(10));
    EXPECT(!first_is_larger_than_one_of(10));
}

TEST_CASE(first_is_equal_to_all_of)
{
    static_assert(first_is_equal_to_all_of(1));
    EXPECT(first_is_equal_to_all_of(1));

    static_assert(first_is_equal_to_all_of(1, 1));
    EXPECT(first_is_equal_to_all_of(1, 1));

    static_assert(!first_is_equal_to_all_of(1, 2));
    EXPECT(!first_is_equal_to_all_of(1, 2));

    static_assert(!first_is_equal_to_all_of(1, 1, 2));
    EXPECT(!first_is_equal_to_all_of(1, 1, 2));

    static_assert(!first_is_equal_to_all_of(2, 1, 1));
    EXPECT(!first_is_equal_to_all_of(2, 1, 1));

    static_assert(!first_is_equal_to_all_of(2, 2, 1));
    EXPECT(!first_is_equal_to_all_of(2, 2, 1));
}

TEST_CASE(first_is_larger_or_equal_than_all_of)
{
    // Finds larger than all items
    static_assert(first_is_larger_or_equal_than_all_of(10, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_or_equal_than_all_of(10, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_or_equal_than_all_of(44, 43));
    EXPECT(first_is_larger_or_equal_than_all_of(44, 43));

    // Find equal items
    static_assert(first_is_larger_or_equal_than_all_of(42, 42));
    EXPECT(first_is_larger_or_equal_than_all_of(42, 42));

    // Match on empty list
    static_assert(first_is_larger_or_equal_than_all_of(10));
    EXPECT(first_is_larger_or_equal_than_all_of(10));

    // Doesn't find larger than some items
    static_assert(!first_is_larger_or_equal_than_all_of(1, 1, -2, 3, -4, 5));
    EXPECT(!first_is_larger_or_equal_than_all_of(1, 1, -2, 3, -4, 5));

    // Doesn't find smaller items
    static_assert(!first_is_larger_or_equal_than_all_of(-10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_larger_or_equal_than_all_of(-10, 1, 2, 3, 4, 5));
}

TEST_CASE(first_is_larger_than_all_of)
{
    // Finds larger than all items
    static_assert(first_is_larger_than_all_of(10, 1, -2, 3, -4, 5));
    EXPECT(first_is_larger_than_all_of(10, 1, -2, 3, -4, 5));

    static_assert(first_is_larger_than_all_of(44, 43));
    EXPECT(first_is_larger_than_all_of(44, 43));

    // Match on empty list
    static_assert(first_is_larger_than_all_of(10));
    EXPECT(first_is_larger_than_all_of(10));

    // Doesn't find equal items
    static_assert(!first_is_larger_than_all_of(42, 42));
    EXPECT(!first_is_larger_than_all_of(42, 42));

    // Doesn't find larger than some items
    static_assert(!first_is_larger_than_all_of(1, 1, -2, 3, -4, 5));
    EXPECT(!first_is_larger_than_all_of(1, 1, -2, 3, -4, 5));

    // Doesn't find smaller items
    static_assert(!first_is_larger_than_all_of(-10, 1, 2, 3, 4, 5));
    EXPECT(!first_is_larger_than_all_of(-10, 1, 2, 3, 4, 5));
}
