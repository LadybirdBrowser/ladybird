/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFEDistantLightElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEDistantLightElement);

SVGFEDistantLightElement::SVGFEDistantLightElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEDistantLightElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_azimuth);
    visitor.visit(m_elevation);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfedistantlightelement-azimuth
GC::Ref<SVGAnimatedNumber> SVGFEDistantLightElement::azimuth()
{
    if (!m_azimuth)
        m_azimuth = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::azimuth, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_azimuth;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfedistantlightelement-elevation
GC::Ref<SVGAnimatedNumber> SVGFEDistantLightElement::elevation()
{
    if (!m_elevation)
        m_elevation = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::elevation, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_elevation;
}

}
