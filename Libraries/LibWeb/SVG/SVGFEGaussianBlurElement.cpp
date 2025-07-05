/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SVGAnimatedEnumeration.h"

#include <LibWeb/Bindings/SVGFEGaussianBlurElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEGaussianBlurElement);

SVGFEGaussianBlurElement::SVGFEGaussianBlurElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEGaussianBlurElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEGaussianBlurElement);
}

void SVGFEGaussianBlurElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
}

GC::Ref<SVGAnimatedString> SVGFEGaussianBlurElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

GC::Ref<SVGAnimatedNumber> SVGFEGaussianBlurElement::std_deviation_x() const
{
    return SVGAnimatedNumber::create(realm(), 125.0f, 125.0f);
}

GC::Ref<SVGAnimatedNumber> SVGFEGaussianBlurElement::std_deviation_y() const
{
    return SVGAnimatedNumber::create(realm(), 125.0f, 125.0f);
}

GC::Ref<SVGAnimatedEnumeration> SVGFEGaussianBlurElement::edge_mode() const
{
    return SVGAnimatedEnumeration::create(realm(), 0);
}

}
