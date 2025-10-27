/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEOffsetElementPrototype.h>
#include <LibWeb/SVG/SVGFEOffsetElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEOffsetElement);

SVGFEOffsetElement::SVGFEOffsetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEOffsetElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEOffsetElement);
    Base::initialize(realm);
}

void SVGFEOffsetElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_dx);
    visitor.visit(m_dy);
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgfeoffsetelement-in1
GC::Ref<SVGAnimatedString> SVGFEOffsetElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgfeoffsetelement-dx
GC::Ref<SVGAnimatedNumber> SVGFEOffsetElement::dx()
{
    if (!m_dx) {
        m_dx = SVGAnimatedNumber::create(realm(), *this, AttributeNames::dx, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    }
    return *m_dx;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgfeoffsetelement-dy
GC::Ref<SVGAnimatedNumber> SVGFEOffsetElement::dy()
{
    if (!m_dy) {
        m_dy = SVGAnimatedNumber::create(realm(), *this, AttributeNames::dy, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    }
    return *m_dy;
}

}
