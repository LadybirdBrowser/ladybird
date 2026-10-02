/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::Layout {

class Node;
class TextNode;

// A document's host for its render state: the DocumentHost that names the state and holds the host tables layout
// answers to, kept alive by the layout shells and paint objects that reach the state's arena.
class WEB_API NodeArena : public RefCounted<NodeArena> {
    AK_MAKE_NONCOPYABLE(NodeArena);
    AK_MAKE_NONMOVABLE(NodeArena);

public:
    NodeArena();
    ~NodeArena();

    void free_subtree(Compositing::RustFFI::NodeSlotId);
    Node* node_if_live(Compositing::RustFFI::NodeSlotId) const;
    void* handle() const { return m_handle; }
    u64 table_cell_measurement_cache_miss_count() const;
    u64 intrinsic_measurement_count() const;
    u64 intrinsic_inline_measurement_count() const;

    void sync_enrolled_content_for_layout();

    DOM::Document* document() const { return m_document.ptr(); }
    void set_document(Badge<DOM::Document>, DOM::Document* document) { m_document = document; }

    // The arena reports to each DOM node whether it has a layout node, and whether that layout node has a committed
    // box, as it changes them. Only the arena writes those bits onto the node.
    void start_reporting_box_presence(Badge<DOM::Document>);
    void stop_reporting_box_presence(Badge<DOM::Document>);
    void commit_box_presence(DOM::Node&);

private:
    RustFFI::DocumentHost* m_host { nullptr };
    // The arena of the document's render state, which the entries that have not been converted to render messages
    // still take.
    void* m_handle { nullptr };
    GC::RawPtr<DOM::Document> m_document;
};

WEB_API bool destroy_layout_subtree(Node&);

}
