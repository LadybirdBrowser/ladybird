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
#include <LibWeb/Layout/RenderDocument.h>

namespace Web::Layout {

class Node;
class TextNode;

// A document's layout node arena, in the render state the document's style engine created, which the host reaches
// only through the document's host. It is kept alive by the layout shells and paint objects that reach the arena.
class WEB_API NodeArena : public RefCounted<NodeArena> {
    AK_MAKE_NONCOPYABLE(NodeArena);
    AK_MAKE_NONMOVABLE(NodeArena);

public:
    explicit NodeArena(RenderDocument&);
    ~NodeArena();

    void free_subtree(Layout::BegunRead const& read, Compositing::RustFFI::NodeSlotId);
    Node* node_if_live(Layout::BegunRead const& read, Compositing::RustFFI::NodeSlotId) const;
    RustFFI::DocumentHost* host() const { return m_render_document->host(); }
    RenderDocument const& render_document() const { return *m_render_document; }
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
    NonnullRefPtr<RenderDocument> m_render_document;
    GC::RawPtr<DOM::Document> m_document;
};

WEB_API bool destroy_layout_subtree(Node&);

}
