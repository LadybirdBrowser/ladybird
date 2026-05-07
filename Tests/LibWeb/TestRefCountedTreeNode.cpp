/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibTest/TestCase.h>
#include <LibWeb/RefCountedTreeNode.h>

class TestNode
    : public RefCounted<TestNode>
    , public Weakable<TestNode>
    , public Web::RefCountedTreeNode<TestNode> {
public:
    static NonnullRefPtr<TestNode> create(int value)
    {
        return adopt_ref(*new TestNode(value));
    }

    ~TestNode() = default;

    int value() const { return m_value; }

private:
    explicit TestNode(int value)
        : m_value(value)
    {
    }

    int m_value { 0 };
};

TEST_CASE(sibling_links_are_updated_when_appending)
{
    auto root = TestNode::create(0);
    auto first = TestNode::create(1);
    auto second = TestNode::create(2);
    auto third = TestNode::create(3);

    root->append_child(first);
    root->append_child(second);
    root->append_child(third);

    EXPECT_EQ(root->first_child().ptr(), first.ptr());
    EXPECT_EQ(root->last_child().ptr(), third.ptr());

    EXPECT_EQ(first->previous_sibling().ptr(), nullptr);
    EXPECT_EQ(first->next_sibling().ptr(), second.ptr());
    EXPECT_EQ(second->previous_sibling().ptr(), first.ptr());
    EXPECT_EQ(second->next_sibling().ptr(), third.ptr());
    EXPECT_EQ(third->previous_sibling().ptr(), second.ptr());
    EXPECT_EQ(third->next_sibling().ptr(), nullptr);
}

TEST_CASE(sibling_links_are_updated_when_removing)
{
    auto root = TestNode::create(0);
    auto first = TestNode::create(1);
    auto second = TestNode::create(2);
    auto third = TestNode::create(3);

    root->append_child(first);
    root->append_child(second);
    root->append_child(third);

    root->remove_child(*second);
    EXPECT_EQ(first->next_sibling().ptr(), third.ptr());
    EXPECT_EQ(third->previous_sibling().ptr(), first.ptr());
    EXPECT_EQ(second->parent().ptr(), nullptr);
    EXPECT_EQ(second->previous_sibling().ptr(), nullptr);
    EXPECT_EQ(second->next_sibling().ptr(), nullptr);

    root->remove_child(*first);
    EXPECT_EQ(root->first_child().ptr(), third.ptr());
    EXPECT_EQ(third->previous_sibling().ptr(), nullptr);

    root->remove_child(*third);
    EXPECT_EQ(root->first_child().ptr(), nullptr);
    EXPECT_EQ(root->last_child().ptr(), nullptr);
    EXPECT_EQ(third->parent().ptr(), nullptr);
}

TEST_CASE(tree_owns_children_through_forward_links)
{
    WeakPtr<TestNode> first;
    WeakPtr<TestNode> second;
    WeakPtr<TestNode> third;

    {
        auto root = TestNode::create(0);

        auto first_child = TestNode::create(1);
        first = first_child;
        root->append_child(move(first_child));

        auto second_child = TestNode::create(2);
        second = second_child;
        root->append_child(move(second_child));

        auto third_child = TestNode::create(3);
        third = third_child;
        root->append_child(move(third_child));

        EXPECT(first);
        EXPECT(second);
        EXPECT(third);
    }

    EXPECT(first.is_null());
    EXPECT(second.is_null());
    EXPECT(third.is_null());
}

TEST_CASE(removing_child_releases_tree_ownership)
{
    auto root = TestNode::create(0);
    WeakPtr<TestNode> child_weak;

    {
        auto child = TestNode::create(1);
        child_weak = child;
        root->append_child(move(child));
    }

    EXPECT(child_weak);

    {
        auto child = root->first_child();
        root->remove_child(*child);
        EXPECT_EQ(root->first_child().ptr(), nullptr);
        EXPECT(child_weak);
    }

    EXPECT(child_weak.is_null());
}

TEST_CASE(parent_destruction_detaches_children_and_siblings)
{
    auto first = TestNode::create(1);
    auto second = TestNode::create(2);
    auto third = TestNode::create(3);

    {
        auto root = TestNode::create(0);
        root->append_child(first);
        root->append_child(second);
        root->append_child(third);
    }

    EXPECT_EQ(first->parent().ptr(), nullptr);
    EXPECT_EQ(first->next_sibling().ptr(), nullptr);
    EXPECT_EQ(second->parent().ptr(), nullptr);
    EXPECT_EQ(second->previous_sibling().ptr(), nullptr);
    EXPECT_EQ(second->next_sibling().ptr(), nullptr);
    EXPECT_EQ(third->parent().ptr(), nullptr);
    EXPECT_EQ(third->previous_sibling().ptr(), nullptr);
}

TEST_CASE(preorder_traversal_uses_sibling_links)
{
    auto root = TestNode::create(0);
    auto first = TestNode::create(1);
    auto second = TestNode::create(2);
    auto third = TestNode::create(3);
    auto second_child = TestNode::create(4);

    root->append_child(first);
    root->append_child(second);
    root->append_child(third);
    second->append_child(second_child);

    Vector<int> values;
    root->for_each_in_inclusive_subtree([&](TestNode& node) {
        values.append(node.value());
        return Web::TraversalDecision::Continue;
    });

    EXPECT_EQ(values.size(), 5u);
    EXPECT_EQ(values[0], 0);
    EXPECT_EQ(values[1], 1);
    EXPECT_EQ(values[2], 2);
    EXPECT_EQ(values[3], 4);
    EXPECT_EQ(values[4], 3);
}
