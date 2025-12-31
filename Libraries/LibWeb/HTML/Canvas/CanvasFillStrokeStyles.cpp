/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CanvasFillStrokeStyles.h"
#include <AK/String.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

template<typename IncludingClass>
void CanvasFillStrokeStyles<IncludingClass>::set_fill_style(FillOrStrokeStyleVariant style)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fillstyle
    style.visit(
        // 1. If the given value is a string, then:
        [&](String const& string) {
            // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.
            HTMLCanvasElement* context = my_canvas_element().visit(
                [&](HTMLCanvasElement* canvas_element) -> HTMLCanvasElement* {
                    return canvas_element;
                },
                [&](OffscreenCanvas*) -> HTMLCanvasElement* {
                    return nullptr;
                });

            // 2. Let parsedValue be the result of parsing the given value with context if non-null.
            // FIXME: Parse a color value
            // https://drafts.csswg.org/css-color/#parse-a-css-color-value
            auto style_value = parse_css_value(CSS::Parser::ParsingParams(), string, CSS::PropertyID::Color);
            if (style_value && style_value->has_color()) {
                CSS::ColorResolutionContext color_resolution_context {};

                if (context) {
                    context->document().update_layout(DOM::UpdateLayoutReason::CanvasRenderingContext2DSetFillStyle);
                    if (context->layout_node())
                        color_resolution_context = CSS::ColorResolutionContext::for_layout_node_with_style(*context->layout_node());
                }

                auto parsedValue = style_value->to_color(color_resolution_context).value_or(Color::Black);

                // 4. Set this's fill style to parsedValue.
                my_drawing_state().fill_style = parsedValue;
            } else {
                // 3. If parsedValue is failure, then return.
                return;
            }

            // 5. Return.
            return;
        },
        [&](auto fill_or_stroke_style) {
            // FIXME: 2. If the given value is a CanvasPattern object that is marked as not origin-clean, then set this's origin-clean flag to false.

            // 3. Set this's fill style to the given value.
            my_drawing_state().fill_style = GC::Ref { *fill_or_stroke_style };
        });
}

template<typename IncludingClass>
CanvasFillStrokeStyles<IncludingClass>::FillOrStrokeStyleVariant CanvasFillStrokeStyles<IncludingClass>::fill_style() const
{
    return my_drawing_state().fill_style.to_js_fill_or_stroke_style();
}

template<typename IncludingClass>
void CanvasFillStrokeStyles<IncludingClass>::set_stroke_style(FillOrStrokeStyleVariant style)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-strokestyle

    style.visit(
        // 1. If the given value is a string, then:
        [&](String const& string) {
            // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.
            HTMLCanvasElement* context = my_canvas_element().visit(
                [&](HTMLCanvasElement* canvas_element) -> HTMLCanvasElement* {
                    return canvas_element;
                },
                [&](OffscreenCanvas*) -> HTMLCanvasElement* {
                    return nullptr;
                });

            // 2. Let parsedValue be the result of parsing the given value with context if non-null.
            // FIXME: Parse a color value
            // https://drafts.csswg.org/css-color/#parse-a-css-color-value
            auto style_value = parse_css_value(CSS::Parser::ParsingParams(), string, CSS::PropertyID::Color);
            if (style_value && style_value->has_color()) {
                CSS::ColorResolutionContext color_resolution_context {};

                if (context) {
                    context->document().update_layout(DOM::UpdateLayoutReason::CanvasRenderingContext2DSetStrokeStyle);
                    if (context->layout_node())
                        color_resolution_context = CSS::ColorResolutionContext::for_layout_node_with_style(*context->layout_node());
                }

                auto parsedValue = style_value->to_color(color_resolution_context).value_or(Color::Black);

                // 4. Set this's stroke style to parsedValue.
                my_drawing_state().stroke_style = parsedValue;
            } else {
                // 3. If parsedValue is failure, then return.
                return;
            }

            // 5. Return.
            return;
        },
        [&](auto fill_or_stroke_style) {
            // FIXME: 2. If the given value is a CanvasPattern object that is marked as not origin-clean, then set this's origin-clean flag to false.

            // 3. Set this's stroke style to the given value.
            my_drawing_state().fill_style = GC::Ref { *fill_or_stroke_style };
        });
}

template<typename IncludingClass>
CanvasFillStrokeStyles<IncludingClass>::FillOrStrokeStyleVariant CanvasFillStrokeStyles<IncludingClass>::stroke_style() const
{
    return my_drawing_state().stroke_style.to_js_fill_or_stroke_style();
}
template<typename IncludingClass>
Variant<HTMLCanvasElement*, OffscreenCanvas*> CanvasFillStrokeStyles<IncludingClass>::my_canvas_element()
{
    return &static_cast<IncludingClass&>(*this).canvas_element();
}

template<typename IncludingClass>
CanvasState::DrawingState& CanvasFillStrokeStyles<IncludingClass>::my_drawing_state()
{
    return static_cast<IncludingClass&>(*this).drawing_state();
}

template<typename IncludingClass>
CanvasState::DrawingState const& CanvasFillStrokeStyles<IncludingClass>::my_drawing_state() const
{
    return static_cast<IncludingClass const&>(*this).drawing_state();
}

template<typename IncludingClass>
WebIDL::ExceptionOr<GC::Ref<CanvasGradient>> CanvasFillStrokeStyles<IncludingClass>::create_radial_gradient(double x0, double y0, double r0, double x1, double y1, double r1)
{
    auto& realm = static_cast<IncludingClass&>(*this).realm();
    return CanvasGradient::create_radial(realm, x0, y0, r0, x1, y1, r1);
}
template<typename IncludingClass>
GC::Ref<CanvasGradient> CanvasFillStrokeStyles<IncludingClass>::create_linear_gradient(double x0, double y0, double x1, double y1)
{
    auto& realm = static_cast<IncludingClass&>(*this).realm();
    return CanvasGradient::create_linear(realm, x0, y0, x1, y1).release_value_but_fixme_should_propagate_errors();
}

template<typename IncludingClass>
GC::Ref<CanvasGradient> CanvasFillStrokeStyles<IncludingClass>::create_conic_gradient(double start_angle, double x, double y)
{
    auto& realm = static_cast<IncludingClass&>(*this).realm();
    return CanvasGradient::create_conic(realm, start_angle, x, y).release_value_but_fixme_should_propagate_errors();
}

template<typename IncludingClass>
WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> CanvasFillStrokeStyles<IncludingClass>::create_pattern(CanvasImageSource const& image, StringView repetition)
{
    auto& realm = static_cast<IncludingClass&>(*this).realm();
    return CanvasPattern::create(realm, image, repetition);
}

template class CanvasFillStrokeStyles<CanvasRenderingContext2D>;
template class CanvasFillStrokeStyles<OffscreenCanvasRenderingContext2D>;

}
