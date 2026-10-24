/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFEPointLightElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEPointLightElement);

SVGFEPointLightElement::SVGFEPointLightElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEPointLightElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_z);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfepointlightelement-x
GC::Ref<SVGAnimatedNumber> SVGFEPointLightElement::x()
{
    if (!m_x)
        m_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::x, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfepointlightelement-y
GC::Ref<SVGAnimatedNumber> SVGFEPointLightElement::y()
{
    if (!m_y)
        m_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::y, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfepointlightelement-z
GC::Ref<SVGAnimatedNumber> SVGFEPointLightElement::z()
{
    if (!m_z)
        m_z = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::z, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_z;
}

}
