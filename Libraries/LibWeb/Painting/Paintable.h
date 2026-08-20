/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/TypeCasts.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibGC/Ptr.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Display.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Forward.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BordersData.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/HitTestResult.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>
#include <LibWeb/TreeTraversal.h>

namespace Web::Painting {

struct FlexboxInspectorOverlayOptions;
struct GridInspectorOverlayOptions;
class HitTestDisplayList;
class Paintable;
class ResizeHandle;
class Scrollbar;

WEB_API void set_paint_viewport_scrollbars(bool enabled);
bool should_paint_viewport_scrollbars();
CSS::ScrollbarColorData scrollbar_colors_for_paint(Paintable const&);
ResolvedCSSFilter resolve_css_filter(CSS::ComputedFilterView computed_filter, Paintable const& paintable_box);

// Walks layout ancestors so it also covers content of unconnected resource subtrees.
WEB_API Paintable const* nearest_svg_viewport_paintable_of(Layout::Node const&);
// The viewport's rect in its own user units: the active viewBox rect, else {0,0} + the used size.
WEB_API Gfx::FloatRect svg_viewport_user_rect(Paintable const& viewport_paintable);

bool body_background_is_propagated_to_root(Layout::NodeWithStyle const&);

// Used grid track data captured at layout time as plain values; getComputedStyle
// reflection mints style values from it on demand.
struct UsedGridTrackList {
    bool is_subgrid { false };
    // One entry per grid line (one more line than there are tracks, unless subgrid);
    // a line's name list may be empty.
    Vector<CSS::GridLineNames> lines;
    Vector<CSSPixels> track_sizes;
};

class WEB_API Paintable
    : public RefCounted<Paintable>
    , public Weakable<Paintable> {
public:
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::Painting);

    static NonnullRefPtr<Paintable> create(Layout::Box const&);
    virtual ~Paintable();

    Layout::RustFFI::PaintableKind kind() const { return rust_data().kind; }
    StringView class_name() const;

    [[nodiscard]] bool is_visible() const;
    [[nodiscard]] bool is_positioned() const { return has_flag(Layout::RustFFI::PaintableFlag::Positioned); }
    [[nodiscard]] bool is_fixed_position() const { return has_flag(Layout::RustFFI::PaintableFlag::FixedPosition); }
    [[nodiscard]] bool is_sticky_position() const { return has_flag(Layout::RustFFI::PaintableFlag::StickyPosition); }
    [[nodiscard]] bool is_absolutely_positioned() const { return has_flag(Layout::RustFFI::PaintableFlag::AbsolutelyPositioned); }
    [[nodiscard]] bool is_floating() const { return has_flag(Layout::RustFFI::PaintableFlag::Floating); }
    [[nodiscard]] bool is_inline() const { return has_flag(Layout::RustFFI::PaintableFlag::Inline); }
    [[nodiscard]] CSS::Display display() const;

    bool has_stacking_context() const;

    Paintable* parent_ptr() { return shell_from_slot(rust_data().parent); }
    Paintable const* parent_ptr() const { return shell_from_slot(rust_data().parent); }
    RefPtr<Paintable> parent() { return parent_ptr(); }
    RefPtr<Paintable const> parent() const { return parent_ptr(); }
    Paintable* first_child_ptr() { return shell_from_slot(rust_data().first_child); }
    Paintable const* first_child_ptr() const { return shell_from_slot(rust_data().first_child); }
    RefPtr<Paintable> first_child() { return first_child_ptr(); }
    RefPtr<Paintable const> first_child() const { return first_child_ptr(); }
    Paintable* next_sibling_ptr() { return shell_from_slot(rust_data().next_sibling); }
    Paintable const* next_sibling_ptr() const { return shell_from_slot(rust_data().next_sibling); }
    RefPtr<Paintable> next_sibling() { return next_sibling_ptr(); }
    RefPtr<Paintable const> next_sibling() const { return next_sibling_ptr(); }
    bool has_children() const { return rust_data().first_child.index != Layout::RustFFI::INVALID_PAINTABLE_SLOT_INDEX; }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback)
    {
        return traverse_ref_counted_preorder(*this, IncludeRefCountedTreeRoot::Yes, move(callback));
    }
    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback) const
    {
        return traverse_ref_counted_preorder(*this, IncludeRefCountedTreeRoot::Yes, move(callback));
    }
    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback)
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](Paintable& paintable) {
            if (auto* paintable_of_type = as_if<U>(paintable))
                return callback(*paintable_of_type);
            return TraversalDecision::Continue;
        });
    }
    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback) const
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](Paintable const& paintable) {
            if (auto const* paintable_of_type = as_if<U>(paintable))
                return callback(*paintable_of_type);
            return TraversalDecision::Continue;
        });
    }
    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback)
    {
        return traverse_ref_counted_preorder(*this, IncludeRefCountedTreeRoot::No, move(callback));
    }
    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback) const
    {
        return traverse_ref_counted_preorder(*this, IncludeRefCountedTreeRoot::No, move(callback));
    }
    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback)
    {
        return for_each_in_subtree([callback = move(callback)](Paintable& paintable) {
            if (auto* paintable_of_type = as_if<U>(paintable))
                return callback(*paintable_of_type);
            return TraversalDecision::Continue;
        });
    }
    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback) const
    {
        return for_each_in_subtree([callback = move(callback)](Paintable const& paintable) {
            if (auto const* paintable_of_type = as_if<U>(paintable))
                return callback(*paintable_of_type);
            return TraversalDecision::Continue;
        });
    }
    template<typename Callback>
    void for_each_child(Callback callback)
    {
        for (auto* child = first_child_ptr(); child; child = child->next_sibling_ptr()) {
            if (callback(*child) == IterationDecision::Break)
                return;
        }
    }
    template<typename Callback>
    void for_each_child(Callback callback) const
    {
        for (auto const* child = first_child_ptr(); child; child = child->next_sibling_ptr()) {
            if (callback(*child) == IterationDecision::Break)
                return;
        }
    }
    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback)
    {
        for (auto* child = first_child_ptr(); child; child = child->next_sibling_ptr()) {
            if (auto* child_of_type = as_if<U>(*child)) {
                if (callback(*child_of_type) == IterationDecision::Break)
                    return;
            }
        }
    }
    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback) const
    {
        for (auto const* child = first_child_ptr(); child; child = child->next_sibling_ptr()) {
            if (auto const* child_of_type = as_if<U>(*child)) {
                if (callback(*child_of_type) == IterationDecision::Break)
                    return;
            }
        }
    }

    bool has_layout_node() const { return m_layout_node; }
    Layout::NodeWithStyle const& layout_node() const
    {
        VERIFY(m_layout_node);
        return *m_layout_node;
    }
    Layout::NodeWithStyle& layout_node() { return const_cast<Layout::NodeWithStyle&>(const_cast<Paintable const&>(*this).layout_node()); }

    [[nodiscard]] GC::Ptr<DOM::Node> dom_node();
    [[nodiscard]] GC::Ptr<DOM::Node const> dom_node() const;
    void set_dom_node(GC::Ptr<DOM::Node>);

    CSS::StyleRecordID style_record_identity() const;

    bool visible_for_hit_testing() const;

    GC::Ptr<HTML::LocalNavigable> navigable() const;

    RefPtr<Paintable> containing_block() const;
    Paintable const* containing_block_ptr() const;

    template<typename T>
    bool fast_is() const = delete;

    [[nodiscard]] bool is_navigable_container_viewport_paintable() const { return kind() == Layout::RustFFI::PaintableKind::NavigableContainerViewportPaintable; }
    [[nodiscard]] bool is_viewport_paintable() const { return kind() == Layout::RustFFI::PaintableKind::ViewportPaintable; }
    [[nodiscard]] bool is_paintable_with_lines() const
    {
        auto kind = this->kind();
        return kind == Layout::RustFFI::PaintableKind::PaintableWithLines
            || kind == Layout::RustFFI::PaintableKind::ViewportPaintable
            || kind == Layout::RustFFI::PaintableKind::SVGForeignObjectPaintable;
    }
    [[nodiscard]] bool is_inline_paintable() const { return kind() == Layout::RustFFI::PaintableKind::InlinePaintable; }
    [[nodiscard]] bool is_svg_paintable() const
    {
        switch (kind()) {
        case Layout::RustFFI::PaintableKind::SVGGraphicsPaintable:
        case Layout::RustFFI::PaintableKind::SVGPathPaintable:
        case Layout::RustFFI::PaintableKind::SVGImagePaintable:
        case Layout::RustFFI::PaintableKind::SVGMaskPaintable:
        case Layout::RustFFI::PaintableKind::SVGClipPaintable:
        case Layout::RustFFI::PaintableKind::SVGPatternPaintable:
            return true;
        default:
            return false;
        }
    }
    [[nodiscard]] bool is_svg_svg_paintable() const { return kind() == Layout::RustFFI::PaintableKind::SVGSVGPaintable; }
    [[nodiscard]] bool is_svg_path_paintable() const { return kind() == Layout::RustFFI::PaintableKind::SVGPathPaintable; }
    [[nodiscard]] bool is_svg_foreign_object_paintable() const { return kind() == Layout::RustFFI::PaintableKind::SVGForeignObjectPaintable; }

    DOM::Document const& document() const;
    DOM::Document& document();

    virtual CSSPixelPoint box_type_agnostic_position() const;

    enum class ScrollBlockDirection {
        No,
        Yes,
    };

    static void scroll_text_offset_into_view(DOM::Text const&, size_t offset, TextAffinity = TextAffinity::Downstream, ScrollBlockDirection = ScrollBlockDirection::Yes);
    void scroll_ancestor_to_offset_into_view(size_t offset);

    using SelectionState = Painting::SelectionState;

    SelectionState selection_state() const { return static_cast<SelectionState>(rust_data().selection_state); }
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

    Layout::RustFFI::PaintableSlotId rust_slot() const { return m_rust_slot; }
    Layout::NodeArena& rust_arena() const { return *m_rust_arena; }
    Layout::RustFFI::PaintableData const& rust_data() const { return *m_rust_data; }

    // The viewBox/preserveAspectRatio transform a viewport-establishing box applies to its
    // content, in content-box-local user coordinates. Presence marks the paintable as
    // viewport-establishing for the accumulated visual context tree.
    Optional<Gfx::AffineTransform> svg_viewport_transform() const;

    Gfx::Path const* committed_svg_path() const;

    bool should_paint_cursor() const;

    // Callers are responsible for checking that the element is empty and visible.

    void invalidate_stacking_context();
    Optional<int> effective_z_index() const;

    Optional<CSSPixelRect> get_mask_area() const;
    Optional<Gfx::MaskKind> get_mask_type() const;
    Optional<CSSPixelRect> get_clip_area() const;

    CSSPixelSize svg_viewport_size() const
    {
        return {
            CSSPixels::from_raw(rust_data().svg_viewport_size.width),
            CSSPixels::from_raw(rust_data().svg_viewport_size.height),
        };
    }

    BoxModelMetrics box_model() const;

    struct OverflowData {
        CSSPixelRect scrollable_overflow_rect;
        bool has_scrollable_overflow { false };
    };

    struct CachedOverflowData {
        CSSPixelRect rect_relative_to_padding_box;
        bool has_scrollable_overflow { false };
    };

    // Offset from the top left of the containing block's content edge.
    [[nodiscard]] CSSPixelPoint offset() const;

    enum class ScrollHandled {
        No,
        Yes,
    };

    CSSPixelPoint scroll_offset() const;
    CSSPixelPoint minimum_scroll_offset() const;
    CSSPixelPoint maximum_scroll_offset() const;
    CSSPixelPoint clamp_scroll_offset(CSSPixelPoint) const;
    CSSPixelRect scroll_snapport_rect() const;
    CSSPixelRect scroll_snapport_rect(CSSPixelRect scrollport) const;
    ScrollHandled set_scroll_offset(CSSPixelPoint);
    ScrollHandled set_scroll_offset_from_user_input(CSSPixelPoint);
    ScrollHandled scroll_by(double delta_x, double delta_y);
    void scroll_into_view(CSSPixelRect);

    void set_offset(CSSPixelPoint);
    void set_offset(float x, float y) { set_offset({ x, y }); }

    CSSPixelSize content_size() const;
    void set_content_size(CSSPixelSize);
    void set_content_size(CSSPixels width, CSSPixels height) { set_content_size({ width, height }); }

    void set_content_width(CSSPixels width) { set_content_size(width, content_height()); }
    void set_content_height(CSSPixels height) { set_content_size(content_width(), height); }
    CSSPixels content_width() const { return content_size().width(); }
    CSSPixels content_height() const { return content_size().height(); }

    CSSPixelRect absolute_rect() const;
    CSSPixelRect absolute_padding_box_rect() const;
    CSSPixelRect absolute_border_box_rect() const;

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

    [[nodiscard]] bool has_scrollable_overflow() const;

    [[nodiscard]] bool has_css_transform() const;

    [[nodiscard]] bool has_non_invertible_css_transform() const { return has_flag(Layout::RustFFI::PaintableFlag::HasNonInvertibleCssTransform); }

    [[nodiscard]] Optional<CSSPixelRect> scrollable_overflow_rect() const;

    [[nodiscard]] Optional<OverflowData> overflow_data() const;
    void clear_overflow_data();

    Optional<CachedOverflowData> cached_overflow_data() const;
    void clear_cached_overflow_data();

    virtual void set_needs_repaint(InvalidateDisplayList = InvalidateDisplayList::Yes);

    virtual bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y);

    struct ScrollbarData {
        CSSPixelRect gutter_rect;
        CSSPixelRect thumb_rect;
        CSSPixelRect track_rect;
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

    bool uses_collapsing_borders_model() const { return rust_data().uses_collapsing_borders_model; }

    BorderRadiiData border_radii_data() const;

    Optional<BordersData> outline_data() const;
    Optional<BordersData> outline_data(CSS::ComputedValues const&) const;
    CSSPixels outline_offset() const;

    void set_filter(ResolvedCSSFilter filter) { m_filter = move(filter); }
    ResolvedCSSFilter const& filter() const { return m_filter; }

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

    RefPtr<Paintable const> nearest_scrollable_ancestor() const;

    using StickyInsets = Painting::StickyInsets;
    bool has_sticky_insets() const { return rust_data().has_sticky_insets; }
    StickyInsets sticky_insets() const;
    void set_sticky_insets(OwnPtr<StickyInsets>);

    [[nodiscard]] bool could_be_scrolled_by_wheel_event() const;
    [[nodiscard]] bool could_be_scrolled_by_wheel_event(ScrollDirection direction) const;

    Optional<UsedGridTrackList> used_values_for_grid_template_columns() const;
    Optional<UsedGridTrackList> used_values_for_grid_template_rows() const;
    Optional<String> grid_layout_json(UniqueNodeID container_node_id) const;
    Optional<String> flex_layout_json(UniqueNodeID container_node_id) const;

    [[nodiscard]] bool has_accumulated_visual_context() const { return rust_data().has_accumulated_visual_context; }
    [[nodiscard]] VisualContextIndex accumulated_visual_context_index() const { return VisualContextIndex { rust_data().accumulated_visual_context_index }; }
    [[nodiscard]] VisualContextIndex accumulated_visual_context_for_descendants_index() const { return VisualContextIndex { rust_data().accumulated_visual_context_for_descendants_index }; }

    Optional<CSSPixelPoint> transform_point_to_local(CSSPixelPoint screen_position) const;
    Optional<CSSPixelPoint> transform_point_to_local_for_descendants(CSSPixelPoint screen_position) const;
    CSSPixelRect transform_rect_to_viewport(CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform = AccumulatedVisualContextTree::IncludeVisualViewportTransform::Yes) const;
    CSSPixelPoint inverse_transform_point(CSSPixelPoint screen_position) const;

    void invalidate_paint_cache() const;
    void repaint_after_style_change(CSS::RequiredInvalidationAfterStyleChange const&);

    [[nodiscard]] Optional<VisualContextIndex> fixed_background_visual_context() const
    {
        if (!rust_data().has_fixed_background_visual_context)
            return {};
        return VisualContextIndex { rust_data().fixed_background_visual_context };
    }

    [[nodiscard]] size_t visual_context_nodes_begin() const { return rust_data().visual_context_nodes_begin; }
    [[nodiscard]] size_t visual_context_nodes_end() const { return rust_data().visual_context_nodes_end; }

    [[nodiscard]] VisualContextIndex enclosing_scroll_node_index() const { return VisualContextIndex { rust_data().enclosing_scroll_node_index }; }

    [[nodiscard]] VisualContextIndex own_scroll_node_index() const { return VisualContextIndex { rust_data().own_scroll_node_index }; }

protected:
    explicit Paintable(Layout::NodeWithStyle const&);
    explicit Paintable(Layout::Box const&);

public:
protected:
    CSSPixelRect compute_absolute_rect() const;
    virtual CSSPixelRect compute_absolute_padding_box_rect() const;
    virtual CSSPixelRect compute_absolute_border_box_rect() const;

    CSSPixels available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& chrome_metrics) const;

public:
    Optional<CSSPixelRect> absolute_resizer_rect(ChromeMetrics const& chrome_metrics) const;

private:
    friend class Layout::LayoutRustBridge;

    enum class InvalidateDescendantGeometry {
        No,
        Yes,
    };

    void detach_from_layout_node(Badge<Layout::Node>);
    void detach_chrome_widgets();
    GC::Ptr<DOM::EventTarget> scroll_event_target();

    void invalidate_absolute_geometry_cache(InvalidateDescendantGeometry);
    void translate_reused_subtree_absolute_geometry(CSSPixelPoint);

    bool has_flag(Layout::RustFFI::PaintableFlag flag) const { return (rust_data().flags & to_underlying(flag)) != 0; }
    Layout::RustFFI::PaintableData& rust_data() { return *m_rust_data; }
    Paintable* shell_from_slot(Layout::RustFFI::PaintableSlotId) const;

    GC::Weak<DOM::Node> m_dom_node;
    WeakPtr<Layout::NodeWithStyle const> m_layout_node;

    NonnullRefPtr<Layout::NodeArena> m_rust_arena;
    Layout::RustFFI::PaintableSlotId m_rust_slot {};
    u32 m_rust_slot_generation { 0 };
    Layout::RustFFI::PaintableData* m_rust_data { nullptr };

    Optional<CSSPixelRect> mutable m_absolute_rect;
    Optional<CSSPixelRect> mutable m_absolute_padding_box_rect;
    Optional<CSSPixelRect> mutable m_absolute_border_box_rect;

    ResolvedCSSFilter m_filter;

    RefPtr<Scrollbar> m_horizontal_scrollbar;
    RefPtr<Scrollbar> m_vertical_scrollbar;
    RefPtr<ResizeHandle> m_resize_handle;
};

template<>
inline bool Paintable::fast_is<PaintableWithLines>() const { return is_paintable_with_lines(); }

WEB_API Painting::BorderRadiiData normalize_border_radii_data(CSSPixelRect const& border_rect, CSSPixelRect const& reference_rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius);

}
