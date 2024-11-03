/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedRectPrototype.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGAnimatedRect);

SVGAnimatedRect::SVGAnimatedRect(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

SVGAnimatedRect::~SVGAnimatedRect() = default;

void SVGAnimatedRect::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedRect);
    m_base_val = Geometry::DOMRect::create(realm, { 0, 0, 0, 0 });
    m_anim_val = Geometry::DOMRect::create(realm, { 0, 0, 0, 0 });
}

void SVGAnimatedRect::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

JS::GCPtr<Geometry::DOMRect> SVGAnimatedRect::base_val() const
{
    if (m_nulled)
        return nullptr;
    return m_base_val;
}

JS::GCPtr<Geometry::DOMRect> SVGAnimatedRect::anim_val() const
{
    if (m_nulled)
        return nullptr;
    return m_anim_val;
}

void SVGAnimatedRect::set_nulled(bool nulled)
{
    m_nulled = nulled;
}

void SVGAnimatedRect::set_base_val(Gfx::DoubleRect const& rect)
{
    m_base_val->set_x(rect.x());
    m_base_val->set_y(rect.y());
    m_base_val->set_width(rect.width());
    m_base_val->set_height(rect.height());
}

void SVGAnimatedRect::set_anim_val(Gfx::DoubleRect const& rect)
{
    m_anim_val->set_x(rect.x());
    m_anim_val->set_y(rect.y());
    m_anim_val->set_width(rect.width());
    m_anim_val->set_height(rect.height());
}

}
