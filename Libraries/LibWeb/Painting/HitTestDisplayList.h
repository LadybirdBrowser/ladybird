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
#include <LibGfx/Path.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web {

struct ChromeMetrics;

namespace Painting {

class PaintableFragment;
class ViewportPaintable;

enum class CaretPositionMode : u8 {
    Normal,
    // Starting or extending selection should feel more eager than the public caret-position API in inter-line gaps.
    SelectionStart,
    Selection,
};

class WEB_API HitTestDisplayList : public RefCounted<HitTestDisplayList> {
public:
    static NonnullRefPtr<HitTestDisplayList> create(u64 visual_context_tree_version);

    void append_box(PaintableBox const&, Paintable& target, CSSPixelRect, VisualContextIndex, BorderRadiiData);
    void append_svg_path(Paintable& target, Gfx::Path, Gfx::WindingRule, CSSPixelRect bounding_box, VisualContextIndex);
    void append_text_fragment(PaintableFragment const&, VisualContextIndex);
    void append_empty_editable(PaintableWithLines const&, CSSPixelRect, VisualContextIndex);
    void append_chrome_widget(PaintableBox const&, ChromeWidget&, VisualContextIndex);

    u64 visual_context_tree_version() const { return m_visual_context_tree_version; }
    [[nodiscard]] Optional<HitTestResult> hit_test(CSSPixelPoint, HitTestType, ViewportPaintable const&, double device_pixels_per_css_pixel, ChromeMetrics const&) const;
    [[nodiscard]] Optional<CaretPosition> caret_position_from_point(CSSPixelPoint, ViewportPaintable const&, double device_pixels_per_css_pixel, ChromeMetrics const&, CaretPositionMode = CaretPositionMode::Normal) const;
    TraversalDecision hit_test_all(CSSPixelPoint, ViewportPaintable const&, double device_pixels_per_css_pixel, ChromeMetrics const&, Function<TraversalDecision(HitTestResult)> const&) const;

private:
    explicit HitTestDisplayList(u64 visual_context_tree_version);

    enum class ItemKind : u8 {
        Box,
        SvgPath,
        TextFragment,
        EmptyEditable,
        ChromeWidget,
    };

    struct Item {
        ItemKind kind;
        NonnullRefPtr<Paintable> paintable;
        RefPtr<ChromeWidget> chrome_widget;
        PaintableFragment const* text_fragment { nullptr };
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

    void add_item_to_spatial_index(size_t item_index);
    void add_item_to_caret_items(size_t item_index);
    SpatialIndex& spatial_index_for(VisualContextIndex);

    [[nodiscard]] Optional<CSSPixelPoint> local_point_for_visual_context(VisualContextIndex, CSSPixelPoint, ViewportPaintable const&, double device_pixels_per_css_pixel) const;
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
    [[nodiscard]] bool line_contains_descendant_of(CaretLine const&, DOM::Node const&) const;
    [[nodiscard]] bool item_is_inline_adjacent_to_line(Item const&, CaretLine const&) const;
    void find_topmost_item_in_list(Vector<size_t> const&, CSSPixelPoint local_point, ChromeMetrics const&, Optional<size_t>& topmost_item_index) const;
    void find_topmost_caret_item_in_list(Vector<size_t> const&, CSSPixelPoint local_point, ChromeMetrics const&, Optional<size_t>& topmost_item_index) const;
    void find_items_in_list(Vector<size_t> const&, CSSPixelPoint local_point, ChromeMetrics const&, Vector<size_t>& hit_item_indices) const;

    u64 m_visual_context_tree_version { 0 };
    Vector<Item> m_items;
    Vector<size_t> m_caret_item_indices;
    Vector<CaretLine> m_caret_lines;
    Vector<OwnPtr<SpatialIndex>> m_spatial_indexes;
    Vector<VisualContextIndex> m_used_visual_context_indices;
};

}

}
