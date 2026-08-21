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
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
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
}

void Paintable::reset_for_relayout()
{
    Painting::invalidate_stacking_context(layout_node());
}

}
