/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/WebView/AccessibilityNodeData.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API AccessibilityTreeManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    void update_tree(Vector<AccessibilityNodeData>);
    // Applies a change to the tree held, if it fits that tree, and returns the IDs of the nodes it removed. A change that
    // doesn't fit (it names nodes the tree doesn't hold, or would leave a node with no parent or two, a cycle, or a tree
    // too deep) changes nothing, and announces nothing: The caller should get the whole tree again instead.
    Optional<Vector<i64>> apply_delta(AccessibilityTreeDelta const&);

    // The same, for the trees WebContent numbers as it sends them: A whole tree replaces the tree held, and a change
    // applies only to the tree with the number it names. No change applies to a whole tree that had to be repaired to
    // hold, since that's no longer the tree WebContent changes: Each update then takes the whole tree again.
    void update_tree(Vector<AccessibilityNodeData>, u64 sequence);
    Optional<Vector<i64>> apply_delta(AccessibilityTreeDelta const&, u64 base_sequence, u64 sequence);

    Function<void(String, String)> on_live_region_changed;

    AccessibilityNodeData const* node(i64 id) const;
    AccessibilityNodeData const* root() const;
    AccessibilityNodeData const* hit_test(Gfx::IntPoint point) const;
    // Marks the node WebContent reports focused.
    void set_focused_node(i64 node_id);
    // Marks the root focused for the assistive technology alone, until the next tree update puts back the focus that
    // WebContent reports.
    void show_focus_on_root();
    Optional<i64> focused_node_id() const;

    i64 root_id() const { return m_root_id; }
    bool is_empty() const { return m_nodes.is_empty(); }

    // Monotonic counter incremented by update_tree() and apply_delta(). Consumers that cache derived data keyed on node contents can
    // compare against the generation they last computed at — to know when a cache is stale.
    u64 generation() const { return m_generation; }

    // Build a flat list of text leaf node IDs in DFS order.
    Vector<i64> text_leaves_in_order() const;

private:
    AccessibilityNodeData const* hit_test_recursive(i64 node_id, Gfx::IntPoint point) const;

    HashMap<i64, AccessibilityNodeData> m_nodes;
    i64 m_root_id { -1 };
    u64 m_sequence { 0 };
    bool m_whole_tree_was_repaired { false };
    Optional<i64> m_focused_id_from_content;
    bool m_focus_shown_on_root { false };
    u64 m_generation { 0 };
};

}
