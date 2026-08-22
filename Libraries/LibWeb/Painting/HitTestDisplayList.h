/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/HitTestResult.h>

namespace Web {

struct ChromeMetrics;

namespace Painting {

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

class WEB_API HitTestDisplayList : public RefCounted<HitTestDisplayList> {
public:
    static NonnullRefPtr<HitTestDisplayList> create_from_rust_recording(u64 visual_context_tree_version, Layout::NodeArena&, ChromeWidgetRegistry&);

    size_t item_count() const { return m_items.size(); }
    void visit_edges(GC::Cell::Visitor&);

    u64 visual_context_tree_version() const { return m_visual_context_tree_version; }
    [[nodiscard]] bool is_current() const;
    [[nodiscard]] Optional<HitTestResult> hit_test(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, ChromeMetrics const&) const;
    // When constraint_scope is given, the caret position is constrained to lines inside that node, and points
    // outside it resolve to the closest position within it.
    [[nodiscard]] Optional<CaretPosition> caret_position_from_point(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, ChromeMetrics const&, CaretPositionMode = CaretPositionMode::Normal, GC::Ptr<DOM::Node const> constraint_scope = nullptr) const;
    // Resolve Home/End against the painted line containing the caret. A visual line can span several DOM nodes and
    // atomic inline boxes, so a text-node or block-element boundary is not necessarily a rendered line boundary.
    [[nodiscard]] Optional<CaretPosition> caret_position_at_line_edge(DOM::Node const&, size_t offset, TextAffinity, CaretLineEdge) const;
    // Find the visually adjacent caret line within scope, preserving the requested inline-axis coordinate. This is a
    // rendered-content query: DOM adjacency alone cannot describe wrapping, writing modes, floats, or empty lines.
    [[nodiscard]] Optional<CaretPosition> caret_position_on_adjacent_line(DOM::Node const&, size_t offset, TextAffinity, CaretLineDirection, CSSPixels inline_coordinate, DOM::Node const& scope) const;
    [[nodiscard]] Optional<CSSPixels> caret_line_block_coordinate(DOM::Node const&, size_t offset, TextAffinity) const;
    TraversalDecision hit_test_all(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, ChromeMetrics const&, Function<TraversalDecision(HitTestResult)> const&) const;

private:
    HitTestDisplayList(u64 visual_context_tree_version, Layout::NodeArena&, ChromeWidgetRegistry&, u64 rust_generation);

    enum class ItemKind : u8 {
        Box,
        SvgPath,
        TextFragment,
        // A line box with no fragments (e.g. a blank line in a textarea), recorded as a caret target only.
        EmptyLine,
        EmptyEditable,
        ChromeWidget,
    };

    struct Item {
        ItemKind kind;
        Layout::RustFFI::NodeSlotId box;
        ChromeWidgetKind chrome_widget_kind { ChromeWidgetKind::None };
        Optional<u32> text_fragment_index;
        GC::Ptr<DOM::Node const> caret_node { nullptr };
        // For EmptyLine items: the caret offset in caret_node.
        size_t caret_offset { 0 };
        CSSPixelRect rect;
        CSSPixelRect caret_rect;
        VisualContextIndex visual_context_index;
    };

    struct CaretLine {
        CSSPixelRect rect;
        VisualContextIndex visual_context_index;
        size_t first_caret_item_index { 0 };
        size_t last_caret_item_index { 0 };
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
    SortingContexts const& ensure_sorting_contexts(DOM::Document const&) const;
    void ensure_caret_lines() const;
    [[nodiscard]] Item const& caret_item(size_t caret_item_index) const { return m_items[m_caret_item_indices[caret_item_index]]; }

    [[nodiscard]] Optional<TopmostItem> find_topmost_item(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, ChromeMetrics const&) const;
    void find_topmost_items_for_caret(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, ChromeMetrics const&, Optional<TopmostItem>& caret_item, Optional<TopmostItem>& hit_item) const;
    [[nodiscard]] Vector<size_t> hit_item_indices_topmost_first(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, ChromeMetrics const&) const;
    [[nodiscard]] size_t item_index_at_line_edge(size_t line_index, CaretPositionType) const;
    [[nodiscard]] Optional<CaretItemForLine> caret_item_for_line(size_t line_index, CSSPixelPoint local_point, CaretPositionMode) const;
    [[nodiscard]] bool item_is_inline_adjacent_to_line(size_t item_index, size_t line_index) const;
    [[nodiscard]] ClosestLine find_closest_line(CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel, CaretPositionMode, DOM::Node const* scope_dom_node, AccumulatedVisualContextTree::ClipBehavior) const;

    [[nodiscard]] Optional<CSSPixelPoint> local_point_for_visual_context(VisualContextIndex, CSSPixelPoint, DOM::Document const&, double device_pixels_per_css_pixel) const;
    [[nodiscard]] CSSPixelRect viewport_rect_for_context(VisualContextIndex, CSSPixelRect const&, DOM::Document const&, double device_pixels_per_css_pixel) const;
    [[nodiscard]] Layout::Node const* layout_node_for_item(Item const&) const;
    [[nodiscard]] RefPtr<ChromeWidget> chrome_widget_for_item(Item const&) const;
    [[nodiscard]] DOM::Node const* item_dom_node(Item const&) const;
    [[nodiscard]] DOM::Node const* event_dispatch_dom_node_for_item(Item const&) const;
    [[nodiscard]] bool item_can_produce_caret_position(Item const&) const;
    [[nodiscard]] bool item_is_direct_caret_target(Item const&) const;
    [[nodiscard]] HitTestResult hit_test_result_for_item(Item const&, CSSPixelPoint local_point) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_item(Item const&, CSSPixelPoint local_point, CaretPositionType = CaretPositionType::Closest) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_hit_container(Item const&) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_line(size_t line_index, CSSPixelPoint local_point, CaretPositionMode) const;
    [[nodiscard]] bool item_contains_caret_position(Item const&, DOM::Node const&, size_t offset, TextAffinity) const;
    [[nodiscard]] Optional<size_t> caret_line_index_for_position(DOM::Node const&, size_t offset, TextAffinity) const;
    [[nodiscard]] bool line_contains_descendant_of(CaretLine const&, DOM::Node const&) const;

    u64 m_visual_context_tree_version { 0 };
    NonnullRefPtr<Layout::NodeArena> m_arena;
    NonnullRefPtr<ChromeWidgetRegistry> m_chrome_widget_registry;
    u64 m_rust_generation { 0 };
    Vector<Item> m_items;
    mutable bool m_caret_lines_materialized { false };
    mutable Vector<size_t> m_caret_item_indices;
    mutable Vector<CaretLine> m_caret_lines;
    mutable Optional<SortingContexts> m_sorting_contexts;
};

}

}
