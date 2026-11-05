/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebView/AccessibilityNodeData.h>

namespace WebView {

// Decides what a page sends the UI process for each accessibility tree it builds: the whole tree when the UI process
// asks for it or the page has a new document, and otherwise only what changed since the last tree sent. Each tree sent
// gets the next sequence number, and a change names the number of the tree it applies to, so the UI process can tell
// when it doesn't hold that tree and has to ask for the whole one again.
class WEBCOMMON_API AccessibilityTreePublisher {
public:
    struct WholeTree {
        u64 sequence { 0 };
        Vector<AccessibilityNodeData> nodes;
    };
    struct Change {
        u64 base_sequence { 0 };
        u64 sequence { 0 };
        AccessibilityTreeDelta delta;
    };
    struct NothingChanged { };
    using Update = Variant<NothingChanged, WholeTree, Change>;

    // The nodes are a whole tree in tree order, as the document builds it.
    Update publish(i64 document_id, Vector<AccessibilityNodeData> nodes, bool whole_tree_requested);

private:
    Optional<i64> m_document_id;
    HashMap<i64, AccessibilityNodeData> m_published_nodes;
    u64 m_sequence { 0 };
};

}
