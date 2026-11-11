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

class HitTestDisplayList;

// A query of a document's hit test list, with what it reads besides the list and the document's scroll state, such as
// the time it samples the compositor's animations at. Only the document makes one, once it has prepared its paint
// properties and found the list current against the visual context tree, so every item a query looks at reads the same
// preparation, and nothing in a query prepares the document again.
class HitTestQuery {
public:
    HitTestDisplayList const& list() const { return m_list; }
    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }
    ChromeMetrics const& chrome_metrics() const { return m_chrome_metrics; }
    // The time the query reads the compositor's animations at, as a frame of the compositor's shows them, if any.
    Optional<i64> visual_animation_sample_time_nanoseconds() const { return m_visual_animation_sample_time_nanoseconds; }

private:
    friend class DOM::Document;

    HitTestQuery(HitTestDisplayList const& list, double device_pixels_per_css_pixel, ChromeMetrics chrome_metrics, Optional<i64> visual_animation_sample_time_nanoseconds)
        : m_list(list)
        , m_device_pixels_per_css_pixel(device_pixels_per_css_pixel)
        , m_chrome_metrics(chrome_metrics)
        , m_visual_animation_sample_time_nanoseconds(visual_animation_sample_time_nanoseconds)
    {
    }

    HitTestDisplayList const& m_list;
    double m_device_pixels_per_css_pixel { 1 };
    ChromeMetrics m_chrome_metrics;
    Optional<i64> m_visual_animation_sample_time_nanoseconds;
};

class WEB_API HitTestDisplayList : public RefCounted<HitTestDisplayList> {
public:
    static NonnullRefPtr<HitTestDisplayList> create_from_rust_recording(Layout::BegunRead const& read, u64 visual_context_tree_structural_epoch, Layout::NodeArena&, ChromeWidgetRegistry&);

    u64 visual_context_tree_structural_epoch() const { return m_visual_context_tree_structural_epoch; }
    [[nodiscard]] bool is_current() const;
    [[nodiscard]] Optional<HitTestResult> hit_test(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&) const;
    // The caret position the box hit at the point gives it, or when constraint_scope is given, the one that node's box
    // gives it wherever the point is.
    [[nodiscard]] Optional<CaretPosition> caret_position_from_point(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&, GC::Ptr<DOM::Node const> constraint_scope = nullptr) const;
    TraversalDecision hit_test_all(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&, Function<TraversalDecision(HitTestResult)> const&) const;

private:
    HitTestDisplayList(u64 visual_context_tree_structural_epoch, Layout::NodeArena&, ChromeWidgetRegistry&, u64 rust_generation);

    struct Item {
        size_t item_index { 0 };
        Layout::RustFFI::FfiHitTestItemExport facts;

        size_t index() const { return item_index; }
        Compositing::RustFFI::NodeSlotId paintable() const { return facts.paintable; }
        Compositing::RustFFI::NodeSlotId hit_node() const { return facts.hit_node; }
        ChromeWidgetKind chrome_widget_kind() const { return static_cast<ChromeWidgetKind>(facts.chrome_widget_kind); }
        Compositing::ContextRef context() const { return facts.context; }
    };

    struct TopmostItem {
        size_t index { 0 };
        CSSPixelPoint local_point;
    };

    static Optional<TopmostItem> topmost_item_from(Layout::RustFFI::FfiTopmostItem const&);
    [[nodiscard]] Item item(Layout::BegunRead const& read, size_t index) const;

    [[nodiscard]] Optional<TopmostItem> find_topmost_item(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&) const;
    [[nodiscard]] Vector<TopmostItem> hit_items_topmost_first(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&) const;

    [[nodiscard]] Layout::Node const* layout_node_for_item(Layout::BegunRead const& read, Item) const;
    [[nodiscard]] RefPtr<ChromeWidget> chrome_widget_for_item(Item) const;
    [[nodiscard]] HitTestResult hit_test_result_for_item(Layout::BegunRead const& read, Item, CSSPixelPoint local_point) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_from(Layout::RustFFI::FfiCaretAt const&) const;

    u64 m_visual_context_tree_structural_epoch { 0 };
    NonnullRefPtr<Layout::NodeArena> m_arena;
    NonnullRefPtr<ChromeWidgetRegistry> m_chrome_widget_registry;
    u64 m_rust_generation { 0 };
};

}
