/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/BorderPainting.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/ClipFrame.h>
#include <LibWeb/Painting/ClippableAndScrollable.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

extern bool g_paint_viewport_scrollbars;

class PaintableBox : public Paintable
    , public ClippableAndScrollable {
    GC_CELL(PaintableBox, Paintable);

public:
    static GC::Ref<PaintableBox> create(Layout::Box const&);
    static GC::Ref<PaintableBox> create(Layout::InlineNode const&);
    virtual ~PaintableBox();

    virtual void before_paint(PaintContext&, PaintPhase) const override;
    virtual void after_paint(PaintContext&, PaintPhase) const override;

    virtual void paint(PaintContext&, PaintPhase) const override;

    StackingContext* stacking_context() { return m_stacking_context; }
    StackingContext const* stacking_context() const { return m_stacking_context; }
    void set_stacking_context(NonnullOwnPtr<StackingContext>);
    void invalidate_stacking_context();

    virtual Optional<CSSPixelRect> get_masking_area() const;
    virtual Optional<Gfx::Bitmap::MaskKind> get_mask_type() const { return {}; }
    virtual RefPtr<Gfx::ImmutableBitmap> calculate_mask(PaintContext&, CSSPixelRect const&) const { return {}; }

    Layout::NodeWithStyleAndBoxModelMetrics& layout_node_with_style_and_box_metrics() { return static_cast<Layout::NodeWithStyleAndBoxModelMetrics&>(Paintable::layout_node()); }
    Layout::NodeWithStyleAndBoxModelMetrics const& layout_node_with_style_and_box_metrics() const { return static_cast<Layout::NodeWithStyleAndBoxModelMetrics const&>(Paintable::layout_node()); }

    auto& box_model() { return m_box_model; }
    auto const& box_model() const { return m_box_model; }

    struct OverflowData {
        CSSPixelRect scrollable_overflow_rect;
        bool has_scrollable_overflow { false };
        CSSPixelPoint scroll_offset {};
    };

    // Offset from the top left of the containing block's content edge.
    [[nodiscard]] CSSPixelPoint offset() const;

    CSSPixelPoint scroll_offset() const;
    void set_scroll_offset(CSSPixelPoint);
    void scroll_by(int delta_x, int delta_y);

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
    CSSPixelRect absolute_paint_rect() const;

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

    [[nodiscard]] bool has_scrollable_overflow() const
    {
        if (!m_overflow_data.has_value())
            return false;
        return m_overflow_data->has_scrollable_overflow;
    }

    [[nodiscard]] bool has_css_transform() const
    {
        if (!computed_values().transformations().is_empty())
            return true;
        if (computed_values().rotate().has_value())
            return true;
        if (computed_values().translate().has_value())
            return true;
        if (computed_values().scale().has_value())
            return true;
        return false;
    }

    [[nodiscard]] Optional<CSSPixelRect> scrollable_overflow_rect() const
    {
        if (!m_overflow_data.has_value())
            return {};
        return m_overflow_data->scrollable_overflow_rect;
    }

    void set_overflow_data(OverflowData data) { m_overflow_data = move(data); }

    DOM::Node const* dom_node() const { return layout_node_with_style_and_box_metrics().dom_node(); }
    DOM::Node* dom_node() { return layout_node_with_style_and_box_metrics().dom_node(); }

    virtual void set_needs_display(InvalidateDisplayList = InvalidateDisplayList::Yes) override;

    virtual void apply_scroll_offset(PaintContext&, PaintPhase) const override;
    virtual void reset_scroll_offset(PaintContext&, PaintPhase) const override;

    virtual void apply_clip_overflow_rect(PaintContext&, PaintPhase) const override;
    virtual void clear_clip_overflow_rect(PaintContext&, PaintPhase) const override;

    [[nodiscard]] virtual TraversalDecision hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const override;
    Optional<HitTestResult> hit_test(CSSPixelPoint, HitTestType) const;
    [[nodiscard]] TraversalDecision hit_test_continuation(Function<TraversalDecision(HitTestResult)> const& callback) const;

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

    void set_transform(Gfx::FloatMatrix4x4 transform) { m_transform = transform; }
    Gfx::FloatMatrix4x4 const& transform() const { return m_transform; }

    void set_transform_origin(CSSPixelPoint transform_origin) { m_transform_origin = transform_origin; }
    CSSPixelPoint const& transform_origin() const { return m_transform_origin; }

    void set_outline_data(Optional<BordersData> outline_data) { m_outline_data = outline_data; }
    Optional<BordersData> const& outline_data() const { return m_outline_data; }

    void set_outline_offset(CSSPixels outline_offset) { m_outline_offset = outline_offset; }
    CSSPixels outline_offset() const { return m_outline_offset; }

    Optional<CSSPixelRect> get_clip_rect() const;

    bool is_viewport() const { return layout_node_with_style_and_box_metrics().is_viewport(); }

    virtual bool wants_mouse_events() const override;

    CSSPixelRect transform_box_rect() const;
    virtual void resolve_paint_properties() override;

    RefPtr<ScrollFrame const> nearest_scroll_frame() const;

    CSSPixelRect border_box_rect_relative_to_nearest_scrollable_ancestor() const;
    PaintableBox const* nearest_scrollable_ancestor() const;

    struct StickyInsets {
        Optional<CSSPixels> top;
        Optional<CSSPixels> right;
        Optional<CSSPixels> bottom;
        Optional<CSSPixels> left;
    };
    StickyInsets const& sticky_insets() const { return *m_sticky_insets; }
    void set_sticky_insets(OwnPtr<StickyInsets> sticky_insets) { m_sticky_insets = move(sticky_insets); }

    [[nodiscard]] bool could_be_scrolled_by_wheel_event() const;

    void set_used_values_for_grid_template_columns(RefPtr<CSS::GridTrackSizeListStyleValue> style_value) { m_used_values_for_grid_template_columns = move(style_value); }
    RefPtr<CSS::GridTrackSizeListStyleValue> const& used_values_for_grid_template_columns() const { return m_used_values_for_grid_template_columns; }

    void set_used_values_for_grid_template_rows(RefPtr<CSS::GridTrackSizeListStyleValue> style_value) { m_used_values_for_grid_template_rows = move(style_value); }
    RefPtr<CSS::GridTrackSizeListStyleValue> const& used_values_for_grid_template_rows() const { return m_used_values_for_grid_template_rows; }

protected:
    explicit PaintableBox(Layout::Box const&);
    explicit PaintableBox(Layout::InlineNode const&);

    virtual void paint_border(PaintContext&) const;
    virtual void paint_backdrop_filter(PaintContext&) const;
    virtual void paint_background(PaintContext&) const;
    virtual void paint_box_shadow(PaintContext&) const;

    virtual CSSPixelRect compute_absolute_rect() const;
    virtual CSSPixelRect compute_absolute_paint_rect() const;

    struct ScrollbarData {
        CSSPixelRect thumb_rect;
        CSSPixelFraction scroll_length;
    };
    enum class ScrollDirection {
        Horizontal,
        Vertical,
    };
    Optional<ScrollbarData> compute_scrollbar_data(ScrollDirection) const;
    [[nodiscard]] Optional<CSSPixelRect> scroll_thumb_rect(ScrollDirection) const;
    [[nodiscard]] bool could_be_scrolled_by_wheel_event(ScrollDirection) const;

    TraversalDecision hit_test_scrollbars(CSSPixelPoint position, Function<TraversalDecision(HitTestResult)> const& callback) const;

private:
    [[nodiscard]] virtual bool is_paintable_box() const final { return true; }

    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers) override;

    OwnPtr<StackingContext> m_stacking_context;

    Optional<OverflowData> m_overflow_data;

    CSSPixelPoint m_offset;
    CSSPixelSize m_content_size;

    Optional<CSSPixelRect> mutable m_absolute_rect;
    Optional<CSSPixelRect> mutable m_absolute_paint_rect;

    RefPtr<ScrollFrame const> m_enclosing_scroll_frame;
    RefPtr<ClipFrame const> m_enclosing_clip_frame;

    Optional<BordersDataWithElementKind> m_override_borders_data;
    Optional<TableCellCoordinates> m_table_cell_coordinates;

    BorderRadiiData m_border_radii_data;
    Vector<ShadowData> m_box_shadow_data;
    Gfx::FloatMatrix4x4 m_transform { Gfx::FloatMatrix4x4::identity() };
    CSSPixelPoint m_transform_origin;

    Optional<BordersData> m_outline_data;
    CSSPixels m_outline_offset { 0 };

    Optional<CSSPixelPoint> m_last_mouse_tracking_position;
    Optional<ScrollDirection> m_scroll_thumb_dragging_direction;

    ResolvedBackground m_resolved_background;

    OwnPtr<StickyInsets> m_sticky_insets;

    RefPtr<CSS::GridTrackSizeListStyleValue> m_used_values_for_grid_template_columns;
    RefPtr<CSS::GridTrackSizeListStyleValue> m_used_values_for_grid_template_rows;

    BoxModelMetrics m_box_model;
};

class PaintableWithLines : public PaintableBox {
    GC_CELL(PaintableWithLines, PaintableBox);

public:
    static GC::Ref<PaintableWithLines> create(Layout::BlockContainer const&);
    static GC::Ref<PaintableWithLines> create(Layout::InlineNode const&, size_t line_index);
    virtual ~PaintableWithLines() override;

    Layout::NodeWithStyleAndBoxModelMetrics const& layout_node_with_style_and_box_metrics() const;
    Layout::NodeWithStyleAndBoxModelMetrics& layout_node_with_style_and_box_metrics();

    Vector<PaintableFragment> const& fragments() const { return m_fragments; }
    Vector<PaintableFragment>& fragments() { return m_fragments; }

    void add_fragment(Layout::LineBoxFragment const& fragment)
    {
        m_fragments.append(PaintableFragment { fragment });
    }

    void set_fragments(Vector<PaintableFragment>&& fragments) { m_fragments = move(fragments); }

    template<typename Callback>
    void for_each_fragment(Callback callback) const
    {
        for (auto& fragment : m_fragments) {
            if (callback(fragment) == IterationDecision::Break)
                return;
        }
    }

    virtual void paint(PaintContext&, PaintPhase) const override;

    [[nodiscard]] virtual TraversalDecision hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const override;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        for (auto& fragment : m_fragments)
            visitor.visit(GC::Ref { fragment.layout_node() });
    }

    virtual void resolve_paint_properties() override;

    size_t line_index() const { return m_line_index; }

protected:
    PaintableWithLines(Layout::BlockContainer const&);
    PaintableWithLines(Layout::InlineNode const&, size_t line_index);

private:
    [[nodiscard]] virtual bool is_paintable_with_lines() const final { return true; }

    Vector<PaintableFragment> m_fragments;

    size_t m_line_index { 0 };
};

void paint_text_decoration(PaintContext&, TextPaintable const&, PaintableFragment const&);
void paint_cursor_if_needed(PaintContext&, TextPaintable const&, PaintableFragment const&);
void paint_text_fragment(PaintContext&, TextPaintable const&, PaintableFragment const&, PaintPhase);

}
