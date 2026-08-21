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

namespace Web::Painting {

struct FlexboxInspectorOverlayOptions;
struct GridInspectorOverlayOptions;
class HitTestDisplayList;
class Paintable;
class PaintableWithLines;
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

GC::Ptr<DOM::Node> event_dispatch_dom_node_for(Paintable const&);
RefPtr<Paintable> paintable_for_slot(void* arena_handle, Layout::RustFFI::PaintableSlotId);

bool body_background_is_propagated_to_root(Layout::NodeWithStyle const&);

void invalidate_descendant_styles_for_container_query_size_change(GC::Ptr<DOM::Node>, CSSPixelSize old_content_size, CSSPixelSize new_content_size);

// Used grid track data captured at layout time as plain values; getComputedStyle
// reflection mints style values from it on demand.
struct UsedGridTrackList {
    bool is_subgrid { false };
    // One entry per grid line (one more line than there are tracks, unless subgrid);
    // a line's name list may be empty.
    Vector<CSS::GridLineNames> lines;
    Vector<CSSPixels> track_sizes;
};

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

struct OverflowData {
    CSSPixelRect scrollable_overflow_rect;
    bool has_scrollable_overflow { false };
};

struct CachedOverflowData {
    CSSPixelRect rect_relative_to_padding_box;
    bool has_scrollable_overflow { false };
};

class WEB_API Paintable
    : public RefCounted<Paintable>
    , public Weakable<Paintable> {
public:
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::Painting);

    static NonnullRefPtr<Paintable> create(Layout::Box const&);
    virtual ~Paintable();

    Layout::RustFFI::PaintableKind kind() const { return rust_data().kind; }

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

    GC::Ptr<HTML::LocalNavigable> navigable() const;

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

    enum class ScrollBlockDirection {
        No,
        Yes,
    };

    static void scroll_text_offset_into_view(DOM::Text const&, size_t offset, TextAffinity = TextAffinity::Downstream, ScrollBlockDirection = ScrollBlockDirection::Yes);
    void scroll_ancestor_to_offset_into_view(size_t offset);

    friend class Layout::Node;

    virtual void reset_for_relayout();

    Layout::RustFFI::PaintableSlotId rust_slot() const { return m_rust_slot; }
    Layout::NodeArena& rust_arena() const { return *m_rust_arena; }
    Layout::RustFFI::PaintableData const& rust_data() const { return *m_rust_data; }

    Optional<CSSPixelRect> get_mask_area() const;
    Optional<Gfx::MaskKind> get_mask_type() const;
    Optional<CSSPixelRect> get_clip_area() const;

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

    RefPtr<Paintable const> nearest_scrollable_ancestor() const;

    [[nodiscard]] bool could_be_scrolled_by_wheel_event() const;
    [[nodiscard]] bool could_be_scrolled_by_wheel_event(ScrollDirection direction) const;

    [[nodiscard]] size_t visual_context_nodes_begin() const { return rust_data().visual_context_nodes_begin; }
    [[nodiscard]] size_t visual_context_nodes_end() const { return rust_data().visual_context_nodes_end; }

protected:
    explicit Paintable(Layout::NodeWithStyle const&);
    explicit Paintable(Layout::Box const&);

public:
protected:
    CSSPixels available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& chrome_metrics) const;

public:
    Optional<CSSPixelRect> absolute_resizer_rect(ChromeMetrics const& chrome_metrics) const;

private:
    void detach_from_layout_node(Badge<Layout::Node>);
    void detach_chrome_widgets();
    GC::Ptr<DOM::EventTarget> scroll_event_target();

    bool has_flag(Layout::RustFFI::PaintableFlag flag) const { return (rust_data().flags & to_underlying(flag)) != 0; }
    Layout::RustFFI::PaintableData& rust_data() { return *m_rust_data; }
    GC::Weak<DOM::Node> m_dom_node;
    WeakPtr<Layout::NodeWithStyle const> m_layout_node;

    NonnullRefPtr<Layout::NodeArena> m_rust_arena;
    Layout::RustFFI::PaintableSlotId m_rust_slot {};
    u32 m_rust_slot_generation { 0 };
    Layout::RustFFI::PaintableData* m_rust_data { nullptr };

    ResolvedCSSFilter m_filter;

    RefPtr<Scrollbar> m_horizontal_scrollbar;
    RefPtr<Scrollbar> m_vertical_scrollbar;
    RefPtr<ResizeHandle> m_resize_handle;
};

template<>
inline bool Paintable::fast_is<PaintableWithLines>() const { return is_paintable_with_lines(); }

}
