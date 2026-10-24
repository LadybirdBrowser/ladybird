/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFESpecularLightingElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFESpecularLightingElement);

SVGFESpecularLightingElement::SVGFESpecularLightingElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFESpecularLightingElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_surface_scale);
    visitor.visit(m_specular_constant);
    visitor.visit(m_specular_exponent);
    visitor.visit(m_kernel_unit_length_x);
    visitor.visit(m_kernel_unit_length_y);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespecularlightingelement-in1
GC::Ref<SVGAnimatedString> SVGFESpecularLightingElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });
    return *m_in1;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespecularlightingelement-surfacescale
GC::Ref<SVGAnimatedNumber> SVGFESpecularLightingElement::surface_scale()
{
    if (!m_surface_scale)
        m_surface_scale = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::surfaceScale, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_surface_scale;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespecularlightingelement-specularconstant
GC::Ref<SVGAnimatedNumber> SVGFESpecularLightingElement::specular_constant()
{
    if (!m_specular_constant)
        m_specular_constant = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::specularConstant, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_specular_constant;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespecularlightingelement-specularexponent
GC::Ref<SVGAnimatedNumber> SVGFESpecularLightingElement::specular_exponent()
{
    if (!m_specular_exponent)
        m_specular_exponent = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::specularExponent, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_specular_exponent;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespecularlightingelement-kernelunitlengthx
GC::Ref<SVGAnimatedNumber> SVGFESpecularLightingElement::kernel_unit_length_x()
{
    if (!m_kernel_unit_length_x)
        m_kernel_unit_length_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::kernelUnitLength, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    return *m_kernel_unit_length_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfespecularlightingelement-kernelunitlengthy
GC::Ref<SVGAnimatedNumber> SVGFESpecularLightingElement::kernel_unit_length_y()
{
    if (!m_kernel_unit_length_y)
        m_kernel_unit_length_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::kernelUnitLength, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    return *m_kernel_unit_length_y;
}

}
