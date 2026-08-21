/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/GenericShorthands.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

DOM::Document const& Paintable::document() const
{
    return layout_node().document();
}

DOM::Document& Paintable::document()
{
    return layout_node().document();
}

void Paintable::set_dom_node(GC::Ptr<DOM::Node> dom_node)
{
    m_dom_node = dom_node.ptr();
}

GC::Ptr<DOM::Node> Paintable::dom_node()
{
    return m_dom_node.ptr();
}

GC::Ptr<DOM::Node const> Paintable::dom_node() const
{
    return m_dom_node.ptr();
}

GC::Ptr<HTML::LocalNavigable> Paintable::navigable() const
{
    return document().navigable();
}

GC::Ptr<DOM::Node> event_dispatch_dom_node_for(Paintable const& paintable)
{
    auto* layout_node_shell = Layout::RustFFI::layout_arena_paintable_event_dispatch_node_shell(paintable.rust_arena().handle(), paintable.rust_slot());
    if (!layout_node_shell)
        return nullptr;
    return static_cast<Layout::Node*>(layout_node_shell)->dom_node();
}

RefPtr<Paintable> paintable_for_slot(void* arena_handle, Layout::RustFFI::PaintableSlotId slot)
{
    auto* layout_node_shell = Layout::RustFFI::layout_arena_paintable_layout_node_shell(arena_handle, slot);
    if (!layout_node_shell)
        return nullptr;
    return static_cast<Layout::Node*>(layout_node_shell)->paintable();
}

RefPtr<Paintable> HitTestResult::paintable() const
{
    return paintable_for_slot(arena->handle(), box);
}

RefPtr<Paintable> CaretPosition::paintable() const
{
    return paintable_for_slot(arena->handle(), box);
}

static bool g_paint_viewport_scrollbars = true;

static bool content_size_change_affects_container_queries(Layout::NodeWithStyle const& layout_node, CSSPixelSize old_size, CSSPixelSize new_size)
{
    auto container_type = layout_node.container_type();
    if (container_type.is_size_container)
        return old_size != new_size;

    if (!container_type.is_inline_size_container)
        return false;

    if (layout_node.writing_mode() == CSS::WritingMode::HorizontalTb)
        return old_size.width() != new_size.width();

    return old_size.height() != new_size.height();
}

void invalidate_descendant_styles_for_container_query_size_change(GC::Ptr<DOM::Node> node, CSSPixelSize old_size, CSSPixelSize new_size)
{
    auto* element = as_if<DOM::Element>(node.ptr());
    if (!element)
        return;

    auto const* layout_node = element->unsafe_layout_node();
    if (!layout_node || !content_size_change_affects_container_queries(*layout_node, old_size, new_size))
        return;

    CSS::Invalidation::invalidate_descendant_styles_depending_on_size_container_query(*element);
}

void set_paint_viewport_scrollbars(bool const enabled)
{
    g_paint_viewport_scrollbars = enabled;
}

bool should_paint_viewport_scrollbars()
{
    return g_paint_viewport_scrollbars;
}

bool body_background_is_propagated_to_root(Layout::NodeWithStyle const& layout_node)
{
    if (!layout_node.is_body())
        return false;
    // Reachable at invalidation time, when the root element's layout node may already be detached.
    auto const* html_element = layout_node.document().html_element();
    return html_element && html_element->unsafe_layout_node() && html_element->should_use_body_background_properties();
}

static bool is_canvas_background_source(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.is_root_element() || body_background_is_propagated_to_root(layout_node);
}

static Color effective_scrollbar_background_color(Paintable const& paintable_box)
{
    auto background_color = paintable_box.document().canvas_background_color();

    Vector<Layout::NodeWithStyle const*, 32> ancestors;
    for (Layout::NodeWithStyle const* layout_node = &paintable_box.layout_node(); layout_node; layout_node = layout_node->parent())
        ancestors.append(layout_node);

    for (auto const* layout_node : ancestors.in_reverse()) {
        auto const& layout_node_with_style = *layout_node;
        if (is_canvas_background_source(layout_node_with_style))
            continue;

        auto color = layout_node_with_style.background_color();
        if (color.alpha() == 0)
            continue;

        background_color = background_color.blend(color);
    }

    return background_color;
}

static CSS::ScrollbarColorData automatic_scrollbar_colors(Paintable const& paintable_box)
{
    auto background_color = effective_scrollbar_background_color(paintable_box);
    auto black_thumb = Color(Color::Black).with_alpha(128);
    auto white_thumb = Color(Color::White).with_alpha(128);

    auto black_thumb_contrast = background_color.contrast_ratio(background_color.blend(black_thumb));
    auto white_thumb_contrast = background_color.contrast_ratio(background_color.blend(white_thumb));
    auto thumb_color = black_thumb_contrast >= white_thumb_contrast ? black_thumb : white_thumb;

    return {
        .thumb_color = thumb_color,
        .track_color = thumb_color.with_alpha(25),
        .is_auto = true,
    };
}

CSS::ScrollbarColorData scrollbar_colors_for_paint(Paintable const& paintable_box)
{
    auto scrollbar_colors = paintable_box.layout_node().scrollbar_color();
    if (!scrollbar_colors.is_auto)
        return scrollbar_colors;

    return automatic_scrollbar_colors(paintable_box);
}

Paintable const* nearest_svg_viewport_paintable_of(Layout::Node const& layout_node)
{
    for (auto const* ancestor = layout_node.parent(); ancestor; ancestor = ancestor->parent()) {
        if (auto paintable = ancestor->paintable(); paintable && Painting::svg_viewport_transform(*ancestor).has_value())
            return paintable.ptr();
    }
    return nullptr;
}

// active_view_box covers <view> redirection and the svg-as-image fallback viewBox, which
// layout used to build the geometry these callers interpret.
static Gfx::FloatRect svg_svg_box_view_box_or_viewport_rect(Layout::Box const& svg_svg_box)
{
    if (auto view_box = as<SVG::SVGSVGElement>(*svg_svg_box.dom_node()).active_view_box(); view_box.has_value())
        return { view_box->min_x, view_box->min_y, view_box->width, view_box->height };
    if (svg_svg_box.paintable_box()) {
        auto viewport_size = Painting::svg_viewport_size(svg_svg_box);
        return { {}, { viewport_size.width().to_float(), viewport_size.height().to_float() } };
    }
    return {};
}

Gfx::FloatRect svg_viewport_user_rect(Paintable const& viewport_paintable)
{
    if (viewport_paintable.layout_node().is_svg_svg_box())
        return svg_svg_box_view_box_or_viewport_rect(static_cast<Layout::Box const&>(viewport_paintable.layout_node()));
    if (auto dom_node = viewport_paintable.dom_node()) {
        if (auto const* fit_to_view_box = as_if<SVG::SVGFitToViewBox>(*dom_node)) {
            if (auto view_box = fit_to_view_box->view_box(); view_box.has_value())
                return { static_cast<float>(view_box->min_x), static_cast<float>(view_box->min_y), static_cast<float>(view_box->width), static_cast<float>(view_box->height) };
        }
    }
    return { {}, Painting::absolute_rect(viewport_paintable.layout_node()).size().to_type<float>() };
}

ResolvedCSSFilter resolve_css_filter(CSS::ComputedFilterView computed_filter, Paintable const& paintable_box)
{
    auto const& layout_node = paintable_box.layout_node();

    ResolvedCSSFilter result;
    bool failed = false;
    computed_filter.for_each_operation([&](auto const& operation) {
        if (failed)
            return;
        if (auto const* url = operation.template get_pointer<CSS::Filter::Url>()) {
            if (url->fragment.is_empty()) {
                failed = true;
                return;
            }
            auto maybe_filter = paintable_box.document().get_element_by_id(url->fragment);
            if (!maybe_filter) {
                failed = true;
                return;
            }
            if (auto* filter_element = as_if<SVG::SVGFilterElement>(*maybe_filter)) {
                // Filter primitive lengths are specified in the filtered element's user coordinate system, but the
                // resulting filter operates in device pixels. Compute the user-unit-to-device-pixel scale so the
                // filter can convert its lengths accordingly.
                // The replay-time layer maps filter parameters through the accumulated transform,
                // so only the device pixel ratio — which lives in recorded coordinates, not in the
                // transform chain — converts here.
                auto device_pixels_per_css_pixel = paintable_box.document().page().client().device_pixels_per_css_pixel();
                auto filter_scale = Gfx::FloatPoint { device_pixels_per_css_pixel, device_pixels_per_css_pixel };
                result.svg_filter = filter_element->gfx_filter(layout_node, filter_scale);
                // The bounds live in the filtered element's user space; an element without
                // geometry of its own falls back to the whole enclosing viewport rect there.
                auto bounds = Painting::absolute_border_box_rect(layout_node);
                if (bounds.is_empty()) {
                    if (auto const* viewport_paintable = nearest_svg_viewport_paintable_of(paintable_box.layout_node()))
                        result.svg_filter_bounds = svg_viewport_user_rect(*viewport_paintable).to_type<CSSPixels>();
                }
                if (!bounds.is_empty())
                    result.svg_filter_bounds = bounds;
            } else {
                failed = true;
            }
            return;
        }

        operation.visit(
            [&](CSS::Filter::Blur const& blur) {
                result.operations.empend(ResolvedCSSFilter::Blur {
                    .radius = CSSPixels::nearest_value_for(blur.resolved_radius),
                });
            },
            [&](CSS::Filter::DropShadow const& drop_shadow) {
                result.operations.empend(ResolvedCSSFilter::DropShadow {
                    .offset_x = drop_shadow.offset_x,
                    .offset_y = drop_shadow.offset_y,
                    .radius = drop_shadow.radius,
                    .color = drop_shadow.color,
                });
            },
            [&](CSS::Filter::ColorOperation const& color_operation) {
                result.operations.empend(ResolvedCSSFilter::Color {
                    .operation = color_operation.operation,
                    .amount = color_operation.resolved_amount,
                });
            },
            [&](CSS::Filter::HueRotate const& hue_rotate) {
                result.operations.empend(ResolvedCSSFilter::HueRotate {
                    .angle_degrees = hue_rotate.angle_degrees,
                });
            },
            [&](CSS::Filter::Url const&) {});
    });
    if (failed)
        return {};
    return result;
}

NonnullRefPtr<Paintable> Paintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new Paintable(layout_box));
}

Paintable::Paintable(Layout::NodeWithStyle const& layout_node)
    : m_layout_node(layout_node)
    , m_rust_arena(layout_node.node_arena())
{
    auto allocation = m_rust_arena->paintable_row_for_node(Layout::Node::slot_id(&layout_node), this);
    m_rust_slot = allocation.slot;
    m_rust_slot_generation = allocation.generation;
    m_rust_data = allocation.data;
}

Paintable::Paintable(Layout::Box const& layout_box)
    : Paintable(static_cast<Layout::NodeWithStyle const&>(layout_box))
{
}

Paintable::~Paintable()
{
    m_rust_arena->paintable_shell_destroyed(m_rust_slot, m_rust_slot_generation, this);
}

void Paintable::detach_from_layout_node(Badge<Layout::Node>)
{
    m_layout_node.clear();
    detach_chrome_widgets();
}

void Paintable::detach_chrome_widgets()
{
    if (m_horizontal_scrollbar) {
        m_horizontal_scrollbar->detach_from_paintable({});
        m_horizontal_scrollbar = nullptr;
    }
    if (m_vertical_scrollbar) {
        m_vertical_scrollbar->detach_from_paintable({});
        m_vertical_scrollbar = nullptr;
    }
    if (m_resize_handle) {
        m_resize_handle->detach_from_paintable({});
        m_resize_handle = nullptr;
    }
}

void Paintable::reset_for_relayout()
{
    // A reused paintable must shed its chrome widgets: whether the box still warrants them
    // (e.g. scrollbars on a scroll container) is only known after the new layout is painted.
    detach_chrome_widgets();

    Painting::invalidate_stacking_context(layout_node());
}

Optional<CSSPixelRect> Paintable::absolute_resizer_rect(ChromeMetrics const& metrics) const
{
    if (!has_resizer())
        return {};
    auto padding_rect = Painting::absolute_padding_box_rect(layout_node());
    CSSPixels x = is_chrome_mirrored() ? padding_rect.x() : padding_rect.right() - metrics.resize_gripper_size;
    CSSPixels y = padding_rect.bottom() - metrics.resize_gripper_size;
    return CSSPixelRect { x, y, metrics.resize_gripper_size, metrics.resize_gripper_size };
}

RefPtr<Scrollbar> Paintable::scrollbar(ScrollDirection direction) const
{
    return direction == ScrollDirection::Horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
}

NonnullRefPtr<Scrollbar> Paintable::ensure_scrollbar(ScrollDirection direction)
{
    auto& slot = direction == ScrollDirection::Horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
    if (!slot)
        slot = Scrollbar::create(const_cast<Paintable&>(*this), direction);
    return *slot;
}

CSSPixels Paintable::available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& metrics) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto padding_rect = Painting::absolute_padding_box_rect(layout_node());
    CSSPixels full_scrollport_length = is_horizontal ? padding_rect.width() : padding_rect.height();
    if (has_resizer())
        full_scrollport_length -= metrics.resize_gripper_size;
    else {
        if (is_horizontal && could_be_scrolled_by_wheel_event(layout_node(), ScrollDirection::Vertical))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
        if (!is_horizontal && could_be_scrolled_by_wheel_event(layout_node(), ScrollDirection::Horizontal))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
    }
    return full_scrollport_length;
}

Optional<CSSPixelRect> Paintable::absolute_scrollbar_rect(ScrollDirection direction, bool with_gutter, ChromeMetrics const& metrics) const
{
    if (!could_be_scrolled_by_wheel_event(layout_node(), direction))
        return {};

    if (layout_node().scrollbar_width() == CSS::ScrollbarWidth::None)
        return {};

    bool is_horizontal = direction == ScrollDirection::Horizontal;
    bool adjusting_for_resizer = has_resizer();

    CSSPixels rect_thickness = with_gutter
        ? metrics.scroll_gutter_thickness
        : metrics.scroll_thumb_thickness_thin + metrics.scroll_thumb_padding_thin;
    CSSPixelRect scrollbar_rect = Painting::absolute_padding_box_rect(layout_node());

    if (is_horizontal) {
        if (!adjusting_for_resizer && could_be_scrolled_by_wheel_event(layout_node(), ScrollDirection::Vertical)) {
            scrollbar_rect.set_width(max(CSSPixels { 0 }, scrollbar_rect.width() - metrics.scroll_gutter_thickness));
            if (is_chrome_mirrored())
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.scroll_gutter_thickness);
        } else if (adjusting_for_resizer) {
            scrollbar_rect.set_width(available_scrollbar_length(ScrollDirection::Horizontal, metrics));
            if (is_chrome_mirrored())
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.resize_gripper_size);
        }
        scrollbar_rect.set_y(max(CSSPixels { 0 }, scrollbar_rect.bottom() - rect_thickness));
        scrollbar_rect.set_height(rect_thickness);
    } else {
        if (adjusting_for_resizer)
            scrollbar_rect.set_height(available_scrollbar_length(ScrollDirection::Vertical, metrics));
        if (!is_chrome_mirrored())
            scrollbar_rect.set_x(max(CSSPixels { 0 }, scrollbar_rect.right() - rect_thickness));
        scrollbar_rect.set_width(rect_thickness);
    }
    return scrollbar_rect;
}

Optional<Paintable::ScrollbarData> Paintable::compute_scrollbar_data(ScrollDirection direction, ChromeMetrics const& metrics, ScrollStateSnapshot const* scroll_state_snapshot, ScrollbarSizing scrollbar_sizing) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? layout_node().overflow_x() : layout_node().overflow_y();

    if (overflow != CSS::Overflow::Scroll && !could_be_scrolled_by_wheel_event(layout_node(), direction))
        return {};

    if (!Painting::own_scroll_node_index(layout_node()).value())
        return {};

    auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(layout_node());
    if (!scrollable_overflow_rect.has_value())
        return {};

    CSSPixels scrollable_overflow_length = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    if (scrollable_overflow_length == 0)
        return {};

    auto const& scrollbar = is_horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
    bool with_gutter = [&] {
        switch (scrollbar_sizing) {
        case ScrollbarSizing::Current:
            return scrollbar && scrollbar->is_enlarged();
        case ScrollbarSizing::Regular:
            return false;
        case ScrollbarSizing::Enlarged:
            return true;
        }
        VERIFY_NOT_REACHED();
    }();
    auto scrollbar_rect = absolute_scrollbar_rect(direction, with_gutter, metrics);
    if (!scrollbar_rect.has_value())
        return {};

    CSSPixels thumb_thickness = metrics.scroll_thumb_thickness_thin;
    CSSPixels thumb_margin = metrics.scroll_thumb_padding_thin;
    if (with_gutter) {
        thumb_thickness = metrics.scroll_thumb_thickness;
        thumb_margin = CSSPixels { (metrics.scroll_gutter_thickness - metrics.scroll_thumb_thickness) / 2.0 };
    }
    CSSPixels scrollbar_length = scrollbar_rect->primary_size_for_orientation(orientation);
    CSSPixels usable_scrollbar_length = max(CSSPixels { 0 }, scrollbar_length - (2 * thumb_margin));
    CSSPixels scrollport_size = Painting::absolute_padding_box_rect(layout_node()).primary_size_for_orientation(orientation);
    CSSPixels min_thumb_length = min(usable_scrollbar_length, metrics.scroll_thumb_min_length);
    CSSPixels thumb_length = max(usable_scrollbar_length * (scrollport_size / scrollable_overflow_length), min_thumb_length);

    ScrollbarData scrollbar_data = { .gutter_rect = {}, .thumb_rect = scrollbar_rect.value(), .track_rect = scrollbar_rect.value(), .thumb_travel_to_scroll_ratio = 0 };

    if (scrollable_overflow_length > scrollport_size)
        scrollbar_data.thumb_travel_to_scroll_ratio = (usable_scrollbar_length - thumb_length) / (scrollable_overflow_length - scrollport_size);

    scrollbar_data.thumb_rect.set_primary_size_for_orientation(orientation, thumb_length);
    scrollbar_data.thumb_rect.set_secondary_size_for_orientation(orientation, thumb_thickness);
    auto minimum_offset = minimum_scroll_offset(layout_node()).primary_offset_for_orientation(orientation);
    scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_margin - minimum_offset * scrollbar_data.thumb_travel_to_scroll_ratio);
    if (with_gutter || (!is_horizontal && is_chrome_mirrored()))
        scrollbar_data.thumb_rect.translate_secondary_offset_for_orientation(orientation, thumb_margin);
    if (with_gutter)
        scrollbar_data.gutter_rect = scrollbar_rect.value();

    if (scroll_state_snapshot) {
        auto own_offset = scroll_state_snapshot->device_offset_for_index(Painting::own_scroll_node_index(layout_node()));
        auto device_scroll_offset = is_horizontal ? -own_offset.x() : -own_offset.y();
        auto device_pixels_per_css_pixel = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
        CSSPixels thumb_offset = CSSPixels::nearest_value_for(device_scroll_offset / device_pixels_per_css_pixel) * scrollbar_data.thumb_travel_to_scroll_ratio;
        scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_offset);
    }

    return scrollbar_data;
}

bool Paintable::has_resizer() const
{
    // https://drafts.csswg.org/css-ui#resize
    if (is_viewport_paintable())
        return false;

    // The effect of the resize property on generated content is undefined.
    // Implementations should not apply the resize property to generated content.

    if (layout_node().generated_for_pseudo_element().has_value())
        return false;

    auto axes = physical_resize_axes();
    return axes.horizontal || axes.vertical;
}

bool Paintable::is_chrome_mirrored() const
{
    auto writing_mode = layout_node().writing_mode();
    return (writing_mode == CSS::WritingMode::HorizontalTb && layout_node().direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl;
}

RefPtr<ResizeHandle> Paintable::resize_handle() const
{
    return m_resize_handle;
}

NonnullRefPtr<ResizeHandle> Paintable::ensure_resize_handle()
{
    if (!m_resize_handle)
        m_resize_handle = ResizeHandle::create(*this);
    return *m_resize_handle;
}

bool Paintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, double wheel_delta_x, double wheel_delta_y)
{
    auto can_scroll_horizontally = could_be_scrolled_by_wheel_event(layout_node(), ScrollDirection::Horizontal);
    auto can_scroll_vertically = could_be_scrolled_by_wheel_event(layout_node(), ScrollDirection::Vertical);
    if (!can_scroll_horizontally)
        wheel_delta_x = 0;
    if (!can_scroll_vertically)
        wheel_delta_y = 0;

    // if none of the axes we scrolled with can be accepted by this element, don't handle scroll.
    if (wheel_delta_x == 0 && wheel_delta_y == 0)
        return false;

    return scroll_by(layout_node(), wheel_delta_x, wheel_delta_y) == ScrollHandled::Yes;
}

bool Paintable::resizer_contains(CSSPixelPoint adjusted_position, ChromeMetrics const& metrics) const
{
    auto handle_rect = absolute_resizer_rect(metrics);
    if (!handle_rect.has_value())
        return false;
    bool bottom_left_resizer = is_chrome_mirrored();
    auto box_model = Painting::box_model(layout_node());
    handle_rect->inflate(0, bottom_left_resizer ? 0 : box_model.border.right, box_model.border.bottom, bottom_left_resizer ? box_model.border.left : 0);

    return handle_rect->contains(adjusted_position);
}

Paintable::PhysicalResizeAxes Paintable::physical_resize_axes() const
{
    // https://drafts.csswg.org/css-ui/#resize
    if (layout_node().resize() == CSS::Resize::None)
        return {};

    // 4.1. ... The resize property applies to elements that are scroll containers. UAs may also apply it,
    // regardless of the value of the overflow property, to:
    // - Replaced elements representing images or videos, such as img, video, picture, svg, object, or canvas.
    // - The <iframe> element.
    if (layout_node().display().is_inline_outside() && layout_node().display().is_flow_inside())
        return {};

    bool horizontal_writing_mode = layout_node().writing_mode() == CSS::WritingMode::HorizontalTb;

    return {
        .horizontal = layout_node().overflow_x() != CSS::Overflow::Visible
            && layout_node().overflow_x() != CSS::Overflow::Clip
            && (layout_node().resize() == CSS::Resize::Both
                || layout_node().resize() == CSS::Resize::Horizontal
                || (layout_node().resize() == CSS::Resize::Inline && horizontal_writing_mode)
                || (layout_node().resize() == CSS::Resize::Block && !horizontal_writing_mode)),
        .vertical = layout_node().overflow_y() != CSS::Overflow::Visible
            && layout_node().overflow_y() != CSS::Overflow::Clip
            && (layout_node().resize() == CSS::Resize::Both
                || layout_node().resize() == CSS::Resize::Vertical
                || (layout_node().resize() == CSS::Resize::Inline && !horizontal_writing_mode)
                || (layout_node().resize() == CSS::Resize::Block && horizontal_writing_mode))
    };
}

}
