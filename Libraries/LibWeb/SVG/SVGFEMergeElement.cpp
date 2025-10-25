/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEMergeElementPrototype.h>
#include <LibWeb/SVG/SVGFEMergeElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEMergeElement);

SVGFEMergeElement::SVGFEMergeElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFEMergeElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEMergeElement);
    Base::initialize(realm);
}

void SVGFEMergeElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
}

}
