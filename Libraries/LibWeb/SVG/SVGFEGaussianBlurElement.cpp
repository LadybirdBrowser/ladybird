/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEGaussianBlurElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEGaussianBlurElement);

SVGFEGaussianBlurElement::SVGFEGaussianBlurElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEGaussianBlurElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEGaussianBlurElement);
    Base::initialize(realm);
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
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fegaussianblur-stddeviation
GC::Ref<SVGAnimatedNumber> SVGFEGaussianBlurElement::std_deviation_x()
{
    if (!m_std_deviation_x) {
        m_std_deviation_x = SVGAnimatedNumber::create(realm(), *this, AttributeNames::stdDeviation, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::First);
    }
    return *m_std_deviation_x;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fegaussianblur-stddeviation
GC::Ref<SVGAnimatedNumber> SVGFEGaussianBlurElement::std_deviation_y()
{
    if (!m_std_deviation_y) {
        m_std_deviation_y = SVGAnimatedNumber::create(realm(), *this, AttributeNames::stdDeviation, 0.f,
            SVGAnimatedNumber::SupportsSecondValue::Yes, SVGAnimatedNumber::ValueRepresented::Second);
    }
    return *m_std_deviation_y;
}

GC::Ref<SVGAnimatedEnumeration> SVGFEGaussianBlurElement::edge_mode() const
{
    // FIXME: Resolve the actual value from AttributeNames::edgeMode.
    return SVGAnimatedEnumeration::create(realm(), 0);
}

}
