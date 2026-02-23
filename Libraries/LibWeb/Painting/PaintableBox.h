/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

WEB_API void set_paint_viewport_scrollbars(bool enabled);

class WEB_API PaintableBox : public Paintable {
    GC_CELL(PaintableBox, Paintable);
    GC_DECLARE_ALLOCATOR(PaintableBox);

public:
    static GC::Ref<PaintableBox> create(Layout::Box const&);
    static GC::Ref<PaintableBox> create(Layout::InlineNode const&);
    virtual ~PaintableBox();

    virtual void reset_for_relayout();

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    StackingContext* stacking_context() { return m_stacking_context; }
    StackingContext const* stacking_context() const { return m_stacking_context; }
    void set_stacking_context(GC::Ref<StackingContext>);
    void invalidate_stacking_context();

    virtual Optional<CSSPixelRect> get_mask_area() const { return {}; }
    virtual Optional<Gfx::MaskKind> get_mask_type() const { return {}; }
    virtual RefPtr<DisplayList> calculate_mask(DisplayListRecordingContext&, CSSPixelRect const&) const { return {}; }

    virtual Optional<CSSPixelRect> get_clip_area() const { return {}; }
    virtual RefPtr<DisplayList> calculate_clip(DisplayListRecordingContext&, CSSPixelRect const&) const { return {}; }

    Layout::NodeWithStyleAndBoxModelMetrics const& layout_node_with_style_and_box_metrics() const { return as<Layout::NodeWithStyleAndBoxModelMetrics const>(layout_node()); }

    auto& box_model() { return m_box_model; }
    auto const& box_model() const { return m_box_model; }

    struct OverflowData {
        CSSPixelRect scrollable_overflow_rect;
        bool has_scrollable_overflow { false };
        CSSPixelPoint scroll_offset {};
    };

    // Offset from the top left of the containing block's content edge.
    [[nodiscard]] CSSPixelPoint offset() const;

    enum class ScrollHandled {
        No,
        Yes,
    };

    CSSPixelPoint scroll_offset() const;
    ScrollHandled set_scroll_offset(CSSPixelPoint);
    ScrollHandled scroll_by(int delta_x, int delta_y);
    void scroll_into_view(CSSPixelRect);

    void set_offset(CSSPixelPoint);
    void set_offset(float x, float y) { set_offset({ x, y }); }

    CSSPixelSize const& content_size() const { return m_content_size; }
    void set_content_size(CSSPixelSize);
    void set_content_size(CSSPixels width, CSSPixels height) { set_content_size({ width, height }); }

    void set_content_width(CSSPixels width) { set_content_size(width, content_height()); }
    void set_content_height(CSSPixels height) { set_content_size(content_width(), height); }
    CSSPixels content_width() const { return m_content_size.width(); }
    CSSPixels content_height() const { return m_content_size.height(); }

    CSSPixelRect absolute_rect() const;
    CSSPixelRect absolute_padding_box_rect() const;
    CSSPixelRect absolute_border_box_rect() const;
    CSSPixelRect overflow_clip_edge_rect() const;

    // These united versions of the above rects take continuation into account.
    CSSPixelRect absolute_united_border_box_rect() const;
    CSSPixelRect absolute_united_content_rect() const;
    CSSPixelRect absolute_united_padding_box_rect() const;

    CSSPixels border_box_width() const
    {
        auto border_box = box_model().border_box();
        return content_width() + border_box.left + border_box.right;
    }

    CSSPixels border_box_height() const
    {
        auto border_box = box_model().border_box();
        return content_height() + border_box.top + border_box.bottom;
    }

    CSSPixels absolute_x() const { return absolute_rect().x(); }
    CSSPixels absolute_y() const { return absolute_rect().y(); }
    CSSPixelPoint absolute_position() const { return absolute_rect().location(); }

    CSSPixelPoint transform_to_local_coordinates(CSSPixelPoint position) const;

    [[nodiscard]] bool has_scrollable_overflow() const
    {
        if (!m_overflow_data.has_value())
            return false;
        return m_overflow_data->has_scrollable_overflow;
    }

    [[nodiscard]] bool has_css_transform() const
    {
        auto const& computed_values = this->computed_values();
        return !computed_values.transformations().is_empty()
            || computed_values.rotate()
            || computed_values.translate()
            || computed_values.scale();
    }

    [[nodiscard]] bool has_non_invertible_css_transform() const { return m_has_non_invertible_css_transform; }
    void set_has_non_invertible_css_transform(bool value) { m_has_non_invertible_css_transform = value; }

    [[nodiscard]] bool overflow_property_applies() const;

    [[nodiscard]] Optional<CSSPixelRect> scrollable_overflow_rect() const
    {
        if (!m_overflow_data.has_value())
            return {};
        return m_overflow_data->scrollable_overflow_rect;
    }

    void set_overflow_data(OverflowData data) { m_overflow_data = move(data); }

    virtual void set_needs_display(InvalidateDisplayList = InvalidateDisplayList::Yes) override;

    [[nodiscard]] virtual TraversalDecision hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const override;
    Optional<HitTestResult> hit_test(CSSPixelPoint, HitTestType) const;

    virtual bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y) override;

    enum class ConflictingElementKind {
        Cell,
        Row,
        RowGroup,
        Column,
        ColumnGroup,
        Table,
    };

    struct BorderDataWithElementKind {
        CSS::BorderData border_data;
        ConflictingElementKind element_kind;
    };

    struct BordersDataWithElementKind {
        BorderDataWithElementKind top;
        BorderDataWithElementKind right;
        BorderDataWithElementKind bottom;
        BorderDataWithElementKind left;
    };

    void set_override_borders_data(BordersDataWithElementKind const& override_borders_data) { m_override_borders_data = override_borders_data; }
    Optional<BordersDataWithElementKind> const& override_borders_data() const { return m_override_borders_data; }

    static BordersData remove_element_kind_from_borders_data(PaintableBox::BordersDataWithElementKind borders_data);

    struct TableCellCoordinates {
        size_t row_index;
        size_t column_index;
        size_t row_span;
        size_t column_span;
    };

    void set_table_cell_coordinates(TableCellCoordinates const& table_cell_coordinates) { m_table_cell_coordinates = table_cell_coordinates; }
    auto const& table_cell_coordinates() const { return m_table_cell_coordinates; }

    enum class ShrinkRadiiForBorders {
        Yes,
        No
    };

    BorderRadiiData normalized_border_radii_data(ShrinkRadiiForBorders shrink = ShrinkRadiiForBorders::No) const;

    BorderRadiiData const& border_radii_data() const { return m_border_radii_data; }
    void set_border_radii_data(BorderRadiiData const& border_radii_data) { m_border_radii_data = border_radii_data; }

    void set_box_shadow_data(Vector<ShadowData> box_shadow_data) { m_box_shadow_data = move(box_shadow_data); }
    Vector<ShadowData> const& box_shadow_data() const { return m_box_shadow_data; }

    void set_outline_data(Optional<BordersData> outline_data) { m_outline_data = outline_data; }
    Optional<BordersData> const& outline_data() const { return m_outline_data; }

    void set_outline_offset(CSSPixels outline_offset) { m_outline_offset = outline_offset; }
    CSSPixels outline_offset() const { return m_outline_offset; }

    void set_filter(ResolvedCSSFilter filter) { m_filter = move(filter); }
    ResolvedCSSFilter const& filter() const { return m_filter; }

    void set_backdrop_filter(ResolvedCSSFilter backdrop_filter) { m_backdrop_filter = move(backdrop_filter); }
    ResolvedCSSFilter const& backdrop_filter() const { return m_backdrop_filter; }

    Optional<CSSPixelRect> get_clip_rect() const;

    virtual bool wants_mouse_events() const override;

    CSSPixelRect transform_reference_box() const;
    virtual void resolve_paint_properties() override;

    RefPtr<ScrollFrame const> nearest_scroll_frame() const;

    PaintableBox const* nearest_scrollable_ancestor() const;

    using StickyInsets = Painting::StickyInsets;
    StickyInsets const& sticky_insets() const { return *m_sticky_insets; }
    void set_sticky_insets(OwnPtr<StickyInsets> sticky_insets) { m_sticky_insets = move(sticky_insets); }

    [[nodiscard]] bool could_be_scrolled_by_wheel_event() const;

    void set_used_values_for_grid_template_columns(RefPtr<CSS::GridTrackSizeListStyleValue const> style_value) { m_used_values_for_grid_template_columns = move(style_value); }
    RefPtr<CSS::GridTrackSizeListStyleValue const> const& used_values_for_grid_template_columns() const { return m_used_values_for_grid_template_columns; }

    void set_used_values_for_grid_template_rows(RefPtr<CSS::GridTrackSizeListStyleValue const> style_value) { m_used_values_for_grid_template_rows = move(style_value); }
    RefPtr<CSS::GridTrackSizeListStyleValue const> const& used_values_for_grid_template_rows() const { return m_used_values_for_grid_template_rows; }

    void set_enclosing_scroll_frame(RefPtr<ScrollFrame const> const& scroll_frame) { m_enclosing_scroll_frame = scroll_frame; }
    void set_own_scroll_frame(RefPtr<ScrollFrame> const& scroll_frame) { m_own_scroll_frame = scroll_frame; }

    void set_accumulated_visual_context(auto state) { m_accumulated_visual_context = move(state); }
    [[nodiscard]] auto accumulated_visual_context() const { return m_accumulated_visual_context; }
    void set_accumulated_visual_context_for_descendants(auto state) { m_accumulated_visual_context_for_descendants = move(state); }
    [[nodiscard]] auto accumulated_visual_context_for_descendants() const { return m_accumulated_visual_context_for_descendants; }

    [[nodiscard]] RefPtr<ScrollFrame const> enclosing_scroll_frame() const { return m_enclosing_scroll_frame; }
    [[nodiscard]] Optional<int> scroll_frame_id() const;

    [[nodiscard]] RefPtr<ScrollFrame const> own_scroll_frame() const { return m_own_scroll_frame; }
    [[nodiscard]] Optional<int> own_scroll_frame_id() const;

protected:
    explicit PaintableBox(Layout::Box const&);
    explicit PaintableBox(Layout::InlineNode const&);

    virtual void visit_edges(Visitor&) override;

    virtual void paint_border(DisplayListRecordingContext&) const;
    virtual void paint_backdrop_filter(DisplayListRecordingContext&) const;
    virtual void paint_background(DisplayListRecordingContext&) const;
    virtual void paint_box_shadow(DisplayListRecordingContext&) const;

    virtual void paint_inspector_overlay_internal(DisplayListRecordingContext&) const override;

    virtual CSSPixelRect compute_absolute_rect() const;

    struct ScrollbarData {
        CSSPixelRect gutter_rect;
        CSSPixelRect thumb_rect;
        CSSPixelFraction thumb_travel_to_scroll_ratio { 0 };
    };
    enum class ScrollDirection {
        Horizontal,
        Vertical,
    };
    [[nodiscard]] TraversalDecision hit_test_children(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const;
    [[nodiscard]] TraversalDecision hit_test_continuation(Function<TraversalDecision(HitTestResult)> const& callback) const;
    [[nodiscard]] TraversalDecision hit_test_chrome(CSSPixelPoint adjusted_position, Function<TraversalDecision(HitTestResult)> const& callback) const;

    Optional<ScrollbarData> compute_scrollbar_data(
        ScrollDirection direction,
        ChromeMetrics const& chrome_metrics,
        ScrollStateSnapshot const* = nullptr) const;
    CSSPixels available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& chrome_metrics) const;
    Optional<CSSPixelRect> absolute_scrollbar_rect(ScrollDirection direction, bool with_gutter, ChromeMetrics const& chrome_metrics) const;
    Optional<CSSPixelRect> absolute_resizer_rect(ChromeMetrics const& chrome_metrics) const;
    bool could_be_scrolled_by_wheel_event(ScrollDirection direction) const;
    bool resizer_contains(CSSPixelPoint adjusted_position, ChromeMetrics const& chrome_metrics) const;
    bool is_chrome_mirrored() const;
    bool has_resizer() const;

private:
    [[nodiscard]] virtual bool is_paintable_box() const final { return true; }

    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers) override;
    virtual void handle_mouseleave(Badge<EventHandler>) override;

    bool scrollbar_contains(ScrollDirection, CSSPixelPoint adjusted_position, ChromeMetrics const& chrome_metrics) const;
    void scroll_to_mouse_position(CSSPixelPoint, ChromeMetrics const& chrome_metrics);

    GC::Ptr<StackingContext> m_stacking_context;

    Optional<OverflowData> m_overflow_data;

    CSSPixelPoint m_offset;
    CSSPixelSize m_content_size;

    Optional<CSSPixelRect> mutable m_absolute_rect;
    Optional<CSSPixelRect> mutable m_absolute_padding_box_rect;
    Optional<CSSPixelRect> mutable m_absolute_border_box_rect;

    RefPtr<ScrollFrame const> m_enclosing_scroll_frame;
    RefPtr<ScrollFrame const> m_own_scroll_frame;
    RefPtr<AccumulatedVisualContext const> m_accumulated_visual_context;
    RefPtr<AccumulatedVisualContext const> m_accumulated_visual_context_for_descendants;

    Optional<BordersDataWithElementKind> m_override_borders_data;
    Optional<TableCellCoordinates> m_table_cell_coordinates;

    BorderRadiiData m_border_radii_data;
    Vector<ShadowData> m_box_shadow_data;
    Optional<BordersData> m_outline_data;
    CSSPixels m_outline_offset { 0 };

    ResolvedCSSFilter m_filter;
    ResolvedCSSFilter m_backdrop_filter;

    Optional<CSSPixels> m_scroll_thumb_grab_position;
    Optional<ScrollDirection> m_scroll_thumb_dragging_direction;
    mutable bool m_draw_enlarged_horizontal_scrollbar { false };
    mutable bool m_draw_enlarged_vertical_scrollbar { false };
    bool m_has_non_invertible_css_transform { false };

    ResolvedBackground m_resolved_background;

    OwnPtr<StickyInsets> m_sticky_insets;

    RefPtr<CSS::GridTrackSizeListStyleValue const> m_used_values_for_grid_template_columns;
    RefPtr<CSS::GridTrackSizeListStyleValue const> m_used_values_for_grid_template_rows;

    BoxModelMetrics m_box_model;
};

}
