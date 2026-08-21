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
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>

namespace Web::Painting {

class ChromeWidget;
class Paintable;

struct HitTestResult {
    GC::Ptr<DOM::Node> node;
    Layout::RustFFI::PaintableSlotId box;
    NonnullRefPtr<Layout::NodeArena> arena;
    RefPtr<ChromeWidget> chrome_widget {};
    size_t index_in_node { 0 };
    bool is_text_fragment { false };

    DOM::Node* dom_node() { return node.ptr(); }
    DOM::Node const* dom_node() const { return node.ptr(); }
    RefPtr<Paintable> paintable() const;
};

struct CaretPosition {
    Layout::RustFFI::PaintableSlotId box;
    NonnullRefPtr<Layout::NodeArena> arena;
    DOM::BoundaryPoint boundary;
    TextAffinity affinity { TextAffinity::Downstream };
    Optional<DOM::BoundaryPoint> secondary_boundary {};
    Optional<CSSPixelRect> debug_rect {};

    RefPtr<Paintable> paintable() const;
};

}
