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

// https://drafts.fxtf.org/filter-effects/#dom-svgfecolormatrixelement-type
GC::Ref<SVGAnimatedEnumeration> SVGFEColorMatrixElement::type()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "matrix"sv, SVG_FECOLORMATRIX_TYPE_MATRIX },
        SVGAnimatedEnumeration::Keyword { "saturate"sv, SVG_FECOLORMATRIX_TYPE_SATURATE },
        SVGAnimatedEnumeration::Keyword { "hueRotate"sv, SVG_FECOLORMATRIX_TYPE_HUEROTATE },
        SVGAnimatedEnumeration::Keyword { "luminanceToAlpha"sv, SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::type, keywords, SVG_FECOLORMATRIX_TYPE_MATRIX);
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
