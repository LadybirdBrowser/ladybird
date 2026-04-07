/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sawyer Ramsey <sawyereramsey@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/AffineTransform.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGTransformPrototype.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTransform);

GC::Ref<SVGTransform> SVGTransform::create(JS::Realm& realm)
{
    return realm.create<SVGTransform>(realm);
}

SVGTransform::SVGTransform(JS::Realm& realm)
    : PlatformObject(realm)
{
}

SVGTransform::~SVGTransform() = default;

void SVGTransform::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTransform);
    Base::initialize(realm);
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__type
SVGTransform::Type SVGTransform::type()
{
    return m_type;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__angle
float SVGTransform::angle()
{
    return m_angle;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setTranslate
void SVGTransform::set_translate(float tx, float ty)
{
    m_matrix = Gfx::AffineTransform {}.translate(tx, ty);
    m_angle = 0;
    m_type = Type::Translate;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setScale
void SVGTransform::set_scale(float sx, float sy)
{
    m_matrix = Gfx::AffineTransform {}.scale(sx, sy);
    m_angle = 0;
    m_type = Type::Scale;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setRotate
void SVGTransform::set_rotate(float angle, float cx, float cy)
{
    m_matrix = Gfx::AffineTransform {}
                   .translate(cx, cy)
                   .rotate_radians(AK::to_radians(angle))
                   .translate(-cx, -cy);
    m_angle = angle;
    m_type = Type::Rotate;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setSkewX
void SVGTransform::set_skew_x(float angle)
{
    m_matrix = Gfx::AffineTransform {}.skew_radians(AK::to_radians(angle), 0);
    m_angle = angle;
    m_type = Type::SkewX;
}

// https://svgwg.org/svg2-draft/single-page.html#coords-__svg__SVGTransform__setSkewY
void SVGTransform::set_skew_y(float angle)
{
    m_matrix = Gfx::AffineTransform {}.skew_radians(0, AK::to_radians(angle));
    m_angle = angle;
    m_type = Type::SkewY;
}

}
