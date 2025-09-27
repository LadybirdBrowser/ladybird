/*
 * Copyright (c) 2025, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
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

void SVGFEColorMatrixElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, new_value, namespace_);
    // No-op; leave to higher-level handlers.
}

GC::Ref<SVGAnimatedString> SVGFEColorMatrixElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in, {}, "SourceGraphic"_fly_string);
    return *m_in1;
}

GC::Ref<SVGAnimatedEnumeration> SVGFEColorMatrixElement::type() const
{
    // https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEColorMatrixElement
    // Map the 'type' attribute to the IDL enumeration values.
    // Defaults to MATRIX when omitted.
    auto type_attribute = attribute(AttributeNames::type).value_or(String {});

    u16 enum_value = 0; // SVG_FECOLORMATRIX_TYPE_UNKNOWN
    if (type_attribute.is_empty() || type_attribute.equals_ignoring_ascii_case("matrix"sv))
        enum_value = 1; // SVG_FECOLORMATRIX_TYPE_MATRIX
    else if (type_attribute.equals_ignoring_ascii_case("saturate"sv))
        enum_value = 2; // SVG_FECOLORMATRIX_TYPE_SATURATE
    else if (type_attribute.equals_ignoring_ascii_case("hueRotate"sv))
        enum_value = 3; // SVG_FECOLORMATRIX_TYPE_HUEROTATE
    else if (type_attribute.equals_ignoring_ascii_case("luminanceToAlpha"sv))
        enum_value = 4; // SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA

    return SVGAnimatedEnumeration::create(realm(), enum_value);
}

GC::Ref<SVGAnimatedString> SVGFEColorMatrixElement::values()
{
    if (!m_values)
        m_values = SVGAnimatedString::create(realm(), *this, AttributeNames::values);
    return *m_values;
}

}
