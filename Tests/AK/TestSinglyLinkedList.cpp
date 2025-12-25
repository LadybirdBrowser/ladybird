/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/SinglyLinkedList.h>

static SinglyLinkedList<int> make_list()
{
    SinglyLinkedList<int> list {};
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

TEST_CASE(should_find_mutable_with_predicate)
{
    auto sut = make_list();

    EXPECT_EQ(4, *sut.find_if([](auto const v) { return v == 4; }));

    EXPECT_EQ(sut.end(), sut.find_if([](auto const v) { return v == 42; }));
}

TEST_CASE(should_find_const)
{
    auto const sut = make_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(should_find_const_with_predicate)
{
    auto const sut = make_list();

    EXPECT_EQ(4, *sut.find_if([](auto const v) { return v == 4; }));

    EXPECT_EQ(sut.end(), sut.find_if([](auto const v) { return v == 42; }));
}

TEST_CASE(removal_during_iteration)
{
    auto list = make_list();
    auto size = list.size();

    for (auto it = list.begin(); it != list.end(); --size) {
        VERIFY(list.size() == size);
        it = list.remove(it);
    }
}

static size_t calls_to_increase { 0 };
static size_t calls_to_decrease { 0 };
static size_t calls_to_reset { 0 };
static size_t calls_to_get_size { 0 };

static void setup()
{
    calls_to_increase = 0;
    calls_to_decrease = 0;
    calls_to_reset = 0;
    calls_to_get_size = 0;
}

struct TestSizeCalculationPolicy {
    void increase_size(auto const&) { ++calls_to_increase; }

    void decrease_size(auto const&) { ++calls_to_decrease; }

    void reset() { ++calls_to_reset; }

    size_t size(auto const*) const
    {
        ++calls_to_get_size;
        return 42;
    }
};

TEST_CASE(should_increase_size_when_appending)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(should_decrease_size_when_removing)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    auto begin = list.begin();
    (void)list.remove(begin);
    EXPECT_EQ(1u, calls_to_decrease);
}

TEST_CASE(should_reset_size_when_clearing)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    list.clear();
    EXPECT_EQ(1u, calls_to_reset);
}

TEST_CASE(should_get_size_from_policy)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    EXPECT_EQ(42u, list.size());
    EXPECT_EQ(1u, calls_to_get_size);
}

TEST_CASE(should_decrease_size_when_taking_first)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    list.take_first();
    EXPECT_EQ(1u, calls_to_decrease);
}

TEST_CASE(should_increase_size_when_try_appending)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_append(0));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(should_increase_size_when_try_prepending)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_prepend(0));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(should_increase_size_when_try_inserting_before)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_insert_before(list.begin(), 42));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(should_increase_size_when_try_inserting_after)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_insert_after(list.begin(), 42));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(should_increase_size_when_inserting_before)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.insert_before(list.begin(), 42);
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(should_increase_size_when_inserting_after)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.insert_after(list.begin(), 42);
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_remove_does_not_leave_dangling_iterator)
{
    SinglyLinkedList<int> list;
    list.append(1);
    list.append(2);
    auto it = list.begin();
    it = list.remove(it); // remove first element

    EXPECT(it != list.end());
    EXPECT_EQ(*it, 2);
    it = list.remove(it);
    EXPECT(it == list.end());
    EXPECT(list.is_empty());
}
