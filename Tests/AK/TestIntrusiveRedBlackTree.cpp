/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/IntrusiveRedBlackTree.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Random.h>
#include <AK/Vector.h>

class IntrusiveTest {
public:
    IntrusiveTest(int value)
        : m_some_value(value)
    {
    }

    IntrusiveRedBlackTreeNode<int, IntrusiveTest, RawPtr<IntrusiveTest>> m_tree_node;
    int m_some_value;
};
using IntrusiveRBTree = IntrusiveRedBlackTree<&IntrusiveTest::m_tree_node>;

TEST_CASE(construct)
{
    IntrusiveRBTree empty;
    EXPECT(empty.is_empty());
    EXPECT(empty.size() == 0);
}

TEST_CASE(ints)
{
    IntrusiveRBTree test;
    IntrusiveTest first { 10 };
    test.insert(1, first);
    IntrusiveTest second { 20 };
    test.insert(3, second);
    IntrusiveTest third { 30 };
    test.insert(2, third);
    EXPECT_EQ(test.size(), 3u);
    EXPECT_EQ(test.find(3)->m_some_value, 20);
    EXPECT_EQ(test.find(2)->m_some_value, 30);
    EXPECT_EQ(test.find(1)->m_some_value, 10);
    EXPECT(!test.remove(4));
    EXPECT(test.remove(2));
    EXPECT(test.remove(1));
    EXPECT(test.remove(3));
    EXPECT_EQ(test.size(), 0u);
}

TEST_CASE(largest_smaller_than)
{
    IntrusiveRBTree test;
    IntrusiveTest first { 10 };
    test.insert(1, first);
    IntrusiveTest second { 20 };
    test.insert(11, second);
    IntrusiveTest third { 30 };
    test.insert(21, third);
    EXPECT_EQ(test.size(), 3u);
    EXPECT_EQ(test.find_largest_not_above(3)->m_some_value, 10);
    EXPECT_EQ(test.find_largest_not_above(17)->m_some_value, 20);
    EXPECT_EQ(test.find_largest_not_above(22)->m_some_value, 30);
    EXPECT_EQ(test.find_largest_not_above(-5), nullptr);
    VERIFY(test.remove(1));
    VERIFY(test.remove(11));
    VERIFY(test.remove(21));
}

TEST_CASE(key_ordered_iteration)
{
    constexpr auto amount = 10000;
    IntrusiveRBTree test;
    Vector<NonnullOwnPtr<IntrusiveTest>> m_entries;
    Array<int, amount> keys {};

    // generate random key order
    for (int i = 0; i < amount; i++) {
        keys[i] = i;
    }
    for (size_t i = 0; i < amount; i++) {
        swap(keys[i], keys[get_random<size_t>() % amount]);
    }

    // insert random keys
    for (size_t i = 0; i < amount; i++) {
        auto entry = make<IntrusiveTest>(keys[i]);
        test.insert(keys[i], *entry);
        m_entries.append(move(entry));
    }

    // check key-ordered iteration
    int index = 0;
    for (auto& value : test) {
        EXPECT(value.m_some_value == index++);
    }

    // ensure we can remove all of them (aka, tree structure is not destroyed somehow)
    for (size_t i = 0; i < amount; i++) {
        EXPECT(test.remove(i));
    }
}

TEST_CASE(clear)
{
    IntrusiveRBTree test;
    Vector<NonnullOwnPtr<IntrusiveTest>> m_entries;
    for (size_t i = 0; i < 1000; i++) {
        auto entry = make<IntrusiveTest>(i);
        test.insert(i, *entry);
        m_entries.append(move(entry));
    }
    test.clear();
    EXPECT_EQ(test.size(), 0u);
}

class IntrusiveRefPtrTest : public RefCounted<IntrusiveRefPtrTest> {
public:
    IntrusiveRefPtrTest()
    {
    }

    IntrusiveRedBlackTreeNode<int, IntrusiveRefPtrTest, RefPtr<IntrusiveRefPtrTest>> m_tree_node;
};
using IntrusiveRefPtrRBTree = IntrusiveRedBlackTree<&IntrusiveRefPtrTest::m_tree_node>;

TEST_CASE(intrusive_ref_ptr_no_ref_leaks)
{
    auto item = adopt_ref(*new IntrusiveRefPtrTest());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveRefPtrRBTree ref_tree;

    ref_tree.insert(0, *item);
    EXPECT_EQ(2u, item->ref_count());

    ref_tree.remove(0);
    EXPECT_EQ(1u, item->ref_count());
}

TEST_CASE(intrusive_ref_ptr_clear)
{
    auto item = adopt_ref(*new IntrusiveRefPtrTest());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveRefPtrRBTree ref_tree;

    ref_tree.insert(0, *item);
    EXPECT_EQ(2u, item->ref_count());

    ref_tree.clear();
    EXPECT_EQ(1u, item->ref_count());
}

TEST_CASE(intrusive_ref_ptr_destructor)
{
    auto item = adopt_ref(*new IntrusiveRefPtrTest());
    EXPECT_EQ(1u, item->ref_count());

    {
        IntrusiveRefPtrRBTree ref_tree;
        ref_tree.insert(0, *item);
        EXPECT_EQ(2u, item->ref_count());
    }

    EXPECT_EQ(1u, item->ref_count());
}

class IntrusiveNonnullRefPtrTest : public RefCounted<IntrusiveNonnullRefPtrTest> {
public:
    IntrusiveNonnullRefPtrTest()
    {
    }

    IntrusiveRedBlackTreeNode<int, IntrusiveNonnullRefPtrTest, NonnullRefPtr<IntrusiveNonnullRefPtrTest>> m_tree_node;
};
using IntrusiveNonnullRefPtrRBTree = IntrusiveRedBlackTree<&IntrusiveNonnullRefPtrTest::m_tree_node>;

TEST_CASE(intrusive_nonnull_ref_ptr_intrusive)
{
    auto item = adopt_ref(*new IntrusiveNonnullRefPtrTest());
    EXPECT_EQ(1u, item->ref_count());
    IntrusiveNonnullRefPtrRBTree nonnull_ref_tree;

    nonnull_ref_tree.insert(0, *item);
    EXPECT_EQ(2u, item->ref_count());
    EXPECT(!nonnull_ref_tree.is_empty());

    nonnull_ref_tree.remove(0);
    EXPECT_EQ(1u, item->ref_count());

    EXPECT(nonnull_ref_tree.is_empty());
}
