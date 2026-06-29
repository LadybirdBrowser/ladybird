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
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ChromeWidget;
class Paintable;

struct HitTestResult {
    NonnullRefPtr<Paintable> paintable;
    RefPtr<ChromeWidget> chrome_widget {};
    GC::Ptr<DOM::Node> dom_node_override {};
    size_t index_in_node { 0 };
    enum InternalPosition {
        None,
        Before,
        Inside,
        After,
    };
    InternalPosition internal_position { None };

    DOM::Node* dom_node();
    DOM::Node const* dom_node() const;
};

struct CaretPosition {
    NonnullRefPtr<Paintable> paintable;
    DOM::BoundaryPoint boundary;
    Optional<DOM::BoundaryPoint> secondary_boundary {};
    Optional<CSSPixelRect> debug_rect {};
};

enum class HitTestType : u8 {
    Exact, // Exact matches only
};

}
