/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEComponentTransferElementPrototype.h>
#include <LibWeb/SVG/SVGFEComponentTransferElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEComponentTransferElement);

SVGFEComponentTransferElement::SVGFEComponentTransferElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEComponentTransferElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEComponentTransferElement);
    Base::initialize(realm);
}

void SVGFEComponentTransferElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
}

GC::Ref<SVGAnimatedString> SVGFEComponentTransferElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, {}, {} });
    return *m_in1;
}

}
