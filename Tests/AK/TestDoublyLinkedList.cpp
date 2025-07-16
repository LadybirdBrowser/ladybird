/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/DoublyLinkedList.h>

static DoublyLinkedList<int> make_list()
{
    DoublyLinkedList<int> list {};
    list.append(0);
    list.append(1);
    list.append(2);
    list.append(3);
    list.append(4);
    list.append(5);
    list.append(6);
    list.append(7);
    list.append(8);
    list.append(9);
    return list;
}

TEST_CASE(should_find_mutable)
{
    auto sut = make_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(should_find_const)
{
    auto const sut = make_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(take_first)
{
    auto sut = make_list();

    EXPECT_EQ(0, sut.take_first());
    EXPECT_EQ(1, sut.first());
    EXPECT_EQ(9, sut.last());
    EXPECT_EQ(9u, sut.size());
}

TEST_CASE(take_last)
{
    auto sut = make_list();

    EXPECT_EQ(9, sut.take_last());
    EXPECT_EQ(8, sut.last());
    EXPECT_EQ(0, sut.first());
    EXPECT_EQ(9u, sut.size());
}

TEST_CASE(take_last_all)
{
    auto sut = make_list();

    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(9 - i, sut.take_last());

    EXPECT_EQ(sut.size(), 0u);
}

TEST_CASE(basic_node_cache)
{
    // FIXME: Add more comprehensive tests.
    DoublyLinkedList<int, 2> list;
    list.append(0);
    list.append(1);

    Vector<void*> seen_ptrs;
    for (auto& entry : list)
        seen_ptrs.append(&entry);

    list.take_last();

    list.append(2);
    EXPECT(seen_ptrs.contains_slow(&list.last())); // node cache should have reused the last node
}
