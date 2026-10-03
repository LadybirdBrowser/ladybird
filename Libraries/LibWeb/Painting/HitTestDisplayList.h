/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
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

enum class CaretPositionMode : u8 {
    Normal,
    // Starting or extending selection should feel more eager than the public caret-position API in inter-line gaps.
    SelectionStart,
    Selection,
};

enum class CaretLineEdge : u8 {
    Start,
    End,
};

enum class CaretLineDirection : u8 {
    Previous,
    Next,
};

class HitTestDisplayList;

// A query of a document's hit test list, with what it reads besides the list: the visual context tree and the scroll
// state its points and rects convert through, as the document prepared them for the query. Only the document makes
// one, once it has prepared them and found the list current against that tree, so every item a query looks at reads
// the same preparation, and nothing in a query prepares the document again.
class HitTestQuery {
public:
    HitTestDisplayList const& list() const { return m_list; }
    Compositing::AccumulatedVisualContextTree const& visual_context_tree() const { return m_visual_context_tree; }
    Compositing::ScrollStateSnapshot const& scroll_state() const { return m_scroll_state; }
    double device_pixels_per_css_pixel() const { return m_device_pixels_per_css_pixel; }
    ChromeMetrics const& chrome_metrics() const { return m_chrome_metrics; }
    ViewportWheelOverflow viewport_wheel_overflow() const { return m_viewport_wheel_overflow; }

private:
    friend class DOM::Document;

    HitTestQuery(HitTestDisplayList const& list, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state, double device_pixels_per_css_pixel, ChromeMetrics chrome_metrics, ViewportWheelOverflow viewport_wheel_overflow)
        : m_list(list)
        , m_visual_context_tree(move(visual_context_tree))
        , m_scroll_state(scroll_state)
        , m_device_pixels_per_css_pixel(device_pixels_per_css_pixel)
        , m_chrome_metrics(chrome_metrics)
        , m_viewport_wheel_overflow(viewport_wheel_overflow)
    {
    }

    HitTestDisplayList const& m_list;
    Compositing::AccumulatedVisualContextTree m_visual_context_tree;
    Compositing::ScrollStateSnapshot const& m_scroll_state;
    double m_device_pixels_per_css_pixel { 1 };
    ChromeMetrics m_chrome_metrics;
    ViewportWheelOverflow m_viewport_wheel_overflow;
};

class WEB_API HitTestDisplayList : public RefCounted<HitTestDisplayList> {
public:
    static NonnullRefPtr<HitTestDisplayList> create_from_rust_recording(Layout::BegunRead const& read, u64 visual_context_tree_structural_epoch, Layout::NodeArena&, ChromeWidgetRegistry&);

    u64 visual_context_tree_structural_epoch() const { return m_visual_context_tree_structural_epoch; }
    [[nodiscard]] bool is_current() const;
    [[nodiscard]] Optional<HitTestResult> hit_test(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&) const;
    // When constraint_scope is given, the caret position is constrained to lines inside that node, and points
    // outside it resolve to the closest position within it.
    [[nodiscard]] Optional<CaretPosition> caret_position_from_point(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&, CaretPositionMode = CaretPositionMode::Normal, GC::Ptr<DOM::Node const> constraint_scope = nullptr) const;
    // Resolve Home/End against the painted line containing the caret. A visual line can span several DOM nodes and
    // atomic inline boxes, so a text-node or block-element boundary is not necessarily a rendered line boundary.
    [[nodiscard]] Optional<CaretPosition> caret_position_at_line_edge(Layout::BegunRead const& read, DOM::Node const&, size_t offset, TextAffinity, CaretLineEdge) const;
    // Find the visually adjacent caret line within scope, preserving the requested inline-axis coordinate. This is a
    // rendered-content query: DOM adjacency alone cannot describe wrapping, writing modes, floats, or empty lines.
    [[nodiscard]] Optional<CaretPosition> caret_position_on_adjacent_line(Layout::BegunRead const& read, DOM::Node const&, size_t offset, TextAffinity, CaretLineDirection, CSSPixels inline_coordinate, DOM::Node const& scope) const;
    [[nodiscard]] Optional<CSSPixels> caret_line_block_coordinate(Layout::BegunRead const& read, DOM::Node const&, size_t offset, TextAffinity) const;
    TraversalDecision hit_test_all(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&, Function<TraversalDecision(HitTestResult)> const&) const;

private:
    HitTestDisplayList(u64 visual_context_tree_structural_epoch, Layout::NodeArena&, ChromeWidgetRegistry&, u64 rust_generation);

    struct Item {
        size_t item_index { 0 };
        Layout::RustFFI::FfiHitTestItemExport facts;

        size_t index() const { return item_index; }
        bool can_produce_caret_position() const { return facts.can_produce_caret_position; }
        Compositing::RustFFI::NodeSlotId paintable() const { return facts.paintable; }
        Compositing::RustFFI::NodeSlotId hit_node() const { return facts.hit_node; }
        ChromeWidgetKind chrome_widget_kind() const { return static_cast<ChromeWidgetKind>(facts.chrome_widget_kind); }
        CSSPixelRect caret_rect() const { return facts.caret_rect; }
        Compositing::ContextRef context() const { return facts.context; }
    };

    enum class CaretPositionType : u8 {
        Closest,
        Before,
        After,
    };

    struct TopmostItem {
        size_t index { 0 };
        CSSPixelPoint local_point;
    };

    struct CaretItemForLine {
        size_t item_index { 0 };
        CaretPositionType type { CaretPositionType::Closest };
    };

    struct ClosestLine {
        Optional<size_t> index;
        CSSPixelPoint local_point;
        CSSPixels block_distance { CSSPixels::max() };
    };

    struct QueryContext;
    static Optional<TopmostItem> topmost_item_from(Layout::RustFFI::FfiTopmostItem const&);
    [[nodiscard]] Item item(Layout::BegunRead const& read, size_t index) const;
    [[nodiscard]] Layout::RustFFI::FfiCaretLineExport caret_line(Layout::BegunRead const& read, size_t line_index) const { return Layout::RustFFI::layout_hit_test_caret_line(m_arena->host(), &read, line_index); }

    [[nodiscard]] Optional<TopmostItem> find_topmost_item(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&) const;
    void find_topmost_items_for_caret(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&, Optional<TopmostItem>& caret_item, Optional<TopmostItem>& hit_item) const;
    [[nodiscard]] Vector<size_t> hit_item_indices_topmost_first(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&) const;
    [[nodiscard]] size_t item_index_at_line_edge(Layout::BegunRead const& read, size_t line_index, CaretPositionType) const;
    [[nodiscard]] Optional<CaretItemForLine> caret_item_for_line(Layout::BegunRead const& read, size_t line_index, CSSPixelPoint local_point, CaretPositionMode) const;
    [[nodiscard]] bool item_is_inline_adjacent_to_line(Layout::BegunRead const& read, size_t item_index, size_t line_index) const;
    [[nodiscard]] ClosestLine find_closest_line(Layout::BegunRead const& read, CSSPixelPoint, HitTestQuery const&, CaretPositionMode, DOM::Node const* scope_dom_node, Compositing::AccumulatedVisualContextTree::ClipBehavior) const;

    [[nodiscard]] Optional<CSSPixelPoint> local_point_for_visual_context(Compositing::ContextRef, CSSPixelPoint, HitTestQuery const&) const;
    [[nodiscard]] CSSPixelRect viewport_rect_for_context(Compositing::SpatialNodeIndex, CSSPixelRect const&, HitTestQuery const&) const;
    [[nodiscard]] Layout::Node const* layout_node_for_item(Layout::BegunRead const& read, Item) const;
    [[nodiscard]] RefPtr<ChromeWidget> chrome_widget_for_item(Item) const;
    [[nodiscard]] DOM::Node const* item_dom_node(Layout::BegunRead const& read, size_t item_index) const;
    [[nodiscard]] DOM::NodeIdentity item_identity(Layout::BegunRead const& read, size_t item_index) const;
    [[nodiscard]] DOM::NodeIdentity event_dispatch_identity_for_item(Layout::BegunRead const& read, size_t item_index) const;
    [[nodiscard]] DOM::Node const* event_dispatch_dom_node_for_item(Layout::BegunRead const& read, size_t item_index) const;
    [[nodiscard]] bool item_is_direct_caret_target(Layout::BegunRead const& read, size_t item_index) const;
    [[nodiscard]] HitTestResult hit_test_result_for_item(Layout::BegunRead const& read, Item, CSSPixelPoint local_point) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_item(Layout::BegunRead const& read, Item, CSSPixelPoint local_point, CaretPositionType = CaretPositionType::Closest) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_hit_container(Layout::BegunRead const& read, Item) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_line(Layout::BegunRead const& read, size_t line_index, CSSPixelPoint local_point, CaretPositionMode) const;

    u64 m_visual_context_tree_structural_epoch { 0 };
    NonnullRefPtr<Layout::NodeArena> m_arena;
    NonnullRefPtr<ChromeWidgetRegistry> m_chrome_widget_registry;
    u64 m_rust_generation { 0 };
};

}
