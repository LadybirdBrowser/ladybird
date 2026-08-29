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
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>

namespace Web::Painting {

struct HitTestResult {
    GC::Ptr<DOM::Node> node;
    Layout::RustFFI::NodeSlotId hit_node;
    NonnullRefPtr<Layout::NodeArena> arena;
    RefPtr<ChromeWidget> chrome_widget {};
    size_t index_in_node { 0 };
    bool is_text_fragment { false };

    DOM::Node* dom_node() { return node.ptr(); }
    DOM::Node const* dom_node() const { return node.ptr(); }
    Layout::Node* layout_node() const { return layout_node_for_committed_slot(*arena, hit_node); }
};

struct CaretPosition {
    Layout::RustFFI::NodeSlotId paintable;
    NonnullRefPtr<Layout::NodeArena> arena;
    DOM::BoundaryPoint boundary;
    TextAffinity affinity { TextAffinity::Downstream };
    Optional<DOM::BoundaryPoint> secondary_boundary {};
    Optional<CSSPixelRect> debug_rect {};

    Layout::Node* layout_node() const { return layout_node_for_committed_slot(*arena, paintable); }
};

}
