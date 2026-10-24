/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFESpotLightElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFESpotLightElement);

SVGFESpotLightElement::SVGFESpotLightElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFESpotLightElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_z);
    visitor.visit(m_points_at_x);
    visitor.visit(m_points_at_y);
    visitor.visit(m_points_at_z);
    visitor.visit(m_specular_exponent);
    visitor.visit(m_limiting_cone_angle);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-x
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::x()
{
    if (!m_x)
        m_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::x, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-y
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::y()
{
    if (!m_y)
        m_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::y, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-z
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::z()
{
    if (!m_z)
        m_z = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::z, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_z;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-pointsatx
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::points_at_x()
{
    if (!m_points_at_x)
        m_points_at_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::pointsAtX, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_points_at_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-pointsaty
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::points_at_y()
{
    if (!m_points_at_y)
        m_points_at_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::pointsAtY, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_points_at_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-pointsatz
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::points_at_z()
{
    if (!m_points_at_z)
        m_points_at_z = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::pointsAtZ, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_points_at_z;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-specularexponent
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::specular_exponent()
{
    if (!m_specular_exponent)
        m_specular_exponent = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::specularExponent, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_specular_exponent;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespotlightelement-limitingconeangle
GC::Ref<SVGAnimatedNumber> SVGFESpotLightElement::limiting_cone_angle()
{
    if (!m_limiting_cone_angle)
        m_limiting_cone_angle = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::limitingConeAngle, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_limiting_cone_angle;
}

}
