/*
 * Copyright (c) 2025, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEColorMatrixElementPrototype.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGFEColorMatrixElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEColorMatrixElement);

SVGFEColorMatrixElement::SVGFEColorMatrixElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEColorMatrixElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEColorMatrixElement);
    Base::initialize(realm);
}

void SVGFEColorMatrixElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_values);
}

GC::Ref<SVGAnimatedString> SVGFEColorMatrixElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });
    return *m_in1;
}

GC::Ref<SVGAnimatedEnumeration> SVGFEColorMatrixElement::type() const
{
    // https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEColorMatrixElement
    // Map the 'type' attribute to the IDL enumeration values.
    // Defaults to MATRIX when omitted.
    auto type_attribute = attribute(AttributeNames::type).value_or(String {});

    u16 enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_UNKNOWN;
    if (type_attribute.is_empty() || type_attribute.equals_ignoring_ascii_case("matrix"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_MATRIX;
    else if (type_attribute.equals_ignoring_ascii_case("saturate"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_SATURATE;
    else if (type_attribute.equals_ignoring_ascii_case("hueRotate"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_HUEROTATE;
    else if (type_attribute.equals_ignoring_ascii_case("luminanceToAlpha"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA;

    return SVGAnimatedEnumeration::create(realm(), enum_value);
}

GC::Ref<SVGAnimatedString> SVGFEColorMatrixElement::values()
{
    if (!m_values)
        m_values = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::values, OptionalNone {}, OptionalNone {} });
    return *m_values;
}

}
