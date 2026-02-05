/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/BinaryHeap.h>
#include <AK/ByteString.h>

using namespace Test::Randomized;

TEST_CASE(construct)
{
    BinaryHeap<int, int, 5> empty;
    EXPECT(empty.is_empty());
    EXPECT(empty.size() == 0);
}

TEST_CASE(construct_from_existing)
{
    int keys[] = { 3, 2, 1 };
    char values[] = { 'c', 'b', 'a' };
    BinaryHeap<int, char, 5> from_existing(keys, values, 3);
    EXPECT(from_existing.size() == 3);
    EXPECT_EQ(from_existing.pop_min(), 'a');
    EXPECT_EQ(from_existing.pop_min(), 'b');
    EXPECT_EQ(from_existing.pop_min(), 'c');
}

TEST_CASE(populate_int)
{
    BinaryHeap<int, int, 5> ints;
    ints.insert(1, 10);
    ints.insert(3, 20);
    ints.insert(2, 30);
    EXPECT_EQ(ints.size(), 3u);
    EXPECT_EQ(ints.pop_min(), 10);
    EXPECT_EQ(ints.size(), 2u);
    EXPECT_EQ(ints.pop_min(), 30);
    EXPECT_EQ(ints.size(), 1u);
    EXPECT_EQ(ints.pop_min(), 20);
    EXPECT_EQ(ints.size(), 0u);
}

TEST_CASE(populate_string)
{
    BinaryHeap<int, ByteString, 5> strings;
    strings.insert(1, "ABC");
    strings.insert(2, "DEF");
    EXPECT_EQ(strings.size(), 2u);
    EXPECT_EQ(strings.pop_min(), "ABC");
    EXPECT_EQ(strings.pop_min(), "DEF");
    EXPECT(strings.is_empty());
}

TEST_CASE(large_populate_reverse)
{
    BinaryHeap<int, int, 1024> ints;
    for (int i = 1023; i >= 0; i--) {
        ints.insert(i, i);
    }
    EXPECT_EQ(ints.size(), 1024u);
    for (int i = 0; i < 1024; i++) {
        EXPECT_EQ(ints.peek_min(), i);
        EXPECT_EQ(ints.pop_min(), i);
    }
}

RANDOMIZED_TEST_CASE(pop_min_is_min)
{
    GEN(vec, Gen::vector(1, 10, []() { return Gen::number_u64(); }));

    auto sorted { vec };
    AK::quick_sort(sorted);

    BinaryHeap<u64, u64, 10> heap;

    // insert in a non-sorted order
    for (u64 n : vec) {
        heap.insert(n, n);
    }

    // check in a sorted order
    for (u64 sorted_n : sorted) {
        EXPECT_EQ(heap.pop_min(), sorted_n);
    }
}

RANDOMIZED_TEST_CASE(peek_min_same_as_pop_min)
{
    GEN(vec, Gen::vector(1, 10, []() { return Gen::number_u64(); }));
    BinaryHeap<u64, u64, 10> heap;
    for (u64 n : vec) {
        heap.insert(n, n);
    }

    while (!heap.is_empty()) {
        u64 peeked = heap.peek_min();
        u64 popped = heap.pop_min();
        EXPECT_EQ(peeked, popped);
    }
}
