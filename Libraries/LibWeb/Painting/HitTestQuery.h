/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibCompositing/Forward.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/HitTestResult.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Painting {

// A hit test of a document's rows, with what it reads besides the rows and the document's scroll state, such as the
// time it samples the compositor's animations at. Only the document makes one, once it has prepared its paint
// properties, so every box a query looks at reads the same preparation, and nothing in a query prepares the document
// again.
class HitTestQuery {
public:
    [[nodiscard]] Optional<HitTestResult> hit_test(Layout::BegunRead const& read, CSSPixelPoint) const;
    // The caret position the box hit at the point gives it, or when constraint_scope is given, the one that node's box
    // gives it wherever the point is.
    [[nodiscard]] Optional<CaretPosition> caret_position_from_point(Layout::BegunRead const& read, CSSPixelPoint, GC::Ptr<DOM::Node const> constraint_scope = nullptr) const;
    TraversalDecision hit_test_all(Layout::BegunRead const& read, CSSPixelPoint, Function<TraversalDecision(HitTestResult)> const&) const;

private:
    friend class DOM::Document;

    HitTestQuery(Layout::NodeArena& arena, ChromeWidgetRegistry& chrome_widget_registry, double device_pixels_per_css_pixel, ChromeMetrics chrome_metrics, Optional<i64> visual_animation_sample_time_nanoseconds)
        : m_arena(arena)
        , m_chrome_widget_registry(chrome_widget_registry)
        , m_device_pixels_per_css_pixel(device_pixels_per_css_pixel)
        , m_chrome_metrics(chrome_metrics)
        , m_visual_animation_sample_time_nanoseconds(visual_animation_sample_time_nanoseconds)
    {
    }

    [[nodiscard]] Layout::RustFFI::FfiHitTestQuery ffi_query() const;
    [[nodiscard]] HitTestResult hit_test_result_for(Layout::BegunRead const& read, Layout::RustFFI::FfiPointHit const&) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_from(Layout::RustFFI::FfiCaretAt const&) const;

    NonnullRefPtr<Layout::NodeArena> m_arena;
    NonnullRefPtr<ChromeWidgetRegistry> m_chrome_widget_registry;
    double m_device_pixels_per_css_pixel { 1 };
    ChromeMetrics m_chrome_metrics;
    // The time the query reads the compositor's animations at, as a frame of the compositor's shows them, if any.
    Optional<i64> m_visual_animation_sample_time_nanoseconds;
};

}
