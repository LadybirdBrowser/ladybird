/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/DoublyLinkedList.h>
#include <AK/IntrusiveList.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/SinglyLinkedList.h>

static SinglyLinkedList<int> make_singly_linked_list()
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

TEST_CASE(singly_linked_list_should_find_mutable)
{
    auto sut = make_singly_linked_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(singly_linked_list_should_find_mutable_with_predicate)
{
    auto sut = make_singly_linked_list();

    EXPECT_EQ(4, *sut.find_if([](auto const v) { return v == 4; }));

    EXPECT_EQ(sut.end(), sut.find_if([](auto const v) { return v == 42; }));
}

TEST_CASE(singly_linked_list_should_find_const)
{
    auto const sut = make_singly_linked_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(singly_linked_list_should_find_const_with_predicate)
{
    auto const sut = make_singly_linked_list();

    EXPECT_EQ(4, *sut.find_if([](auto const v) { return v == 4; }));

    EXPECT_EQ(sut.end(), sut.find_if([](auto const v) { return v == 42; }));
}

TEST_CASE(singly_linked_list_removal_during_iteration)
{
    auto list = make_singly_linked_list();
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

TEST_CASE(singly_linked_list_should_increase_size_when_appending)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_should_decrease_size_when_removing)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    auto begin = list.begin();
    (void)list.remove(begin);
    EXPECT_EQ(1u, calls_to_decrease);
}

TEST_CASE(singly_linked_list_should_reset_size_when_clearing)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    list.clear();
    EXPECT_EQ(1u, calls_to_reset);
}

TEST_CASE(singly_linked_list_should_get_size_from_policy)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    EXPECT_EQ(42u, list.size());
    EXPECT_EQ(1u, calls_to_get_size);
}

TEST_CASE(singly_linked_list_should_decrease_size_when_taking_first)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.append(0);
    list.take_first();
    EXPECT_EQ(1u, calls_to_decrease);
}

TEST_CASE(singly_linked_list_should_increase_size_when_try_appending)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_append(0));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_should_increase_size_when_try_prepending)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_prepend(0));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_should_increase_size_when_try_inserting_before)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_insert_before(list.begin(), 42));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_should_increase_size_when_try_inserting_after)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    MUST(list.try_insert_after(list.begin(), 42));
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_should_increase_size_when_inserting_before)
{
    setup();
    SinglyLinkedList<int, TestSizeCalculationPolicy> list {};
    list.insert_before(list.begin(), 42);
    EXPECT_EQ(1u, calls_to_increase);
}

TEST_CASE(singly_linked_list_should_increase_size_when_inserting_after)
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

TEST_CASE(singly_linked_list_remove_consecutive_mid_list_nodes)
{
    SinglyLinkedList<int> list;
    list.append(1);
    list.append(2);
    list.append(3);
    list.append(4);

    auto it = list.begin();
    ++it;
    it = list.remove(it);
    EXPECT_EQ(*it, 3);
    it = list.remove(it);
    EXPECT_EQ(*it, 4);

    it = list.begin();
    EXPECT_EQ(*it, 1);
    ++it;
    EXPECT_EQ(*it, 4);
    ++it;
    EXPECT(it == list.end());
}

static DoublyLinkedList<int> make_doubly_linked_list()
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

TEST_CASE(doubly_linked_list_should_find_mutable)
{
    auto sut = make_doubly_linked_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(doubly_linked_list_should_find_const)
{
    auto const sut = make_doubly_linked_list();

    EXPECT_EQ(4, *sut.find(4));

    EXPECT_EQ(sut.end(), sut.find(42));
}

TEST_CASE(doubly_linked_list_take_first)
{
    auto sut = make_doubly_linked_list();

    EXPECT_EQ(0, sut.take_first());
    EXPECT_EQ(1, sut.first());
    EXPECT_EQ(9, sut.last());
    EXPECT_EQ(9u, sut.size());
}

TEST_CASE(doubly_linked_list_take_last)
{
    auto sut = make_doubly_linked_list();

    EXPECT_EQ(9, sut.take_last());
    EXPECT_EQ(8, sut.last());
    EXPECT_EQ(0, sut.first());
    EXPECT_EQ(9u, sut.size());
}

TEST_CASE(doubly_linked_list_take_last_all)
{
    auto sut = make_doubly_linked_list();

    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(9 - i, sut.take_last());

    EXPECT_EQ(sut.size(), 0u);
}

TEST_CASE(doubly_linked_list_basic_node_cache)
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

class IntrusiveTestItem {
public:
    IntrusiveTestItem() = default;
    IntrusiveListNode<IntrusiveTestItem> m_list_node;
};
using IntrusiveTestList = IntrusiveList<&IntrusiveTestItem::m_list_node>;

TEST_CASE(intrusive_list_construct)
{
    IntrusiveTestList empty;
    EXPECT(empty.is_empty());
}

TEST_CASE(intrusive_list_insert)
{
    IntrusiveTestList list;
    list.append(*new IntrusiveTestItem());

    EXPECT(!list.is_empty());

    delete list.take_last();
}

TEST_CASE(intrusive_list_insert_before)
{
    IntrusiveTestList list;
    auto two = new IntrusiveTestItem();
    list.append(*two);
    auto zero = new IntrusiveTestItem();
    list.append(*zero);
    auto one = new IntrusiveTestItem();
    list.insert_before(*zero, *one);

    EXPECT_EQ(list.first(), two);
    EXPECT_EQ(list.last(), zero);
    EXPECT(list.contains(*zero));
    EXPECT(list.contains(*one));
    EXPECT(list.contains(*two));

    EXPECT(zero->m_list_node.is_in_list());
    EXPECT(one->m_list_node.is_in_list());
    EXPECT(two->m_list_node.is_in_list());
    EXPECT_EQ(list.size_slow(), 3u);

    while (auto elem = list.take_first()) {
        delete elem;
    }
}

TEST_CASE(intrusive_list_enumeration)
{
    constexpr size_t expected_size = 10;
    IntrusiveTestList list;
    for (size_t i = 0; i < expected_size; i++) {
        list.append(*new IntrusiveTestItem());
    }

    size_t actual_size = 0;
    for (auto& elem : list) {
        (void)elem;
        actual_size++;
    }
    EXPECT_EQ(expected_size, actual_size);
    EXPECT_EQ(expected_size, list.size_slow());

    size_t reverse_actual_size = 0;
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        reverse_actual_size++;
    }
    EXPECT_EQ(expected_size, reverse_actual_size);

    while (auto elem = list.take_first()) {
        delete elem;
    }
}

class IntrusiveRefPtrItem : public RefCounted<IntrusiveRefPtrItem> {
public:
    IntrusiveRefPtrItem() = default;
    IntrusiveListNode<IntrusiveRefPtrItem, RefPtr<IntrusiveRefPtrItem>> m_list_node;
};
using IntrusiveRefPtrList = IntrusiveList<&IntrusiveRefPtrItem::m_list_node>;

TEST_CASE(intrusive_list_intrusive_ref_ptr_no_ref_leaks)
{
    auto item = adopt_ref(*new IntrusiveRefPtrItem());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveRefPtrList ref_list;

    ref_list.append(*item);
    EXPECT_EQ(2u, item->ref_count());

    ref_list.remove(*item);
    EXPECT_EQ(1u, item->ref_count());
}

TEST_CASE(intrusive_list_intrusive_ref_ptr_clear)
{
    auto item = adopt_ref(*new IntrusiveRefPtrItem());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveRefPtrList ref_list;

    ref_list.append(*item);
    EXPECT_EQ(2u, item->ref_count());

    ref_list.clear();
    EXPECT_EQ(1u, item->ref_count());
}

TEST_CASE(intrusive_list_intrusive_ref_ptr_destructor)
{
    auto item = adopt_ref(*new IntrusiveRefPtrItem());
    EXPECT_EQ(1u, item->ref_count());

    {
        IntrusiveRefPtrList ref_list;
        ref_list.append(*item);
        EXPECT_EQ(2u, item->ref_count());
    }

    EXPECT_EQ(1u, item->ref_count());
}

class IntrusiveNonnullRefPtrItem : public RefCounted<IntrusiveNonnullRefPtrItem> {
public:
    IntrusiveNonnullRefPtrItem() = default;
    IntrusiveListNode<IntrusiveNonnullRefPtrItem, NonnullRefPtr<IntrusiveNonnullRefPtrItem>> m_list_node;
};
using IntrusiveNonnullRefPtrList = IntrusiveList<&IntrusiveNonnullRefPtrItem::m_list_node>;

TEST_CASE(intrusive_list_intrusive_nonnull_ref_ptr)
{
    auto item = adopt_ref(*new IntrusiveNonnullRefPtrItem());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveNonnullRefPtrList nonnull_ref_list;

    nonnull_ref_list.append(*item);
    EXPECT_EQ(2u, item->ref_count());
    EXPECT(!nonnull_ref_list.is_empty());

    nonnull_ref_list.remove(*item);
    EXPECT_EQ(1u, item->ref_count());

    EXPECT(nonnull_ref_list.is_empty());
}

TEST_CASE(intrusive_list_destroy_nonempty)
{
    IntrusiveNonnullRefPtrList nonnull_ref_list;
    nonnull_ref_list.append(adopt_ref(*new IntrusiveNonnullRefPtrItem));
}
