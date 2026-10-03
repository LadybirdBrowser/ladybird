/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibWeb/DOM/AbstractRange.h>
#include <LibWeb/DOM/NodeIdentity.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/TextAffinity.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Painting {

struct HitTestResult {
    DOM::NodeIdentity node;
    Compositing::RustFFI::NodeSlotId hit_node;
    NonnullRefPtr<Layout::NodeArena> arena;
    RefPtr<ChromeWidget> chrome_widget {};
    size_t index_in_node { 0 };
    bool is_text_fragment { false };

    DOM::Node* dom_node() const;
    Layout::Node* layout_node(Layout::BegunRead const& read) const { return layout_node_for_committed_slot(read, *arena, hit_node); }
};

// A boundary point that names its node instead of pointing at it. A node that left the tree since
// the hit test resolves to nothing, where a pointer would have handed back a node the document no
// longer contains.
struct WEB_API BoundaryIdentity {
    DOM::NodeIdentity node;
    WebIDL::UnsignedLong offset { 0 };

    Optional<DOM::BoundaryPoint> resolve(DOM::Document&) const;
};

struct WEB_API CaretPosition {
    Compositing::RustFFI::NodeSlotId paintable;
    NonnullRefPtr<Layout::NodeArena> arena;
    BoundaryIdentity boundary;
    TextAffinity affinity { TextAffinity::Downstream };
    Optional<BoundaryIdentity> secondary_boundary {};
    Optional<CSSPixelRect> debug_rect {};

    GC::Ptr<DOM::Node> boundary_node() const;
    Optional<DOM::BoundaryPoint> boundary_point() const;
    // The layout node the boundary's node is bound to, found in the arena rather than asked of that node.
    Layout::Node* boundary_layout_node(Layout::BegunRead const& read) const;
    Layout::Node* layout_node(Layout::BegunRead const& read) const { return layout_node_for_committed_slot(read, *arena, paintable); }
};

}
