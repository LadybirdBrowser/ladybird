/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Path.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web {

struct ChromeMetrics;

namespace Painting {

class PaintableFragment;
class PaintableWithLines;
class ViewportPaintable;

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
    static NonnullRefPtr<HitTestDisplayList> create(u64 visual_context_tree_version);

    u64 id() const { return m_id; }
    size_t item_count() const { return m_items.size(); }
    void ensure_item_capacity(size_t capacity) { m_items.ensure_capacity(capacity); }

    // Copies a validated range of items recorded by one (paintable, phase) from the retained previous
    // list into this one, returning where it landed. Items are copied, never inspected: source ranges
    // belonging to relaid-out paintables may hold dangling fragment pointers, but only ranges whose
    // owners kept a valid cache entry (and therefore were not relaid out) are ever passed here.
    Paintable::HitTestItemRange append_cached_items(HitTestDisplayList const& source, Paintable::HitTestItemRange);

    void verify_cached_items_match_fresh_recording(Paintable::HitTestItemRange spliced_range, HitTestDisplayList const& fresh_recording, Paintable const&, PaintPhase) const;

    void append_box(Paintable const&, Paintable& target, CSSPixelRect, VisualContextIndex, BorderRadiiData);
    void append_svg_path(Paintable& target, Gfx::Path, Gfx::WindingRule, CSSPixelRect bounding_box, VisualContextIndex);
    void append_text_fragment(PaintableFragment const&, VisualContextIndex);
    void append_empty_line(PaintableFragment const& sibling_fragment, size_t caret_offset, size_t line_box_index, CSSPixelRect line_rect, VisualContextIndex);
    void append_empty_line(PaintableWithLines const&, DOM::Node const&, size_t caret_offset, CSSPixelRect line_rect, VisualContextIndex);
    void append_empty_editable(Paintable const&, CSSPixelRect, VisualContextIndex);
    void append_chrome_widget(Paintable const&, ChromeWidget&, VisualContextIndex);
    void visit_edges(GC::Cell::Visitor&);

    u64 visual_context_tree_version() const { return m_visual_context_tree_version; }
    [[nodiscard]] Optional<HitTestResult> hit_test(CSSPixelPoint, HitTestType, ViewportPaintable const&, double device_pixels_per_css_pixel, ChromeMetrics const&) const;
    // When constraint_scope is given, the caret position is constrained to lines inside that node, and points
    // outside it resolve to the closest position within it.
    [[nodiscard]] Optional<CaretPosition> caret_position_from_point(CSSPixelPoint, ViewportPaintable const&, double device_pixels_per_css_pixel, ChromeMetrics const&, CaretPositionMode = CaretPositionMode::Normal, DOM::Node const* constraint_scope = nullptr) const;
    // Resolve Home/End against the painted line containing the caret. A visual line can span several DOM nodes and
    // atomic inline boxes, so a text-node or block-element boundary is not necessarily a rendered line boundary.
    [[nodiscard]] Optional<CaretPosition> caret_position_at_line_edge(DOM::Node const&, size_t offset, TextAffinity, CaretLineEdge) const;
    // Find the visually adjacent caret line within scope, preserving the requested inline-axis coordinate. This is a
    // rendered-content query: DOM adjacency alone cannot describe wrapping, writing modes, floats, or empty lines.
    [[nodiscard]] Optional<CaretPosition> caret_position_on_adjacent_line(DOM::Node const&, size_t offset, TextAffinity, CaretLineDirection, CSSPixels inline_coordinate, DOM::Node const& scope) const;
    [[nodiscard]] Optional<CSSPixels> caret_line_block_coordinate(DOM::Node const&, size_t offset, TextAffinity) const;
    TraversalDecision hit_test_all(CSSPixelPoint, ViewportPaintable const&, double device_pixels_per_css_pixel, ChromeMetrics const&, Function<TraversalDecision(HitTestResult)> const&) const;

private:
    explicit HitTestDisplayList(u64 visual_context_tree_version);

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
        NonnullRefPtr<Paintable> paintable;
        RefPtr<ChromeWidget> chrome_widget;
        PaintableFragment const* text_fragment { nullptr };
        GC::Ptr<DOM::Node const> caret_node { nullptr };
        // For EmptyLine items: the caret offset in caret_node.
        size_t caret_offset { 0 };
        CSSPixelRect rect;
        CSSPixelRect caret_rect;
        Optional<size_t> caret_line_index;
        Optional<CSSPixelRect> caret_line_rect;
        Optional<CSSPixelRect> block_container_margin_rect;
        VisualContextIndex visual_context_index;
        BorderRadiiData border_radii;
        Optional<Gfx::Path> path {};
        Gfx::WindingRule winding_rule { Gfx::WindingRule::Nonzero };
    };

    struct SpatialIndex {
        HashMap<u64, Vector<size_t>> cells;
        Vector<size_t> unbucketed_items;
    };

    // A visual line assembled from consecutive caret-capable display-list items. Caret lines preserve painted
    // topology independently of the spatial hit-test index so keyboard navigation can reason about lines that contain
    // empty or zero-area caret targets.
    struct CaretLine {
        CSSPixelRect rect;
        Optional<CSSPixelRect> block_container_margin_rect;
        VisualContextIndex visual_context_index;
        size_t first_caret_item_index { 0 };
        size_t last_caret_item_index { 0 };
    };

    enum class CaretPositionType : u8 {
        Closest,
        Before,
        After,
    };

    void build_derived_structures_if_needed() const;
    void verify_no_item_appended_after_derived_structures_are_built() const { VERIFY(!m_derived_structures_built); }
    void add_item_to_spatial_index(size_t item_index) const;
    void add_item_to_caret_items(size_t item_index) const;
    SpatialIndex& spatial_index_for(VisualContextIndex) const;

    [[nodiscard]] Optional<CSSPixelPoint> local_point_for_visual_context(VisualContextIndex, CSSPixelPoint, ViewportPaintable const&, double device_pixels_per_css_pixel, AccumulatedVisualContextTree::ClipBehavior = AccumulatedVisualContextTree::ClipBehavior::Respect) const;
    [[nodiscard]] CSSPixelRect viewport_rect_for_item(Item const&, CSSPixelRect const&, ViewportPaintable const&, double device_pixels_per_css_pixel) const;
    [[nodiscard]] CSSPixelRect caret_line_rect_for_item(Item const&) const;
    [[nodiscard]] bool item_contains(Item const&, CSSPixelPoint local_point, ChromeMetrics const&) const;
    [[nodiscard]] DOM::Node const* item_dom_node(Item const&) const;
    [[nodiscard]] DOM::Node const* event_dispatch_dom_node_for_item(Item const&) const;
    [[nodiscard]] bool item_can_produce_caret_position(Item const&) const;
    [[nodiscard]] bool item_is_direct_caret_target(Item const&) const;
    [[nodiscard]] HitTestResult hit_test_result_for_item(Item const&, CSSPixelPoint local_point) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_item(Item const&, CSSPixelPoint local_point, CaretPositionType = CaretPositionType::Closest) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_hit_container(Item const&) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_for_line(CaretLine const&, CSSPixelPoint local_point, CaretPositionMode) const;
    [[nodiscard]] Item const& item_at_line_edge(CaretLine const&, CaretPositionType) const;
    [[nodiscard]] bool item_contains_caret_position(Item const&, DOM::Node const&, size_t offset, TextAffinity) const;
    [[nodiscard]] Optional<size_t> caret_line_index_for_position(DOM::Node const&, size_t offset, TextAffinity) const;
    [[nodiscard]] bool line_contains_descendant_of(CaretLine const&, DOM::Node const&) const;
    [[nodiscard]] bool item_is_inline_adjacent_to_line(Item const&, CaretLine const&) const;
    void find_topmost_item_in_list(Vector<size_t> const&, CSSPixelPoint local_point, ChromeMetrics const&, Optional<size_t>& topmost_item_index) const;
    void find_topmost_caret_item_in_list(Vector<size_t> const&, CSSPixelPoint local_point, ChromeMetrics const&, Optional<size_t>& topmost_item_index) const;
    void find_items_in_list(Vector<size_t> const&, CSSPixelPoint local_point, ChromeMetrics const&, Vector<size_t>& hit_item_indices) const;

    static bool items_equal_for_cache_verification(Item const&, Item const&);
    static String dump_item_for_cache_verification(Item const&);

    u64 m_visual_context_tree_version { 0 };
    u64 m_id { 0 };
    Vector<Item> m_items;
    mutable bool m_derived_structures_built { false };
    mutable Vector<size_t> m_caret_item_indices;
    mutable Vector<CaretLine> m_caret_lines;
    mutable Vector<OwnPtr<SpatialIndex>> m_spatial_indexes;
    mutable Vector<VisualContextIndex> m_used_visual_context_indices;
};

}

}
