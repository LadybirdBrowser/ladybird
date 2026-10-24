/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFEDiffuseLightingElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEDiffuseLightingElement);

SVGFEDiffuseLightingElement::SVGFEDiffuseLightingElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEDiffuseLightingElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_surface_scale);
    visitor.visit(m_diffuse_constant);
    visitor.visit(m_kernel_unit_length_x);
    visitor.visit(m_kernel_unit_length_y);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfediffuselightingelement-in1
GC::Ref<SVGAnimatedString> SVGFEDiffuseLightingElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });
    return *m_in1;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfediffuselightingelement-surfacescale
GC::Ref<SVGAnimatedNumber> SVGFEDiffuseLightingElement::surface_scale()
{
    if (!m_surface_scale)
        m_surface_scale = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::surfaceScale, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_surface_scale;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfediffuselightingelement-diffuseconstant
GC::Ref<SVGAnimatedNumber> SVGFEDiffuseLightingElement::diffuse_constant()
{
    if (!m_diffuse_constant)
        m_diffuse_constant = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::diffuseConstant, OptionalNone {}, OptionalNone {} }, 1.f);
    return *m_diffuse_constant;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfediffuselightingelement-kernelunitlengthx
GC::Ref<SVGAnimatedNumber> SVGFEDiffuseLightingElement::kernel_unit_length_x()
{
    if (!m_kernel_unit_length_x)
        m_kernel_unit_length_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::kernelUnitLength, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    return *m_kernel_unit_length_x;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfediffuselightingelement-kernelunitlengthy
GC::Ref<SVGAnimatedNumber> SVGFEDiffuseLightingElement::kernel_unit_length_y()
{
    if (!m_kernel_unit_length_y)
        m_kernel_unit_length_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::kernelUnitLength, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    return *m_kernel_unit_length_y;
}

}
