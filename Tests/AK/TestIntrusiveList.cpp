/*
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/IntrusiveList.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>

class IntrusiveTestItem {
public:
    IntrusiveTestItem() = default;
    IntrusiveListNode<IntrusiveTestItem> m_list_node;
};
using IntrusiveTestList = IntrusiveList<&IntrusiveTestItem::m_list_node>;

TEST_CASE(construct)
{
    IntrusiveTestList empty;
    EXPECT(empty.is_empty());
}

TEST_CASE(insert)
{
    IntrusiveTestList list;
    list.append(*new IntrusiveTestItem());

    EXPECT(!list.is_empty());

    delete list.take_last();
}

TEST_CASE(insert_before)
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

TEST_CASE(enumeration)
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

TEST_CASE(intrusive_ref_ptr_no_ref_leaks)
{
    auto item = adopt_ref(*new IntrusiveRefPtrItem());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveRefPtrList ref_list;

    ref_list.append(*item);
    EXPECT_EQ(2u, item->ref_count());

    ref_list.remove(*item);
    EXPECT_EQ(1u, item->ref_count());
}

TEST_CASE(intrusive_ref_ptr_clear)
{
    auto item = adopt_ref(*new IntrusiveRefPtrItem());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveRefPtrList ref_list;

    ref_list.append(*item);
    EXPECT_EQ(2u, item->ref_count());

    ref_list.clear();
    EXPECT_EQ(1u, item->ref_count());
}

TEST_CASE(intrusive_ref_ptr_destructor)
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

TEST_CASE(intrusive_nonnull_ref_ptr_intrusive)
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

TEST_CASE(destroy_nonempty_intrusive_list)
{
    IntrusiveNonnullRefPtrList nonnull_ref_list;
    nonnull_ref_list.append(adopt_ref(*new IntrusiveNonnullRefPtrItem));
}
