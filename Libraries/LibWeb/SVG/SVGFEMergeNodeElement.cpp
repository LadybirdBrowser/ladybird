/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEMergeNodeElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGFEMergeNodeElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEMergeNodeElement);

SVGFEMergeNodeElement::SVGFEMergeNodeElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEMergeNodeElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEMergeNodeElement);
    Base::initialize(realm);
}

void SVGFEMergeNodeElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_in1);
}

GC::Ref<SVGAnimatedString> SVGFEMergeNodeElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, AttributeNames::in);

    return *m_in1;
}

}
