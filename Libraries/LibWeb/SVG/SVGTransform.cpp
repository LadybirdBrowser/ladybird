/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGTransformPrototype.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGTransform);

JS::NonnullGCPtr<SVGTransform> SVGTransform::create(JS::Realm& realm)
{
    return realm.heap().allocate<SVGTransform>(realm, realm);
}

SVGTransform::SVGTransform(JS::Realm& realm)
    : PlatformObject(realm) {};

SVGTransform::~SVGTransform() = default;

void SVGTransform::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTransform);
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__type
SVGTransform::Type SVGTransform::type()
{
    dbgln("FIXME: Implement SVGTransform::type()");
    return SVGTransform::Type::Unknown;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__angle
float SVGTransform::angle()
{
    dbgln("FIXME: Implement SVGTransform::angle()");
    return 0;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setTranslate
void SVGTransform::set_translate(float tx, float ty)
{
    (void)tx;
    (void)ty;
    dbgln("FIXME: Implement SVGTransform::set_translate(float tx, float ty)");
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setScale
void SVGTransform::set_scale(float sx, float sy)
{
    (void)sx;
    (void)sy;
    dbgln("FIXME: Implement SVGTransform::set_scale(float sx, float sy)");
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setRotate
void SVGTransform::set_rotate(float angle, float cx, float cy)
{
    (void)angle;
    (void)cx;
    (void)cy;
    dbgln("FIXME: Implement SVGTransform::set_rotate(float angle, float cx, float cy)");
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setSkewX
void SVGTransform::set_skew_x(float angle)
{
    (void)angle;
    dbgln("FIXME: Implement SVGTransform::set_skew_x(float angle)");
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setSkewY
void SVGTransform::set_skew_y(float angle)
{
    (void)angle;
    dbgln("FIXME: Implement SVGTransform::set_skew_y(float angle)");
}

}
