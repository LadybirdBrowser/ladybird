/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/BorderPainting.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/ClipFrame.h>
#include <LibWeb/Painting/ClippableAndScrollable.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

class PaintableBox : public Paintable
    , public ClippableAndScrollable {
    JS_CELL(PaintableBox, Paintable);

public:
    static JS::NonnullGCPtr<PaintableBox> create(Layout::Box const&);
    virtual ~PaintableBox();

    virtual void before_paint(PaintContext&, PaintPhase) const override;
    virtual void after_paint(PaintContext&, PaintPhase) const override;

    virtual void paint(PaintContext&, PaintPhase) const override;

    virtual Optional<CSSPixelRect> get_masking_area() const;
    virtual Optional<Gfx::Bitmap::MaskKind> get_mask_type() const;
    virtual RefPtr<Gfx::Bitmap> calculate_mask(PaintContext&, CSSPixelRect const&) const;

    Layout::Box& layout_box() { return static_cast<Layout::Box&>(Paintable::layout_node()); }
    Layout::Box const& layout_box() const { return static_cast<Layout::Box const&>(Paintable::layout_node()); }

    auto const& box_model() const { return layout_box().box_model(); }

    struct OverflowData {
        CSSPixelRect scrollable_overflow_rect;
        bool has_scrollable_overflow { false };
        CSSPixelPoint scroll_offset {};
    };

    CSSPixelRect absolute_rect() const;

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

    CSSPixelRect absolute_padding_box_rect() const
    {
        auto absolute_rect = this->absolute_rect();
        CSSPixelRect rect;
        rect.set_x(absolute_rect.x() - box_model().padding.left);
        rect.set_width(content_width() + box_model().padding.left + box_model().padding.right);
        rect.set_y(absolute_rect.y() - box_model().padding.top);
        rect.set_height(content_height() + box_model().padding.top + box_model().padding.bottom);
        return rect;
    }

    CSSPixelRect absolute_border_box_rect() const
    {
        auto padded_rect = this->absolute_padding_box_rect();
        CSSPixelRect rect;
        auto use_collapsing_borders_model = override_borders_data().has_value();
        // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
        auto border_top = use_collapsing_borders_model ? round(box_model().border.top / 2) : box_model().border.top;
        auto border_bottom = use_collapsing_borders_model ? round(box_model().border.bottom / 2) : box_model().border.bottom;
        auto border_left = use_collapsing_borders_model ? round(box_model().border.left / 2) : box_model().border.left;
        auto border_right = use_collapsing_borders_model ? round(box_model().border.right / 2) : box_model().border.right;
        rect.set_x(padded_rect.x() - border_left);
        rect.set_width(padded_rect.width() + border_left + border_right);
        rect.set_y(padded_rect.y() - border_top);
        rect.set_height(padded_rect.height() + border_top + border_bottom);
        return rect;
    }

    CSSPixelRect absolute_paint_rect() const;

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

    [[nodiscard]] bool has_scrollable_overflow() const { return m_overflow_data->has_scrollable_overflow; }

    [[nodiscard]] Optional<CSSPixelRect> scrollable_overflow_rect() const
    {
        if (!m_overflow_data.has_value())
            return {};
        return m_overflow_data->scrollable_overflow_rect;
    }

    void set_overflow_data(OverflowData data) { m_overflow_data = move(data); }

    DOM::Node const* dom_node() const { return layout_box().dom_node(); }
    DOM::Node* dom_node() { return layout_box().dom_node(); }

    virtual void set_needs_display() const override;

    virtual void apply_scroll_offset(PaintContext&, PaintPhase) const override;
    virtual void reset_scroll_offset(PaintContext&, PaintPhase) const override;

    virtual void apply_clip_overflow_rect(PaintContext&, PaintPhase) const override;
    virtual void clear_clip_overflow_rect(PaintContext&, PaintPhase) const override;

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

    void set_transform(Gfx::FloatMatrix4x4 transform) { m_transform = transform; }
    Gfx::FloatMatrix4x4 const& transform() const { return m_transform; }

    void set_transform_origin(CSSPixelPoint transform_origin) { m_transform_origin = transform_origin; }
    CSSPixelPoint const& transform_origin() const { return m_transform_origin; }

    void set_outline_data(Optional<BordersData> outline_data) { m_outline_data = outline_data; }
    Optional<BordersData> const& outline_data() const { return m_outline_data; }

    void set_outline_offset(CSSPixels outline_offset) { m_outline_offset = outline_offset; }
    CSSPixels outline_offset() const { return m_outline_offset; }

    CSSPixelRect compute_absolute_padding_rect_with_css_transform_applied() const;

    Optional<CSSPixelRect> get_clip_rect() const;

    bool is_viewport() const { return layout_box().is_viewport(); }

    virtual bool wants_mouse_events() const override;

    virtual void resolve_paint_properties() override;

protected:
    explicit PaintableBox(Layout::Box const&);

    virtual void paint_border(PaintContext&) const;
    virtual void paint_backdrop_filter(PaintContext&) const;
    virtual void paint_background(PaintContext&) const;
    virtual void paint_box_shadow(PaintContext&) const;

    virtual CSSPixelRect compute_absolute_rect() const;
    virtual CSSPixelRect compute_absolute_paint_rect() const;

    enum class ScrollDirection {
        Horizontal,
        Vertical,
    };
    [[nodiscard]] Optional<CSSPixelRect> scroll_thumb_rect(ScrollDirection) const;
    [[nodiscard]] bool is_scrollable(ScrollDirection) const;

    TraversalDecision hit_test_scrollbars(CSSPixelPoint position, Function<TraversalDecision(HitTestResult)> const& callback) const;

private:
    [[nodiscard]] virtual bool is_paintable_box() const final { return true; }

    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers) override;
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers) override;

    Optional<OverflowData> m_overflow_data;

    CSSPixelPoint m_offset;
    CSSPixelSize m_content_size;

    Optional<CSSPixelRect> mutable m_absolute_rect;
    Optional<CSSPixelRect> mutable m_absolute_paint_rect;

    mutable bool m_clipping_overflow { false };
    mutable Vector<u32> m_corner_clipper_ids;

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
};

class PaintableWithLines : public PaintableBox {
    JS_CELL(PaintableWithLines, PaintableBox);

public:
    static JS::NonnullGCPtr<PaintableWithLines> create(Layout::BlockContainer const&);
    virtual ~PaintableWithLines() override;

    Layout::BlockContainer const& layout_box() const;
    Layout::BlockContainer& layout_box();

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
            visitor.visit(JS::NonnullGCPtr { fragment.layout_node() });
    }

    virtual void resolve_paint_properties() override;

protected:
    PaintableWithLines(Layout::BlockContainer const&);

private:
    [[nodiscard]] virtual bool is_paintable_with_lines() const final { return true; }

    Vector<PaintableFragment> m_fragments;
};

void paint_text_decoration(PaintContext&, TextPaintable const&, PaintableFragment const&);
void paint_cursor_if_needed(PaintContext&, TextPaintable const&, PaintableFragment const&);
void paint_text_fragment(PaintContext&, TextPaintable const&, PaintableFragment const&, PaintPhase);

}
