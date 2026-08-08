/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibTest/TestCase.h>
#include <LibWebView/AccessibilityTreeManager.h>

namespace {

WebView::AccessibilityNodeData make_node(i64 id, i64 parent_id, Vector<i64> child_ids = {})
{
    WebView::AccessibilityNodeData node;
    node.id = id;
    node.parent_id = parent_id;
    node.child_ids = move(child_ids);
    return node;
}

// Walk child_ids from the root using the test's own visited set — so the walk here can never hang,
// even if update_tree() failed to sanitize — and assert the stored tree is acyclic and that no node
// is reachable through more than one parent. Also cross-checks child_ids against parent_id. Returns
// the set of reachable ids.
HashTable<i64> assert_valid_tree_and_collect(WebView::AccessibilityTreeManager const& manager)
{
    HashTable<i64> seen;
    auto root_id = manager.root_id();
    if (root_id == -1)
        return seen;

    Vector<i64> stack;
    stack.append(root_id);
    while (!stack.is_empty()) {
        auto id = stack.take_last();
        EXPECT(!seen.contains(id)); // a repeat visit would mean a cycle or a shared child survived
        seen.set(id);

        auto const* node = manager.node(id);
        EXPECT(node != nullptr);
        if (!node)
            continue;
        for (auto child_id : node->child_ids) {
            auto const* child = manager.node(child_id);
            EXPECT(child != nullptr); // no dangling child ids
            if (child)
                EXPECT_EQ(child->parent_id, id); // child_ids and parent_id agree
            stack.append(child_id);
        }
    }
    return seen;
}

}

TEST_CASE(well_formed_tree_is_preserved)
{
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1, 2 }));
    nodes.append(make_node(1, 0, { 3 }));
    nodes.append(make_node(2, 0));
    nodes.append(make_node(3, 1));
    manager.update_tree(move(nodes));

    EXPECT_EQ(manager.root_id(), 0);
    auto reachable = assert_valid_tree_and_collect(manager);
    EXPECT_EQ(reachable.size(), 4u);
    EXPECT_EQ(manager.node(0)->child_ids, (Vector<i64> { 1, 2 }));
    EXPECT_EQ(manager.node(1)->child_ids, (Vector<i64> { 3 }));
}

TEST_CASE(cycle_in_child_ids_is_broken)
{
    // 0 -> 1 -> 2 -> 0 : a back edge to the root. The production traversals must terminate.
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1 }));
    nodes.append(make_node(1, 0, { 2 }));
    nodes.append(make_node(2, 1, { 0 }));
    manager.update_tree(move(nodes));

    // These would hang / stack-overflow on an unsanitized cyclic tree.
    (void)manager.text_leaves_in_order();
    (void)manager.hit_test({ 0, 0 });

    auto reachable = assert_valid_tree_and_collect(manager);
    EXPECT_EQ(reachable.size(), 3u);
    EXPECT_EQ(manager.node(2)->child_ids.size(), 0u); // the back edge to 0 was dropped
}

TEST_CASE(self_loop_is_dropped)
{
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1 }));
    nodes.append(make_node(1, 0, { 1 })); // lists itself as its own child
    manager.update_tree(move(nodes));

    (void)manager.text_leaves_in_order();
    auto reachable = assert_valid_tree_and_collect(manager);
    EXPECT_EQ(reachable.size(), 2u);
    EXPECT_EQ(manager.node(1)->child_ids.size(), 0u);
}

TEST_CASE(dangling_child_id_is_dropped)
{
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1, 99 })); // 99 was never sent
    nodes.append(make_node(1, 0));
    manager.update_tree(move(nodes));

    assert_valid_tree_and_collect(manager);
    EXPECT_EQ(manager.node(0)->child_ids, (Vector<i64> { 1 }));
    EXPECT_EQ(manager.node(99), nullptr);
}

TEST_CASE(multiple_roots_keep_a_single_root)
{
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1 }));
    nodes.append(make_node(1, 0));
    nodes.append(make_node(5, -1, { 6 })); // a second, competing root
    nodes.append(make_node(6, 5));
    manager.update_tree(move(nodes));

    EXPECT_EQ(manager.root_id(), 0);
    auto reachable = assert_valid_tree_and_collect(manager);
    EXPECT_EQ(reachable.size(), 2u);     // only the first root's subtree survives
    EXPECT_EQ(manager.node(5), nullptr); // the orphaned second root is dropped
    EXPECT_EQ(manager.node(6), nullptr);
}

TEST_CASE(shared_child_gets_a_single_parent)
{
    // Both 1 and 2 claim 3 as a child. 3 must end up under exactly one of them.
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1, 2 }));
    nodes.append(make_node(1, 0, { 3 }));
    nodes.append(make_node(2, 0, { 3 }));
    nodes.append(make_node(3, 1));
    manager.update_tree(move(nodes));

    auto reachable = assert_valid_tree_and_collect(manager);
    EXPECT_EQ(reachable.size(), 4u);
    bool under_1 = manager.node(1)->child_ids.contains_slow(3);
    bool under_2 = manager.node(2)->child_ids.contains_slow(3);
    EXPECT_NE(under_1, under_2); // exactly one parent kept 3
}

TEST_CASE(deep_chain_is_depth_capped)
{
    // A chain far deeper than the cap must be truncated rather than overflowing the stack.
    WebView::AccessibilityTreeManager manager;
    Vector<WebView::AccessibilityNodeData> nodes;
    constexpr i64 chain_length = 1100;
    for (i64 i = 0; i < chain_length; ++i) {
        auto parent = i == 0 ? -1 : i - 1;
        Vector<i64> children;
        if (i + 1 < chain_length)
            children.append(i + 1);
        nodes.append(make_node(i, parent, move(children)));
    }
    manager.update_tree(move(nodes));

    (void)manager.hit_test({ 0, 0 }); // recursive; would overflow without the cap
    auto reachable = assert_valid_tree_and_collect(manager);
    EXPECT(reachable.size() <= 1001u);                  // root at depth 0 .. deepest kept at the cap
    EXPECT_EQ(manager.node(chain_length - 1), nullptr); // the tail past the cap was dropped
}

TEST_CASE(live_region_walk_terminates_on_cyclic_parents)
{
    WebView::AccessibilityTreeManager manager;
    bool announced = false;
    manager.on_live_region_changed = [&](String const&, String const&) { announced = true; };

    // Seed a non-empty tree so the next update runs the live-region ancestor walk.
    Vector<WebView::AccessibilityNodeData> first;
    first.append(make_node(0, -1, { 1 }));
    first.append(make_node(1, 0));
    manager.update_tree(move(first));

    // A parent_id cycle in the raw input must not hang the ancestor walk (sanitize breaks it first).
    Vector<WebView::AccessibilityNodeData> second;
    second.append(make_node(0, -1, { 1 }));
    second.append(make_node(1, 0, { 2 }));
    second.append(make_node(2, 1, { 1 })); // 2's child points back up at 1
    manager.update_tree(move(second));

    assert_valid_tree_and_collect(manager); // completing at all is the assertion
    (void)announced;
}
