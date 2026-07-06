/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/Forward.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/FlexLayoutData.h>
#include <LibWeb/Layout/GridLayoutData.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/HitTestResult.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollFrame.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/RefCountedTreeNode.h>

namespace Web::Painting {

struct FlexboxInspectorOverlayOptions;
struct GridInspectorOverlayOptions;
class ResizeHandle;
class Scrollbar;

WEB_API void set_paint_viewport_scrollbars(bool enabled);
bool should_paint_viewport_scrollbars();
ResolvedCSSFilter resolve_css_filter(CSS::Filter const& computed_filter, Paintable const& paintable_box);

class WEB_API Paintable
    : public RefCounted<Paintable>
    , public Weakable<Paintable>
    , public RefCountedTreeNode<Paintable> {
public:
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::Painting);

    static NonnullRefPtr<Paintable> create(Layout::Box const&);
    static NonnullRefPtr<Paintable> create(Layout::InlineNode const&);
    virtual ~Paintable();
    virtual StringView class_name() const { return "Paintable"sv; }

    [[nodiscard]] bool is_visible() const
    {
        auto const& cv = computed_values();
        return cv.visibility() == CSS::Visibility::Visible && cv.opacity() != 0;
    }
    [[nodiscard]] bool is_positioned() const { return m_positioned; }
    [[nodiscard]] bool is_fixed_position() const { return m_fixed_position; }
    [[nodiscard]] bool is_sticky_position() const { return m_sticky_position; }
    [[nodiscard]] bool is_absolutely_positioned() const { return m_absolutely_positioned; }
    [[nodiscard]] bool is_floating() const { return m_floating; }
    [[nodiscard]] bool is_inline() const { return m_inline; }
    [[nodiscard]] CSS::Display display() const { return m_display; }

    bool has_stacking_context() const;
    RefPtr<StackingContext> enclosing_stacking_context();

    void paint_inspector_overlay(DisplayListRecordingContext&) const;

    virtual bool forms_unconnected_subtree() const { return false; }

    Layout::Node const& layout_node() const
    {
        VERIFY(m_layout_node);
        return *m_layout_node;
    }
    Layout::Node& layout_node() { return const_cast<Layout::Node&>(const_cast<Paintable const&>(*this).layout_node()); }

    [[nodiscard]] GC::Ptr<DOM::Node> dom_node();
    [[nodiscard]] GC::Ptr<DOM::Node const> dom_node() const;
    void set_dom_node(GC::Ptr<DOM::Node>);

    CSS::ImmutableComputedValues const& computed_values() const;

    bool visible_for_hit_testing() const;

    GC::Ptr<HTML::LocalNavigable> navigable() const;

    RefPtr<Paintable> containing_block() const;

    template<typename T>
    bool fast_is() const = delete;

    [[nodiscard]] virtual bool is_navigable_container_viewport_paintable() const { return false; }
    [[nodiscard]] virtual bool is_viewport_paintable() const { return false; }
    [[nodiscard]] virtual bool is_paintable_with_lines() const { return false; }
    [[nodiscard]] virtual bool is_svg_paintable() const { return false; }
    [[nodiscard]] virtual bool is_svg_svg_paintable() const { return false; }
    [[nodiscard]] virtual bool is_svg_path_paintable() const { return false; }
    [[nodiscard]] virtual bool is_svg_graphics_paintable() const { return false; }

    DOM::Document const& document() const;
    DOM::Document& document();

    CSSPixelPoint box_type_agnostic_position() const;

    static void scroll_text_offset_into_view(DOM::Text const&, size_t offset);
    void scroll_ancestor_to_offset_into_view(size_t offset);

    using SelectionState = Painting::SelectionState;

    SelectionState selection_state() const { return m_selection_state; }
    void set_selection_state(SelectionState state);

    struct TextDecorationStyle {
        Vector<CSS::TextDecorationLine> line;
        CSS::TextDecorationStyle style;
        Color color;
    };
    struct SelectionStyle {
        Color background_color;
        Optional<Color> text_color {};
        Optional<Vector<ShadowData>> text_shadow {};
        Optional<TextDecorationStyle> text_decoration {};

        bool has_styling() const
        {
            return background_color.alpha() > 0 || text_color.has_value() || text_shadow.has_value() || text_decoration.has_value();
        }
    };
    [[nodiscard]] SelectionStyle selection_style() const;
    [[nodiscard]] static SelectionStyle selection_style_for_node(Layout::Node const&, GC::Ptr<DOM::Node const>);

    [[nodiscard]] String debug_description() const;

    friend class Layout::Node;

    virtual void reset_for_relayout();

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const;
    virtual void record_hit_test_items(DisplayListRecordingContext&, PaintPhase) const;
    void record_async_scrolling_metadata(DisplayListRecordingContext&) const;

    RefPtr<StackingContext> stacking_context();
    RefPtr<StackingContext const> stacking_context() const;
    void set_stacking_context(NonnullRefPtr<StackingContext>);
    void invalidate_stacking_context();
    Optional<int> effective_z_index() const;

    virtual Optional<CSSPixelRect> get_mask_area() const { return {}; }
    virtual Optional<Gfx::MaskKind> get_mask_type() const { return {}; }
    virtual Optional<DisplayListResource> calculate_mask(DisplayListRecordingContext&, CSSPixelRect const&) const { return {}; }

    virtual Optional<CSSPixelRect> get_clip_area() const { return {}; }
    virtual Optional<DisplayListResource> calculate_clip(DisplayListRecordingContext&, CSSPixelRect const&) const { return {}; }

    Layout::NodeWithStyleAndBoxModelMetrics const& layout_node_with_style_and_box_metrics() const;

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
    ScrollHandled scroll_by(double delta_x, double delta_y);
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

    enum class FragmentationState {
        Unfragmented,
        HorizontalStart,
        HorizontalMiddle,
        HorizontalEnd,
        VerticalStart,
        VerticalMiddle,
        VerticalEnd
    };
    void set_fragmentation_state(FragmentationState);

    CSSPixelRect absolute_rect() const;
    CSSPixelRect absolute_padding_box_rect() const;
    CSSPixelRect absolute_border_box_rect() const;
    CSSPixelRect overflow_clip_edge_rect() const;

    // These united versions of the above rects include all paintables for this layout node.
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

    void set_containing_line_box_data(LineBoxData line_box_data) { m_containing_line_box_data = line_box_data; }
    Optional<LineBoxData> const& containing_line_box_data() const { return m_containing_line_box_data; }
    Optional<CSSPixelRect> absolute_containing_line_box_rect() const;

    CSSPixelPoint transform_to_local_coordinates(CSSPixelPoint position) const;

    [[nodiscard]] bool has_scrollable_overflow() const
    {
        if (!m_overflow_data.has_value())
            return false;
        return m_overflow_data->has_scrollable_overflow;
    }

    [[nodiscard]] bool has_css_transform() const;

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

    virtual void set_needs_repaint(InvalidateDisplayList = InvalidateDisplayList::Yes);

    virtual bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y);

    struct ScrollbarData {
        CSSPixelRect gutter_rect;
        CSSPixelRect thumb_rect;
        CSSPixelFraction thumb_travel_to_scroll_ratio { 0 };
    };
    enum class ScrollDirection {
        Horizontal,
        Vertical,
    };
    enum class ScrollbarSizing {
        Current,
        Regular,
        Enlarged,
    };

    Optional<ScrollbarData> compute_scrollbar_data(
        ScrollDirection direction,
        ChromeMetrics const& chrome_metrics,
        ScrollStateSnapshot const* = nullptr,
        ScrollbarSizing = ScrollbarSizing::Current) const;
    Optional<CSSPixelRect> absolute_scrollbar_rect(ScrollDirection direction, bool with_gutter, ChromeMetrics const& chrome_metrics) const;

    RefPtr<Scrollbar> scrollbar(ScrollDirection) const;
    NonnullRefPtr<Scrollbar> ensure_scrollbar(ScrollDirection);

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

    static BordersData remove_element_kind_from_borders_data(Paintable::BordersDataWithElementKind borders_data);

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

    BorderRadiiData border_radii_data() const;

    Optional<BordersData> outline_data() const;
    CSSPixels outline_offset() const;

    void set_filter(ResolvedCSSFilter filter) { m_filter = move(filter); }
    ResolvedCSSFilter const& filter() const { return m_filter; }

    Optional<CSSPixelRect> get_clip_rect() const;

    struct PhysicalResizeAxes {
        bool horizontal;
        bool vertical;
    };
    PhysicalResizeAxes physical_resize_axes() const;

    bool resizer_contains(CSSPixelPoint adjusted_position, ChromeMetrics const& chrome_metrics) const;
    bool is_chrome_mirrored() const;
    bool has_resizer() const;

    RefPtr<ResizeHandle> resize_handle() const;
    NonnullRefPtr<ResizeHandle> ensure_resize_handle();

    CSSPixelRect transform_reference_box() const;

    ScrollFrameIndex nearest_scroll_frame_index() const;

    RefPtr<Paintable const> nearest_scrollable_ancestor() const;

    using StickyInsets = Painting::StickyInsets;
    bool has_sticky_insets() const { return !!m_sticky_insets; }
    StickyInsets const& sticky_insets() const { return *m_sticky_insets; }
    void set_sticky_insets(OwnPtr<StickyInsets> sticky_insets) { m_sticky_insets = move(sticky_insets); }

    [[nodiscard]] bool could_be_scrolled_by_wheel_event() const;
    [[nodiscard]] bool could_be_scrolled_by_wheel_event(ScrollDirection direction) const;

    void set_used_values_for_grid_template_columns(RefPtr<CSS::GridTrackSizeListStyleValue const> style_value) { m_used_values_for_grid_template_columns = move(style_value); }
    RefPtr<CSS::GridTrackSizeListStyleValue const> const& used_values_for_grid_template_columns() const { return m_used_values_for_grid_template_columns; }

    void set_used_values_for_grid_template_rows(RefPtr<CSS::GridTrackSizeListStyleValue const> style_value) { m_used_values_for_grid_template_rows = move(style_value); }
    RefPtr<CSS::GridTrackSizeListStyleValue const> const& used_values_for_grid_template_rows() const { return m_used_values_for_grid_template_rows; }

    void set_grid_layout_data(OwnPtr<Layout::GridLayoutData> grid_layout_data) { m_grid_layout_data = move(grid_layout_data); }
    Layout::GridLayoutData const* grid_layout_data() const { return m_grid_layout_data.ptr(); }
    void set_flex_layout_data(OwnPtr<Layout::FlexLayoutData> flex_layout_data) { m_flex_layout_data = move(flex_layout_data); }
    Layout::FlexLayoutData const* flex_layout_data() const { return m_flex_layout_data.ptr(); }
    void paint_flexbox_inspector_overlay(DisplayListRecordingContext&, FlexboxInspectorOverlayOptions const&) const;
    void paint_grid_inspector_overlay(DisplayListRecordingContext&, GridInspectorOverlayOptions const&) const;

    void set_enclosing_scroll_frame_index(ScrollFrameIndex index) { m_enclosing_scroll_frame_index = index; }
    void set_own_scroll_frame_index(ScrollFrameIndex index) { m_own_scroll_frame_index = index; }

    void set_accumulated_visual_context(VisualContextIndex index) { m_accumulated_visual_context_index = index; }
    [[nodiscard]] VisualContextIndex accumulated_visual_context_index() const { return m_accumulated_visual_context_index; }
    void set_accumulated_visual_context_for_descendants(VisualContextIndex index) { m_accumulated_visual_context_for_descendants_index = index; }
    [[nodiscard]] VisualContextIndex accumulated_visual_context_for_descendants_index() const { return m_accumulated_visual_context_for_descendants_index; }

    Optional<CSSPixelPoint> transform_point_to_local(CSSPixelPoint screen_position) const;
    Optional<CSSPixelPoint> transform_point_to_local_for_descendants(CSSPixelPoint screen_position) const;
    CSSPixelRect transform_rect_to_viewport(CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform = AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes) const;
    CSSPixelPoint inverse_transform_point(CSSPixelPoint screen_position) const;

    static constexpr size_t paint_phase_count = to_underlying(PaintPhase::Overlay) + 1;

    void invalidate_paint_cache() const;

    bool has_cached_commands(PaintPhase) const;
    ReadonlyBytes cached_commands(PaintPhase) const;
    void set_cached_commands(PaintPhase phase, ByteBuffer const& commands) const;

    void set_fixed_background_visual_context(VisualContextIndex index) { m_fixed_background_visual_context = index; }
    [[nodiscard]] Optional<VisualContextIndex> fixed_background_visual_context() const { return m_fixed_background_visual_context; }

    // Range of visual context nodes this box appended during the last tree build, used for in-place value patching.
    void set_visual_context_node_range(size_t begin, size_t end)
    {
        m_visual_context_nodes_begin = begin;
        m_visual_context_nodes_end = end;
    }
    [[nodiscard]] size_t visual_context_nodes_begin() const { return m_visual_context_nodes_begin; }
    [[nodiscard]] size_t visual_context_nodes_end() const { return m_visual_context_nodes_end; }

    [[nodiscard]] ScrollFrameIndex enclosing_scroll_frame_index() const { return m_enclosing_scroll_frame_index; }

    [[nodiscard]] ScrollFrameIndex own_scroll_frame_index() const { return m_own_scroll_frame_index; }

protected:
    explicit Paintable(Layout::Node const&);
    explicit Paintable(Layout::Box const&);
    explicit Paintable(Layout::InlineNode const&);

    void paint_with_inspector_overlay_context(DisplayListRecordingContext&, Function<void()> const&) const;
    bool has_layout_node() const { return m_layout_node; }

    virtual void paint_border(DisplayListRecordingContext&) const;
    virtual void paint_backdrop_filter(DisplayListRecordingContext&) const;
    virtual void paint_background(DisplayListRecordingContext&) const;
    virtual void paint_box_shadow(DisplayListRecordingContext&) const;

    virtual void paint_inspector_overlay_internal(DisplayListRecordingContext&) const;

    virtual CSSPixelRect compute_absolute_rect() const;

    CSSPixels available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& chrome_metrics) const;
    Optional<CSSPixelRect> absolute_resizer_rect(ChromeMetrics const& chrome_metrics) const;

    Optional<WeakPtr<Paintable>> mutable m_containing_block;

private:
    struct CachedPaintData;
    enum class InvalidateDescendantGeometry {
        No,
        Yes,
    };

    void detach_from_layout_node(Badge<Layout::Node>)
    {
        m_containing_block.clear();
        m_layout_node.clear();
    }

    void paint_middle_button_scroll_indicator(DisplayListRecordingContext&) const;
    void acquire_cache_references_for_cached_commands(ReadonlyBytes) const;
    void release_cache_references_for_cached_commands(ReadonlyBytes) const;
    void invalidate_absolute_geometry_cache(InvalidateDescendantGeometry);

    GC::Weak<DOM::Node> m_dom_node;
    WeakPtr<Layout::Node const> m_layout_node;

    SelectionState m_selection_state { SelectionState::None };

    bool m_positioned : 1 { false };
    bool m_fixed_position : 1 { false };
    bool m_sticky_position : 1 { false };
    bool m_absolutely_positioned : 1 { false };
    bool m_floating : 1 { false };
    bool m_inline : 1 { false };
    CSS::Display m_display;

    RefPtr<StackingContext> m_stacking_context;

    Optional<OverflowData> m_overflow_data;

    CSSPixelPoint m_offset;
    CSSPixelSize m_content_size;

    Optional<CSSPixelRect> mutable m_absolute_rect;
    Optional<CSSPixelRect> mutable m_absolute_padding_box_rect;
    Optional<CSSPixelRect> mutable m_absolute_border_box_rect;

    ScrollFrameIndex m_enclosing_scroll_frame_index {};
    ScrollFrameIndex m_own_scroll_frame_index {};
    VisualContextIndex m_accumulated_visual_context_index { VISUAL_VIEWPORT_NODE_INDEX };
    VisualContextIndex m_accumulated_visual_context_for_descendants_index { VISUAL_VIEWPORT_NODE_INDEX };
    Optional<VisualContextIndex> m_fixed_background_visual_context;
    size_t m_visual_context_nodes_begin { 0 };
    size_t m_visual_context_nodes_end { 0 };

    Optional<BordersDataWithElementKind> m_override_borders_data;
    Optional<TableCellCoordinates> m_table_cell_coordinates;
    Optional<LineBoxData> m_containing_line_box_data;

    ResolvedCSSFilter m_filter;

    RefPtr<Scrollbar> m_horizontal_scrollbar;
    RefPtr<Scrollbar> m_vertical_scrollbar;
    RefPtr<ResizeHandle> m_resize_handle;
    bool m_has_non_invertible_css_transform { false };

    OwnPtr<StickyInsets> m_sticky_insets;

    RefPtr<CSS::GridTrackSizeListStyleValue const> m_used_values_for_grid_template_columns;
    RefPtr<CSS::GridTrackSizeListStyleValue const> m_used_values_for_grid_template_rows;
    OwnPtr<Layout::GridLayoutData> m_grid_layout_data;
    OwnPtr<Layout::FlexLayoutData> m_flex_layout_data;

    BoxModelMetrics m_box_model;

    // FIXME: This is not how this is meant to work in the spec. The box needs to be drawn in full and then sliced
    //        visually, in case something like border-radius is in effect.
    //        ( see https://drafts.csswg.org/css-break/#valdef-box-decoration-break-slice )
    bool m_fragment_top_edge_away { false };
    bool m_fragment_left_edge_away { false };
    bool m_fragment_right_edge_away { false };
    bool m_fragment_bottom_edge_away { false };

    mutable OwnPtr<CachedPaintData> m_cached_paint_data;
};

template<>
inline bool Paintable::fast_is<PaintableWithLines>() const { return is_paintable_with_lines(); }

WEB_API Painting::BorderRadiiData normalize_border_radii_data(CSSPixelRect const& border_rect, CSSPixelRect const& reference_rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius);

}
