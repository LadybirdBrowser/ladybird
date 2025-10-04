/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Bindings/CanvasGradientPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CanvasGradient);

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createradialgradient
WebIDL::ExceptionOr<GC::Ref<CanvasGradient>> CanvasGradient::create_radial(JS::Realm& realm, double x0, double y0, double r0, double x1, double y1, double r1)
{
    // If either of r0 or r1 are negative, then an "IndexSizeError" DOMException must be thrown.
    if (r0 < 0)
        return WebIDL::IndexSizeError::create(realm, "The r0 passed is less than 0"_utf16);
    if (r1 < 0)
        return WebIDL::IndexSizeError::create(realm, "The r1 passed is less than 0"_utf16);

    auto radial_gradient = TRY_OR_THROW_OOM(realm.vm(), Gfx::CanvasRadialGradientPaintStyle::create(Gfx::FloatPoint { x0, y0 }, r0, Gfx::FloatPoint { x1, y1 }, r1));
    return realm.create<CanvasGradient>(realm, *radial_gradient);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createlineargradient
WebIDL::ExceptionOr<GC::Ref<CanvasGradient>> CanvasGradient::create_linear(JS::Realm& realm, double x0, double y0, double x1, double y1)
{
    auto linear_gradient = TRY_OR_THROW_OOM(realm.vm(), Gfx::CanvasLinearGradientPaintStyle::create(Gfx::FloatPoint { x0, y0 }, Gfx::FloatPoint { x1, y1 }));
    return realm.create<CanvasGradient>(realm, *linear_gradient);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createconicgradient
WebIDL::ExceptionOr<GC::Ref<CanvasGradient>> CanvasGradient::create_conic(JS::Realm& realm, double start_angle, double x, double y)
{
    auto conic_gradient = TRY_OR_THROW_OOM(realm.vm(), Gfx::CanvasConicGradientPaintStyle::create(Gfx::FloatPoint { x, y }, start_angle));
    return realm.create<CanvasGradient>(realm, *conic_gradient);
}

CanvasGradient::CanvasGradient(JS::Realm& realm, Gfx::GradientPaintStyle& gradient)
    : PlatformObject(realm)
    , m_gradient(gradient)
{
}

CanvasGradient::~CanvasGradient() = default;

void CanvasGradient::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CanvasGradient);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvasgradient-addcolorstop
WebIDL::ExceptionOr<void> CanvasGradient::add_color_stop(double offset, StringView color)
{
    // 1. If the offset is less than 0 or greater than 1, then throw an "IndexSizeError" DOMException.
    if (offset < 0 || offset > 1)
        return WebIDL::IndexSizeError::create(realm(), "CanvasGradient color stop offset out of bounds"_utf16);

    // 2. Let parsed color be the result of parsing color.
    // https://drafts.csswg.org/css-color/#parse-a-css-color-value
    auto const maybe_color = parse_css_value(CSS::Parser::ParsingParams(), color, CSS::PropertyID::Color);

    // 3. If parsed color is failure, throw a "SyntaxError" DOMException.
    if (maybe_color.is_null() || !maybe_color->has_color())
        return WebIDL::SyntaxError::create(realm(), "Could not parse color for CanvasGradient"_utf16);

    auto const parsed_color = maybe_color->to_color({}).value();

    // 4. Place a new stop on the gradient, at offset offset relative to the whole gradient, and with the color parsed color.
    TRY_OR_THROW_OOM(realm().vm(), m_gradient->add_color_stop(offset, parsed_color));

    // FIXME: If multiple stops are added at the same offset on a gradient, then they must be placed in the order added,
    //        with the first one closest to the start of the gradient, and each subsequent one infinitesimally further along
    //        towards the end point (in effect causing all but the first and last stop added at each point to be ignored).

    return {};
}

}
