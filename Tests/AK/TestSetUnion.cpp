/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/SetUnion.h>
#include <AK/Vector.h>

TEST_CASE(appends_shared_elements_once)
{
    Vector<int> left { 1, 3, 5, 9 };
    Array<int, 4> right_storage { 2, 3, 5, 6 };
    ReadonlySpan<int> right = right_storage;
    Vector<int> output;
    set_union(left, right, output);
    EXPECT_EQ(output, (Vector<int> { 1, 2, 3, 5, 6, 9 }));
}

TEST_CASE(takes_shared_elements_from_the_left)
{
    struct Item {
        int key;
        char side;
        bool operator==(Item const&) const = default;
    };
    Vector<Item> left { { 1, 'l' }, { 2, 'l' } };
    Vector<Item> right { { 2, 'r' }, { 3, 'r' } };
    Vector<Item> output;
    set_union(left, right, output, [](Item const& a, Item const& b) { return a.key < b.key; });
    EXPECT_EQ(output, (Vector<Item> { { 1, 'l' }, { 2, 'l' }, { 3, 'r' } }));
}

TEST_CASE(merges_with_a_comparator)
{
    Vector<int> left { 9, 5, 1 };
    Vector<int> right { 7, 5, 2 };
    Vector<int> output;
    set_union(left, right, output, [](int a, int b) { return a > b; });
    EXPECT_EQ(output, (Vector<int> { 9, 7, 5, 2, 1 }));
}

TEST_CASE(merges_with_an_empty_side)
{
    Vector<int> empty;
    Vector<int> values { 4, 8 };
    Vector<int> output;
    set_union(empty, values, output);
    EXPECT_EQ(output, values);
    output.clear();
    set_union(values, empty, output);
    EXPECT_EQ(output, values);
    output.clear();
    set_union(empty, empty, output);
    EXPECT(output.is_empty());
}

TEST_CASE(rejects_an_output_that_is_an_input)
{
    Vector<int> values { 1, 2 };
    Vector<int> other { 3 };
    EXPECT_DEATH("merging into the left input", set_union(values, other, values));
    EXPECT_DEATH("merging into the right input", set_union(other, values, values));
}
