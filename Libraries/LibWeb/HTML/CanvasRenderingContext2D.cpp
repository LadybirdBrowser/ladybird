/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/Rect.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/CanvasRenderingContext2DPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/Path2D.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<CanvasRenderingContext2D>> CanvasRenderingContext2D::create(JS::Realm& realm, HTMLCanvasElement& element, JS::Value options)
{
    auto context_attributes = TRY(CanvasRenderingContext2DSettings::from_js_value(realm.vm(), options));
    return realm.create<CanvasRenderingContext2D>(realm, element, context_attributes);
}

CanvasRenderingContext2D::CanvasRenderingContext2D(JS::Realm& realm, HTMLCanvasElement& element, CanvasRenderingContext2DSettings context_attributes)
    : Bindings::PlatformObject(realm)
    , AbstractCanvasRenderingContext2D(static_cast<Bindings::PlatformObject&>(*this), element, context_attributes)
{
}

void CanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::CanvasRenderingContext2DPrototype>(realm, "CanvasRenderingContext2D"_string));
}

void CanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    CanvasState::visit_edges(visitor);
    visitor.visit(m_element);
}

void CanvasRenderingContext2D::set_shadow_color(String color)
{
    // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.
    auto& context = canvas_element();

    // 2. Let parsedValue be the result of parsing the given value with context if non-null.
    auto style_value = parse_css_value(CSS::Parser::ParsingParams(), color, CSS::PropertyID::Color);
    if (style_value && style_value->has_color()) {
        CSS::ColorResolutionContext color_resolution_context {};
        context.document().update_layout(DOM::UpdateLayoutReason::CanvasRenderingContext2DSetShadowColor);
        if (auto node = context.layout_node()) {
            color_resolution_context = CSS::ColorResolutionContext::for_layout_node_with_style(*node);
        }

        auto parsedValue = style_value->to_color(color_resolution_context).value_or(Color::Black);

        // 4. Set this's shadow color to parsedValue.
        drawing_state().shadow_color = parsedValue;
    } else {
        // 3. If parsedValue is failure, then return.
        return;
    }
}

void CanvasRenderingContext2D::set_filter(String filter)
{

    drawing_state().filter.clear();

    // 1. If the given value is "none", then set this's current filter to "none" and return.
    if (filter == "none"sv) {
        drawing_state().filter_string.clear();
        return;
    }

    auto& realm = static_cast<CanvasRenderingContext2D&>(*this).realm();
    auto parser = CSS::Parser::Parser::create(CSS::Parser::ParsingParams(realm), filter);

    // 2. Let parsedValue be the result of parsing the given values as a <filter-value-list>.
    //    If any property-independent style sheet syntax like 'inherit' or 'initial' is present,
    //    then this parsing must return failure.
    auto style_value = parser.parse_as_css_value(CSS::PropertyID::Filter);

    if (style_value && style_value->is_filter_value_list()) {
        // Note: The layout must be updated to make sure the canvas's layout node isn't null.
        canvas_element().document().update_layout(DOM::UpdateLayoutReason::CanvasRenderingContext2DSetFilter);
        auto layout_node = canvas_element().layout_node();

        CSS::ComputationContext computation_context {
            .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(*layout_node),
            .abstract_element = DOM::AbstractElement { canvas_element() },
            .color_scheme = layout_node->computed_values().color_scheme(),
        };
        auto filter_value_list = style_value->absolutized(computation_context)->as_filter_value_list().filter_value_list();

        // 4. Set this's current filter to the given value.
        for (auto& item : filter_value_list) {
            // FIXME: Add support for SVG filters when they get implement by the CSS parser.
            item.visit(
                [&](CSS::FilterOperation::Blur const& blur_filter) {
                    float radius = blur_filter.resolved_radius();
                    auto new_filter = Gfx::Filter::blur(radius, radius);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::FilterOperation::Color const& color) {
                    float amount = color.resolved_amount();
                    auto new_filter = Gfx::Filter::color(color.operation, amount);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::FilterOperation::HueRotate const& hue_rotate) {
                    float angle = hue_rotate.angle_degrees();
                    auto new_filter = Gfx::Filter::hue_rotate(angle);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::FilterOperation::DropShadow const& drop_shadow) {
                    float offset_x = static_cast<float>(CSS::Length::from_style_value(drop_shadow.offset_x, {}).absolute_length_to_px());
                    float offset_y = static_cast<float>(CSS::Length::from_style_value(drop_shadow.offset_y, {}).absolute_length_to_px());

                    float radius = 0.0f;
                    if (drop_shadow.radius) {
                        radius = static_cast<float>(CSS::Length::from_style_value(*drop_shadow.radius, {}).absolute_length_to_px());
                    };

                    auto color_context = CSS::ColorResolutionContext::for_layout_node_with_style(*layout_node);
                    auto color = drop_shadow.color
                        ? drop_shadow.color->to_color(color_context).value_or(Gfx::Color::Black)
                        : Gfx::Color::Black;

                    auto new_filter = Gfx::Filter::drop_shadow(offset_x, offset_y, radius, color);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::URL const& url) {
                    (void)url;
                    // FIXME: Resolve the SVG filter
                    dbgln("FIXME: SVG filters are not implemented for Canvas2D");
                });
        }

        drawing_state().filter_string = move(filter);
    }

    // 3. If parsedValue is failure, then return.
}

void CanvasRenderingContext2D::did_draw(Gfx::FloatRect const&)
{
    // FIXME: Make use of the rect to reduce the invalidated area when possible.
    if (!canvas_element().paintable())
        return;
    canvas_element().paintable()->set_needs_display(InvalidateDisplayList::No);
}

Gfx::Painter* CanvasRenderingContext2D::painter()
{
    allocate_painting_surface_if_needed();
    auto surface = canvas_element().surface();
    if (!m_painter && surface) {
        canvas_element().document().invalidate_display_list();
        m_painter = make<Gfx::PainterSkia>(*canvas_element().surface());
    }
    return m_painter.ptr();
}

void CanvasRenderingContext2D::allocate_painting_surface_if_needed()
{
    if (m_surface || m_size.is_empty())
        return;

    // FIXME: implement context attribute .color_space
    // FIXME: implement context attribute .color_type
    // FIXME: implement context attribute .desynchronized
    // FIXME: implement context attribute .will_read_frequently

    auto color_type = m_context_attributes.alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;

    auto skia_backend_context = canvas_element().navigable()->traversable_navigable()->skia_backend_context();
    m_surface = Gfx::PaintingSurface::create_with_size(skia_backend_context, canvas_element().bitmap_size_for_canvas(), color_type, Gfx::AlphaType::Premultiplied);
    m_painter = nullptr;

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    // Thus, the bitmap of such a context starts off as opaque black instead of transparent black;
    // AD-HOC: Skia provides us with a full transparent surface by default; only clear the surface if alpha is disabled.
    if (!m_context_attributes.alpha) {
        auto* painter = this->painter();
        painter->clear_rect(m_surface->rect().to_type<float>(), clear_color());
    }
}

}
