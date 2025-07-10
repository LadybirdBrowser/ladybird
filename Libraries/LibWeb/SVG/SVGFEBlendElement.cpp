/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEBlendElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEBlendElement);

SVGFEBlendElement::SVGFEBlendElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEBlendElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEBlendElement);
    Base::initialize(realm);
}

void SVGFEBlendElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_in2);
}

GC::Ref<SVGAnimatedString> SVGFEBlendElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

GC::Ref<SVGAnimatedString> SVGFEBlendElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(realm(), *this, AttributeNames::in2);

    return *m_in2;
}

GC::Ref<SVGAnimatedEnumeration> SVGFEBlendElement::mode() const
{
    // FIXME: Resolve the actual value from AttributeName::mode.
    return SVGAnimatedEnumeration::create(realm(), 1);
}

}
