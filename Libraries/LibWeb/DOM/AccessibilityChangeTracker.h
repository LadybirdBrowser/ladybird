/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <LibWebCommon/WebView/AccessibilityNodeData.h>

namespace Web::DOM {

// Remembers what was last handed over of each node of a document's accessibility tree, so that a build can hand over
// only the nodes whose data changed since then, and the IDs of the nodes that are gone: see Document::
// build_accessibility_tree_change(). Each consumer of the tree keeps its own.
class AccessibilityChangeTracker {
public:
    struct Change {
        bool is_whole_tree { false };
        // In tree order.
        Vector<WebView::AccessibilityNodeData> nodes;
        Vector<i64> removed_ids;
    };

    // Whether the next build has to hand over the whole tree: the first for a document.
    bool needs_whole_tree(i64 document_id) const { return m_document_id != document_id; }

    // For Document::build_accessibility_tree_change(): A build begins, notes each node it reaches, and ends.
    void begin(i64 document_id, bool whole_tree);
    // Returns whether the node goes in the change. A node's data is the same as last time when its kept record and
    // geometry have the same versions (see Document::kept_accessibility_record()) and every field computed again on
    // each build is the same; a geometry version that isn't known counts as changed.
    bool note(WebView::AccessibilityNodeData const&, u64 record_version, Optional<u64> geometry_version);
    // Returns the IDs of the nodes noted last time but not this time.
    Vector<i64> end();

private:
    struct NodeState {
        u64 record_version { 0 };
        Optional<u64> geometry_version;
        i64 parent_id { -1 };
        Vector<i64> child_ids;
        Gfx::IntRect bounds;
        bool is_focused { false };
        i32 caret_offset { -1 };
        i32 selection_start { -1 };
        i32 selection_end { -1 };
        u64 build { 0 };
    };

    Optional<i64> m_document_id;
    HashMap<i64, NodeState> m_states;
    u64 m_build { 0 };
    size_t m_known_at_begin { 0 };
    size_t m_known_noted { 0 };
    bool m_whole_tree { false };
};

}
