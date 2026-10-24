/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEGaussianBlurElement);

SVGFEGaussianBlurElement::SVGFEGaussianBlurElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEGaussianBlurElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_std_deviation_x);
    visitor.visit(m_std_deviation_y);
}

GC::Ref<SVGAnimatedString> SVGFEGaussianBlurElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });

    return *m_in1;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fegaussianblur-stddeviation
GC::Ref<SVGAnimatedNumber> SVGFEGaussianBlurElement::std_deviation_x()
{
    if (!m_std_deviation_x) {
        m_std_deviation_x = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::stdDeviation, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    }
    return *m_std_deviation_x;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fegaussianblur-stddeviation
GC::Ref<SVGAnimatedNumber> SVGFEGaussianBlurElement::std_deviation_y()
{
    if (!m_std_deviation_y) {
        m_std_deviation_y = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::stdDeviation, OptionalNone {}, OptionalNone {} }, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    }
    return *m_std_deviation_y;
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfegaussianblurelement-edgemode
GC::Ref<SVGAnimatedEnumeration> SVGFEGaussianBlurElement::edge_mode()
{
    static constexpr Array keywords {
        SVGAnimatedEnumeration::Keyword { "duplicate"sv, SVG_EDGEMODE_DUPLICATE },
        SVGAnimatedEnumeration::Keyword { "wrap"sv, SVG_EDGEMODE_WRAP },
        SVGAnimatedEnumeration::Keyword { "none"sv, SVG_EDGEMODE_NONE },
    };
    return svg_animated_enumeration_for_attribute(AttributeNames::edgeMode, keywords, SVG_EDGEMODE_NONE);
}

// https://drafts.fxtf.org/filter-effects/#dom-svgfegaussianblurelement-setstddeviation
void SVGFEGaussianBlurElement::set_std_deviation(float std_deviation_x, float std_deviation_y)
{
    // Sets the values for attribute stdDeviation.
    //
    // stdDeviationX
    //     The X component of attribute stdDeviation.
    //
    // stdDeviationY
    //     The Y component of attribute stdDeviation.
    set_attribute_value(AttributeNames::stdDeviation, Utf16String::formatted("{} {}", std_deviation_x, std_deviation_y));
}

}
