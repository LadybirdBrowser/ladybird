/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Variant.h>
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

WebView::AccessibilityNodeData make_named_node(i64 id, i64 parent_id, StringView name, StringView live = {}, Vector<i64> child_ids = {})
{
    auto node = make_node(id, parent_id, move(child_ids));
    node.name = MUST(String::from_utf8(name));
    if (!live.is_empty())
        node.live = MUST(String::from_utf8(live));
    return node;
}

// Walk child_ids from the root using the test's own visited set — so the walk here can never hang, even if
// update_tree() failed to sanitize — and assert the stored tree is acyclic and that no node is reachable through more
// than one parent. Also cross-checks child_ids against parent_id. Returns the set of reachable ids.
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
        // EXPECT records a failure but does not stop, so re-pushing an already-seen node's children would spin
        // forever on a surviving cycle. Record the repeat visit, then skip it, and the walk still terminates.
        if (seen.contains(id)) {
            EXPECT(false); // a repeat visit means a cycle or a shared child survived
            continue;
        }
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

// A live region that is already on the page, then gains a child: the classic status-message pattern. The new
// child has no entry in the previous tree, so it must not be skipped just for being new.
TEST_CASE(insertion_into_existing_live_region_is_announced)
{
    WebView::AccessibilityTreeManager manager;
    Vector<String> announcements;
    manager.on_live_region_changed = [&](String const& text, String const&) { announcements.append(text); };

    Vector<WebView::AccessibilityNodeData> first;
    first.append(make_node(0, -1, { 1 }));
    first.append(make_named_node(1, 0, ""sv, "polite"sv));
    manager.update_tree(move(first));
    EXPECT(announcements.is_empty());

    Vector<WebView::AccessibilityNodeData> second;
    second.append(make_node(0, -1, { 1 }));
    second.append(make_named_node(1, 0, ""sv, "polite"sv, { 2 }));
    second.append(make_named_node(2, 1, "Message received"sv));
    manager.update_tree(move(second));

    EXPECT_EQ(announcements.size(), 1u);
    EXPECT_EQ(announcements[0], "Message received"sv);
}

// A live region that arrives already populated announces nothing: adding the region is not itself a change.
TEST_CASE(newly_added_live_region_does_not_announce_its_initial_content)
{
    WebView::AccessibilityTreeManager manager;
    Vector<String> announcements;
    manager.on_live_region_changed = [&](String const& text, String const&) { announcements.append(text); };

    Vector<WebView::AccessibilityNodeData> first;
    first.append(make_node(0, -1, { 1 }));
    first.append(make_named_node(1, 0, "Static"sv));
    manager.update_tree(move(first));

    Vector<WebView::AccessibilityNodeData> second;
    second.append(make_node(0, -1, { 1, 2 }));
    second.append(make_named_node(1, 0, "Static"sv));
    second.append(make_named_node(2, 0, ""sv, "polite"sv, { 3 }));
    second.append(make_named_node(3, 2, "Born with this"sv));
    manager.update_tree(move(second));

    EXPECT(announcements.is_empty());
}

// Inserting a subtree can present the same text at several new nodes; the region announces once, not per node.
TEST_CASE(subtree_insertion_into_live_region_announces_once)
{
    WebView::AccessibilityTreeManager manager;
    Vector<String> announcements;
    manager.on_live_region_changed = [&](String const& text, String const&) { announcements.append(text); };

    Vector<WebView::AccessibilityNodeData> first;
    first.append(make_node(0, -1, { 1 }));
    first.append(make_named_node(1, 0, ""sv, "polite"sv));
    manager.update_tree(move(first));

    Vector<WebView::AccessibilityNodeData> second;
    second.append(make_node(0, -1, { 1 }));
    second.append(make_named_node(1, 0, ""sv, "polite"sv, { 2 }));
    second.append(make_named_node(2, 1, "Alert text"sv, {}, { 3 }));
    second.append(make_named_node(3, 2, "Alert text"sv));
    manager.update_tree(move(second));

    EXPECT_EQ(announcements.size(), 1u);
}

// aria-live="off" stays silent even for content inserted into it.
TEST_CASE(insertion_into_off_live_region_is_not_announced)
{
    WebView::AccessibilityTreeManager manager;
    Vector<String> announcements;
    manager.on_live_region_changed = [&](String const& text, String const&) { announcements.append(text); };

    Vector<WebView::AccessibilityNodeData> first;
    first.append(make_node(0, -1, { 1 }));
    first.append(make_named_node(1, 0, ""sv, "off"sv));
    manager.update_tree(move(first));

    Vector<WebView::AccessibilityNodeData> second;
    second.append(make_node(0, -1, { 1 }));
    second.append(make_named_node(1, 0, ""sv, "off"sv, { 2 }));
    second.append(make_named_node(2, 1, "Ignored"sv));
    manager.update_tree(move(second));

    EXPECT(announcements.is_empty());
}

namespace {

// Every node of the tree held, in depth-first order, as "id(parent)[children] name" - so a test can check a tree
// exactly, and check that a rejected delta left it exactly as it was.
ByteString describe_tree(WebView::AccessibilityTreeManager const& manager)
{
    StringBuilder builder;
    Vector<i64> stack;
    if (manager.root_id() != -1)
        stack.append(manager.root_id());
    while (!stack.is_empty()) {
        auto const* node = manager.node(stack.take_last());
        if (!node) {
            builder.append("missing "sv);
            continue;
        }
        builder.appendff("{}({})[", node->id, node->parent_id);
        builder.join(',', node->child_ids);
        builder.appendff("]{} ", node->name);
        for (size_t i = node->child_ids.size(); i > 0; --i)
            stack.append(node->child_ids[i - 1]);
    }
    return builder.to_byte_string();
}

// 0 holds 1 and 2; 1 holds 3 and 4. 0 is a live region, so any change a delta makes to a name would announce.
void seed_base_tree(WebView::AccessibilityTreeManager& manager)
{
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_named_node(0, -1, ""sv, "polite"sv, { 1, 2 }));
    nodes.append(make_node(1, 0, { 3, 4 }));
    nodes.append(make_node(2, 0));
    nodes.append(make_node(3, 1));
    nodes.append(make_node(4, 1));
    manager.update_tree(move(nodes));
}

}

TEST_CASE(delta_changes_inserts_moves_and_removes_nodes)
{
    WebView::AccessibilityTreeManager manager;
    seed_base_tree(manager);
    auto generation = manager.generation();

    // Rename 3, move 4 from 1 to 2, and give 2 a new child 5. 4's own data doesn't change, and 5 claims a wrong parent:
    // The parent comes from the child lists alone.
    WebView::AccessibilityTreeDelta delta;
    delta.changed_nodes.append(make_node(1, 0, { 3 }));
    delta.changed_nodes.append(make_node(2, 0, { 4, 5 }));
    delta.changed_nodes.append(make_named_node(3, 1, "renamed"sv));
    delta.changed_nodes.append(make_node(5, 3));
    auto removed = manager.apply_delta(delta);
    EXPECT(removed.has_value());
    EXPECT(removed->is_empty());
    EXPECT_EQ(describe_tree(manager), "0(-1)[1,2] 1(0)[3] 3(1)[]renamed 2(0)[4,5] 4(2)[] 5(2)[] "sv);
    EXPECT_EQ(manager.generation(), generation + 1);

    // Remove 2 with its subtree.
    WebView::AccessibilityTreeDelta removal;
    removal.changed_nodes.append(make_node(0, -1, { 1 }));
    removal.removed_ids = { 2, 4, 5 };
    removed = manager.apply_delta(removal);
    EXPECT(removed.has_value());
    EXPECT_EQ(removed->size(), 3u);
    EXPECT_EQ(describe_tree(manager), "0(-1)[1] 1(0)[3] 3(1)[]renamed "sv);
    EXPECT(!manager.node(2) && !manager.node(4) && !manager.node(5));
    EXPECT_EQ(manager.generation(), generation + 2);
}

TEST_CASE(delta_that_does_not_fit_changes_nothing)
{
    struct Case {
        StringView description;
        Function<void(WebView::AccessibilityTreeDelta&)> build;
    };
    Vector<Case> cases;
    cases.append({ "removes a node the tree doesn't hold"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_node(2, 0));
                      delta.removed_ids = { 99 };
                  } });
    cases.append({ "removes the root"sv, [](auto& delta) { delta.removed_ids = { 0 }; } });
    cases.append({ "removes a node its unchanged parent still lists"sv, [](auto& delta) { delta.removed_ids = { 3 }; } });
    cases.append({ "removes a node but not its children"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_node(0, -1, { 2 }));
                      delta.removed_ids = { 1 };
                  } });
    cases.append({ "adds a node no child list holds"sv, [](auto& delta) { delta.changed_nodes.append(make_node(9, 2)); } });
    cases.append({ "lists one node in two child lists"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_node(1, 0, { 3, 4 }));
                      delta.changed_nodes.append(make_node(2, 0, { 4 }));
                  } });
    cases.append({ "lists one node twice in a child list"sv, [](auto& delta) { delta.changed_nodes.append(make_node(2, 0, { 3, 3 })); } });
    cases.append({ "moves a node its unchanged old parent still lists"sv, [](auto& delta) { delta.changed_nodes.append(make_node(2, 0, { 4 })); } });
    cases.append({ "drops a child without removing or moving it"sv, [](auto& delta) { delta.changed_nodes.append(make_node(1, 0, { 3 })); } });
    cases.append({ "forms a cycle"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_node(0, -1, { 2 }));
                      delta.changed_nodes.append(make_node(3, 1, { 1 }));
                  } });
    cases.append({ "makes a node its own child"sv, [](auto& delta) { delta.changed_nodes.append(make_node(2, 0, { 2 })); } });
    cases.append({ "lists a node that doesn't exist"sv, [](auto& delta) { delta.changed_nodes.append(make_node(2, 0, { 77 })); } });
    cases.append({ "lists the root as a child"sv, [](auto& delta) { delta.changed_nodes.append(make_node(2, 0, { 0 })); } });
    cases.append({ "changes one node twice"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_named_node(2, 0, "a"sv));
                      delta.changed_nodes.append(make_named_node(2, 0, "b"sv));
                  } });
    cases.append({ "changes and removes the same node"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_node(1, 0, { 4 }));
                      delta.changed_nodes.append(make_named_node(3, 1, "a"sv));
                      delta.removed_ids = { 3 };
                  } });
    cases.append({ "changes a node with a negative ID"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_node(2, 0, { -1 }));
                      delta.changed_nodes.append(make_named_node(-1, 2, "negative"sv));
                  } });
    cases.append({ "renames a node and removes one that doesn't exist"sv, [](auto& delta) {
                      delta.changed_nodes.append(make_named_node(3, 1, "renamed"sv));
                      delta.removed_ids = { 99 };
                  } });
    cases.append({ "makes the tree too deep"sv, [](auto& delta) {
                      // 2 is at depth 1, so a chain of 1,000 new nodes under it ends at depth 1,001.
                      delta.changed_nodes.append(make_node(2, 0, { 100 }));
                      for (i64 id = 100; id < 1100; ++id)
                          delta.changed_nodes.append(make_named_node(id, id - 1, "deep"sv, {}, id + 1 < 1100 ? Vector<i64> { id + 1 } : Vector<i64> {}));
                  } });

    for (auto& test_case : cases) {
        WebView::AccessibilityTreeManager manager;
        Vector<String> announcements;
        manager.on_live_region_changed = [&](String const& text, String const&) { announcements.append(text); };
        seed_base_tree(manager);
        auto before = describe_tree(manager);
        Vector<WebView::AccessibilityNodeData> base_nodes;
        for (i64 id = 0; id <= 4; ++id)
            base_nodes.append(*manager.node(id));
        auto generation = manager.generation();

        WebView::AccessibilityTreeDelta delta;
        test_case.build(delta);
        auto result = manager.apply_delta(delta);
        if (result.has_value())
            warnln("accepted a delta that {}", test_case.description);
        EXPECT(!result.has_value());
        EXPECT_EQ(describe_tree(manager), before);
        for (auto const& base_node : base_nodes) {
            auto const* node = manager.node(base_node.id);
            EXPECT(node && *node == base_node);
        }
        for (auto const& changed_node : delta.changed_nodes) {
            if (changed_node.id < 0 || changed_node.id > 4)
                EXPECT(!manager.node(changed_node.id));
        }
        EXPECT_EQ(manager.generation(), generation);
        EXPECT(announcements.is_empty());
    }

    // The deepest chain allowed fits: 999 new nodes under 2 end at depth 1,000.
    WebView::AccessibilityTreeManager manager;
    seed_base_tree(manager);
    WebView::AccessibilityTreeDelta delta;
    delta.changed_nodes.append(make_node(2, 0, { 100 }));
    for (i64 id = 100; id < 1099; ++id)
        delta.changed_nodes.append(make_node(id, id - 1, id + 1 < 1099 ? Vector<i64> { id + 1 } : Vector<i64> {}));
    EXPECT(manager.apply_delta(delta).has_value());

    // And with no tree held, there's nothing a delta can apply to.
    WebView::AccessibilityTreeManager empty_manager;
    WebView::AccessibilityTreeDelta any_delta;
    EXPECT(!empty_manager.apply_delta(any_delta).has_value());
}

TEST_CASE(delta_announces_live_region_changes)
{
    WebView::AccessibilityTreeManager manager;
    Vector<String> announcements;
    manager.on_live_region_changed = [&](String const& text, String const&) { announcements.append(text); };

    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1, 2 }));
    nodes.append(make_named_node(1, 0, ""sv, "polite"sv, { 3 }));
    nodes.append(make_node(2, 0));
    nodes.append(make_named_node(3, 1, "first"sv));
    manager.update_tree(move(nodes));

    // A subtree inserted into a region that was already there announces once: the text of its topmost node, which
    // comes first in tree order.
    WebView::AccessibilityTreeDelta insertion;
    insertion.changed_nodes.append(make_named_node(1, 0, ""sv, "polite"sv, { 3, 4 }));
    insertion.changed_nodes.append(make_named_node(4, 1, "inserted"sv, {}, { 5 }));
    insertion.changed_nodes.append(make_named_node(5, 4, "fragment"sv));
    EXPECT(manager.apply_delta(insertion).has_value());
    EXPECT_EQ(announcements.size(), 1u);
    EXPECT_EQ(announcements[0], "inserted"sv);

    // A changed name in the region announces; a node that changes with its name the same doesn't.
    announcements.clear();
    WebView::AccessibilityTreeDelta renaming;
    renaming.changed_nodes.append(make_named_node(3, 1, "second"sv));
    auto moved_but_same_name = make_named_node(4, 1, "inserted"sv, {}, { 5 });
    moved_but_same_name.bounds = { 1, 2, 3, 4 };
    renaming.changed_nodes.append(moved_but_same_name);
    EXPECT(manager.apply_delta(renaming).has_value());
    EXPECT_EQ(announcements.size(), 1u);
    EXPECT_EQ(announcements[0], "second"sv);

    // A region that arrives with its content announces nothing.
    announcements.clear();
    WebView::AccessibilityTreeDelta new_region;
    new_region.changed_nodes.append(make_node(2, 0, { 6 }));
    new_region.changed_nodes.append(make_named_node(6, 2, ""sv, "assertive"sv, { 7 }));
    new_region.changed_nodes.append(make_named_node(7, 6, "born with this"sv));
    EXPECT(manager.apply_delta(new_region).has_value());
    EXPECT(announcements.is_empty());
}

namespace {

// A stand-in for WebContent that sends each whole tree once, and then only what changed in it, found by comparing it
// with the last tree sent: what the manager has to apply.
class Publisher {
public:
    struct WholeTree {
        u64 sequence { 0 };
        Vector<WebView::AccessibilityNodeData> nodes;
    };
    struct Change {
        u64 base_sequence { 0 };
        u64 sequence { 0 };
        WebView::AccessibilityTreeDelta delta;
    };
    struct NothingChanged { };
    using Update = Variant<NothingChanged, WholeTree, Change>;

    Update publish(i64 document_id, Vector<WebView::AccessibilityNodeData> nodes, bool whole_tree_requested)
    {
        auto remember = [&](Vector<WebView::AccessibilityNodeData> const& published) {
            m_document_id = document_id;
            m_published_nodes.clear();
            m_published_nodes.ensure_capacity(published.size());
            for (auto const& node : published)
                m_published_nodes.set(node.id, node);
        };

        if (whole_tree_requested || m_document_id != document_id) {
            remember(nodes);
            return WholeTree { ++m_sequence, move(nodes) };
        }

        // Every node that's new or whose data changed goes in the change, in tree order. A node whose children changed
        // is among them, so a node that moved is listed by its new parent, and dropped by its old one.
        WebView::AccessibilityTreeDelta delta;
        Vector<size_t> new_node_indices;
        size_t surviving_count = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            auto published = m_published_nodes.find(node.id);
            if (published == m_published_nodes.end()) {
                delta.changed_nodes.append(node);
                new_node_indices.append(i);
                continue;
            }
            ++surviving_count;
            if (published->value == node)
                continue;
            delta.changed_nodes.append(node);
            published->value = move(node);
        }

        // Only when fewer nodes of the tree sent survive than it had are any of them gone. (A node moved from still has
        // its ID: Moving an integer copies it.)
        if (surviving_count < m_published_nodes.size()) {
            HashTable<i64> current_ids;
            current_ids.ensure_capacity(nodes.size());
            for (auto const& node : nodes)
                current_ids.set(node.id);
            for (auto const& [id, node] : m_published_nodes) {
                if (!current_ids.contains(id))
                    delta.removed_ids.append(id);
            }
            for (auto id : delta.removed_ids)
                m_published_nodes.remove(id);
        }
        for (auto i : new_node_indices)
            m_published_nodes.set(nodes[i].id, move(nodes[i]));

        if (delta.changed_nodes.is_empty() && delta.removed_ids.is_empty())
            return NothingChanged {};

        auto base_sequence = m_sequence;
        return Change { base_sequence, ++m_sequence, move(delta) };
    }

private:
    Optional<i64> m_document_id;
    HashMap<i64, WebView::AccessibilityNodeData> m_published_nodes;
    u64 m_sequence { 0 };
};

Vector<WebView::AccessibilityNodeData> base_tree_nodes()
{
    Vector<WebView::AccessibilityNodeData> nodes;
    nodes.append(make_node(0, -1, { 1, 2 }));
    nodes.append(make_node(1, 0, { 3 }));
    nodes.append(make_node(2, 0));
    nodes.append(make_node(3, 1));
    return nodes;
}

// Applies what the publisher sent to the manager, as the UI process does, and returns whether the manager took it.
bool deliver(WebView::AccessibilityTreeManager& manager, Publisher::Update update)
{
    return update.visit(
        [](Publisher::NothingChanged) { return true; },
        [&](Publisher::WholeTree& tree) {
            manager.update_tree(move(tree.nodes), tree.sequence);
            return true;
        },
        [&](Publisher::Change& change) {
            return manager.apply_delta(change.delta, change.base_sequence, change.sequence).has_value();
        });
}

}

TEST_CASE(change_to_a_tree_the_manager_does_not_hold_is_rejected)
{
    Publisher publisher;
    WebView::AccessibilityTreeManager manager;
    EXPECT(deliver(manager, publisher.publish(7, base_tree_nodes(), false)));

    // The manager misses a change, so the next one names a tree it doesn't hold.
    auto nodes = base_tree_nodes();
    nodes[3].name = "first"_string;
    (void)publisher.publish(7, nodes, false);
    nodes[3].name = "second"_string;
    auto before = describe_tree(manager);
    EXPECT(!deliver(manager, publisher.publish(7, nodes, false)));
    EXPECT_EQ(describe_tree(manager), before);

    // The whole tree brings it back in step, and the changes after it apply again.
    EXPECT(deliver(manager, publisher.publish(7, nodes, true)));
    nodes[3].name = "third"_string;
    EXPECT(deliver(manager, publisher.publish(7, nodes, false)));
    EXPECT_EQ(manager.node(3)->name, "third"sv);
}

TEST_CASE(changes_the_publisher_sends_keep_the_manager_tree_identical)
{
    // A tree held as parent and child lists, edited at random, and serialized in tree order the way a document builds
    // its node data.
    struct Node {
        i64 parent { -1 };
        Vector<i64> children;
        String name;
    };
    HashMap<i64, Node> tree;
    tree.set(0, {});
    i64 next_id = 1;

    u64 state = 0x9e3779b97f4a7c15;
    auto random = [&](size_t bound) -> size_t {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<size_t>((state >> 33) % bound);
    };
    auto random_node = [&]() {
        auto keys = tree.keys();
        return keys[random(keys.size())];
    };
    auto is_in_subtree = [&](i64 id, i64 subtree_root) {
        for (auto current = id; current != -1; current = tree.get(current)->parent) {
            if (current == subtree_root)
                return true;
        }
        return false;
    };
    auto serialize = [&]() {
        Vector<WebView::AccessibilityNodeData> nodes;
        Vector<i64> stack { 0 };
        while (!stack.is_empty()) {
            auto id = stack.take_last();
            auto const& node = *tree.get(id);
            auto data = make_node(id, node.parent, node.children);
            data.name = node.name;
            nodes.append(move(data));
            for (size_t i = node.children.size(); i > 0; --i)
                stack.append(node.children[i - 1]);
        }
        return nodes;
    };

    Publisher publisher;
    WebView::AccessibilityTreeManager manager;
    for (size_t round = 0; round < 400; ++round) {
        for (size_t edit = 0; edit < 5; ++edit) {
            auto kind = tree.size() < 3 ? 0 : random(5);
            if (kind == 0) {
                // Insert a new node.
                auto parent = random_node();
                auto& children = tree.find(parent)->value.children;
                children.insert(random(children.size() + 1), next_id);
                tree.set(next_id, { parent, {}, MUST(String::formatted("node {}", next_id)) });
                ++next_id;
            } else if (kind == 1) {
                // Remove a subtree.
                auto root = random_node();
                if (root == 0)
                    continue;
                tree.find(tree.get(root)->parent)->value.children.remove_first_matching([&](i64 child) { return child == root; });
                Vector<i64> doomed { root };
                while (!doomed.is_empty()) {
                    auto id = doomed.take_last();
                    doomed.extend(tree.get(id)->children);
                    tree.remove(id);
                }
            } else if (kind == 2) {
                // Move a subtree.
                auto moved = random_node();
                auto new_parent = random_node();
                if (moved == 0 || is_in_subtree(new_parent, moved))
                    continue;
                tree.find(tree.get(moved)->parent)->value.children.remove_first_matching([&](i64 child) { return child == moved; });
                auto& children = tree.find(new_parent)->value.children;
                children.insert(random(children.size() + 1), moved);
                tree.find(moved)->value.parent = new_parent;
            } else if (kind == 3) {
                // Rename a node.
                tree.find(random_node())->value.name = MUST(String::formatted("renamed in round {}", round));
            } else {
                // Reverse a node's children.
                auto& children = tree.find(random_node())->value.children;
                children.reverse();
            }
        }

        auto nodes = serialize();
        auto update = publisher.publish(1, nodes, false);
        // After the first round, nothing goes out but changes.
        if (round > 0)
            EXPECT(!update.has<Publisher::WholeTree>());
        EXPECT(deliver(manager, move(update)));

        EXPECT_EQ(manager.root_id(), 0);
        size_t reachable = assert_valid_tree_and_collect(manager).size();
        EXPECT_EQ(reachable, nodes.size());
        for (auto const& expected : nodes) {
            auto const* held = manager.node(expected.id);
            EXPECT(held && *held == expected);
        }
        // And every node removed is gone, not just unreachable.
        for (i64 id = 0; id < next_id; ++id) {
            if (!tree.contains(id))
                EXPECT(!manager.node(id));
        }
    }
}

TEST_CASE(no_change_applies_to_a_repaired_whole_tree)
{
    // A chain deeper than the manager allows: It keeps the chain only down to its depth limit, so it holds a tree that
    // isn't the one WebContent changes, and every change has to be refused for the whole tree to come again.
    Vector<WebView::AccessibilityNodeData> nodes;
    for (i64 id = 0; id <= 1001; ++id)
        nodes.append(make_node(id, id - 1, id < 1001 ? Vector<i64> { id + 1 } : Vector<i64> {}));
    Publisher publisher;
    WebView::AccessibilityTreeManager manager;
    EXPECT(deliver(manager, publisher.publish(7, nodes, false)));
    EXPECT(!manager.node(1001));

    // Move 500 under the root: 1001 would fit again, but only a whole tree can bring it.
    nodes[0].child_ids = { 1, 500 };
    nodes[499].child_ids = {};
    nodes[500].parent_id = 0;
    EXPECT(!deliver(manager, publisher.publish(7, nodes, false)));
    EXPECT(deliver(manager, publisher.publish(7, nodes, true)));
    EXPECT(manager.node(1001));
}

TEST_CASE(root_focus_shown_for_the_assistive_technology_lasts_until_the_next_change)
{
    Publisher publisher;
    WebView::AccessibilityTreeManager manager;
    auto nodes = base_tree_nodes();
    nodes[3].is_focused = true;
    EXPECT(deliver(manager, publisher.publish(7, nodes, false)));
    EXPECT_EQ(manager.focused_node_id(), 3);

    manager.show_focus_on_root();
    EXPECT(manager.root()->is_focused);
    EXPECT(!manager.node(3)->is_focused);

    // A change that doesn't touch 3 still puts back the focus WebContent reports, as a whole tree would.
    nodes[2].name = "renamed"_string;
    EXPECT(deliver(manager, publisher.publish(7, nodes, false)));
    EXPECT(!manager.root()->is_focused);
    EXPECT(manager.node(3)->is_focused);
    EXPECT_EQ(manager.focused_node_id(), 3);
}
