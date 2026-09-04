/*
 * Copyright (c) 2025, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGFEColorMatrixElement.h>
#include <LibWeb/SVG/SVGNumber.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEColorMatrixElement);

SVGFEColorMatrixElement::SVGFEColorMatrixElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEColorMatrixElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_values);
}

void SVGFEColorMatrixElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // FIXME: Support reflection instead of invalidating the list.
    if (name == AttributeNames::values)
        m_values = {};
}

GC::Ref<SVGAnimatedString> SVGFEColorMatrixElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });
    return *m_in1;
}

GC::Ref<SVGAnimatedEnumeration> SVGFEColorMatrixElement::type() const
{
    // https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEColorMatrixElement
    // Map the 'type' attribute to the IDL enumeration values.
    // Defaults to MATRIX when omitted.
    auto type_attribute = attribute(AttributeNames::type).value_or({});

    u16 enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_UNKNOWN;
    if (type_attribute.is_empty() || type_attribute.equals_ignoring_ascii_case("matrix"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_MATRIX;
    else if (type_attribute.equals_ignoring_ascii_case("saturate"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_SATURATE;
    else if (type_attribute.equals_ignoring_ascii_case("hueRotate"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_HUEROTATE;
    else if (type_attribute.equals_ignoring_ascii_case("luminanceToAlpha"sv))
        enum_value = SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA;

    return SVGAnimatedEnumeration::create(enum_value);
}

GC::Ref<SVGAnimatedNumberList> SVGFEColorMatrixElement::values()
{
    if (!m_values) {
        auto numbers = parse_table_values(get_attribute_value(AttributeNames::values));

        auto items = GC::Heap::the().allocate<SVGNumberList::List>();
        items->elements().ensure_capacity(numbers.size());
        for (auto number : numbers)
            items->elements().unchecked_append(SVGNumber::create(number, SVGNumber::ReadOnly::Yes));

        auto number_list = SVGNumberList::create(items, ReadOnlyList::Yes);
        m_values = SVGAnimatedNumberList::create(number_list);
    }

    return *m_values;
}

}
