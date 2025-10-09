/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGAnimationElement.h>

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimationElementPrototype.h>
#include <LibWeb/Layout/Node.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimationElement);

SVGAnimationElement::SVGAnimationElement(DOM::Document& document, DOM::QualifiedName name)
    : SVGElement(document, move(name))
{
}

void SVGAnimationElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimationElement);
    Base::initialize(realm);
}

}
