/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenity.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEDropShadowElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/SVG/SVGFEDropShadowElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEDropShadowElement);

SVGFEDropShadowElement::SVGFEDropShadowElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEDropShadowElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEDropShadowElement);
    Base::initialize(realm);
}

void SVGFEDropShadowElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_dx);
    visitor.visit(m_dy);
    visitor.visit(m_std_deviation_x);
    visitor.visit(m_std_deviation_y);
}

// https://www.w3.org/TR/filter-effects-1/#FloodColorProperty
Gfx::Color SVGFEDropShadowElement::flood_color()
{
    VERIFY(computed_properties());
    return computed_properties()->color_or_fallback(CSS::PropertyID::FloodColor, CSS::ColorResolutionContext::for_element({ *this }), CSS::InitialValues::flood_color()).resolved();
}

// https://www.w3.org/TR/filter-effects-1/#FloodOpacityProperty
float SVGFEDropShadowElement::flood_opacity() const
{
    VERIFY(computed_properties());
    return computed_properties()->flood_opacity();
}

// https://drafts.csswg.org/filter-effects-1/#dom-svgfedropshadowelement-in1
GC::Ref<SVGAnimatedString> SVGFEDropShadowElement::in1()
{
    // Corresponds to attribute in on the given feDropShadow element.
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, {}, {} });
    return *m_in1;
}

// https://drafts.csswg.org/filter-effects-1/#dom-svgfedropshadowelement-dx
// https://drafts.csswg.org/filter-effects-1/#element-attrdef-fedropshadow-dx
GC::Ref<SVGAnimatedNumber> SVGFEDropShadowElement::dx()
{
    // Corresponds to attribute dx on the given feDropShadow element.
    // The initial value for dx is 2.
    if (!m_dx)
        m_dx = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::dx, {}, {} }, 2);
    return *m_dx;
}

// https://drafts.csswg.org/filter-effects-1/#dom-svgfedropshadowelement-dy
// https://drafts.csswg.org/filter-effects-1/#element-attrdef-fedropshadow-dy
GC::Ref<SVGAnimatedNumber> SVGFEDropShadowElement::dy()
{
    // Corresponds to attribute dy on the given feDropShadow element.
    // The initial value for dy is 2.
    if (!m_dy)
        m_dy = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::dy, {}, {} }, 2);
    return *m_dy;
}

// https://drafts.csswg.org/filter-effects-1/#dom-svgfedropshadowelement-stddeviationx
// https://drafts.csswg.org/filter-effects-1/#element-attrdef-fedropshadow-stddeviation
GC::Ref<SVGAnimatedNumber> SVGFEDropShadowElement::std_deviation_x()
{
    // Corresponds to attribute stdDeviation on the given feDropShadow element. Contains the X component of attribute stdDeviation.
    // The initial value for stdDeviation is 2.
    if (!m_std_deviation_x)
        m_std_deviation_x = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::stdDeviation, {}, {} }, 2,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);

    return *m_std_deviation_x;
}

// https://drafts.csswg.org/filter-effects-1/#dom-svgfedropshadowelement-stddeviationy
// https://drafts.csswg.org/filter-effects-1/#element-attrdef-fedropshadow-stddeviation
GC::Ref<SVGAnimatedNumber> SVGFEDropShadowElement::std_deviation_y()
{
    // Corresponds to attribute stdDeviation on the given feDropShadow element. Contains the Y component of attribute stdDeviation.
    // The initial value for stdDeviation is 2.
    if (!m_std_deviation_y)
        m_std_deviation_y = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::stdDeviation, {}, {} }, 2,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    return *m_std_deviation_y;
}

// https://drafts.csswg.org/filter-effects-1/#dom-svgfedropshadowelement-setstddeviation
void SVGFEDropShadowElement::set_std_deviation(float std_deviation_x, float std_deviation_y)
{
    // Sets the values for attribute stdDeviation.
    //
    // stdDeviationX
    //     The X component of attribute stdDeviation.
    //
    // stdDeviationY
    //     The Y component of attribute stdDeviation.
    set_attribute_value(AttributeNames::stdDeviation, MUST(String::formatted("{} {}", std_deviation_x, std_deviation_y)));
}

}
