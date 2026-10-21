/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Color.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/HTML/CanvasGradient.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_canvas_gradient_create_linear(JSVM*, double x0, double y0, double x1, double y1);
void* web_canvas_gradient_create_radial(JSVM*, double x0, double y0, double r0, double x1, double y1, double r1);
void* web_canvas_gradient_create_conic(JSVM*, double start_angle, double x, double y);
bool web_canvas_gradient_is_visible(void* gradient);

// What the Rust implementation asks of LibWeb, declared in Bindings/Rust/src/html/canvas_gradient.rs.
bool web_canvas_gradient_parse_color(FlatPtr color, Gfx::Color* out_color);
}

static JSVM* vm()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

GC::Ref<CanvasGradient> CanvasGradient::create_linear(double x0, double y0, double x1, double y1)
{
    return *static_cast<CanvasGradient*>(web_canvas_gradient_create_linear(vm(), x0, y0, x1, y1));
}

GC::Ref<CanvasGradient> CanvasGradient::create_radial(double x0, double y0, double r0, double x1, double y1, double r1)
{
    return *static_cast<CanvasGradient*>(web_canvas_gradient_create_radial(vm(), x0, y0, r0, x1, y1, r1));
}

GC::Ref<CanvasGradient> CanvasGradient::create_conic(double start_angle, double x, double y)
{
    return *static_cast<CanvasGradient*>(web_canvas_gradient_create_conic(vm(), start_angle, x, y));
}

bool CanvasGradient::is_visible() const
{
    return web_canvas_gradient_is_visible(const_cast<CanvasGradient*>(this));
}

// https://drafts.csswg.org/css-color/#parse-a-css-color-value
bool web_canvas_gradient_parse_color(FlatPtr color, Gfx::Color* out_color)
{
    auto color_string = Utf16String::from_raw(color);
    auto const maybe_color = parse_css_value(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, color_string.utf16_view(), CSS::PropertyID::Color);
    if (maybe_color.is_null() || !maybe_color->has_color())
        return false;
    *out_color = maybe_color->to_color({}).value();
    return true;
}

}
