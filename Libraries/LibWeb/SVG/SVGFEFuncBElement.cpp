/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEFuncBElementPrototype.h>
#include <LibWeb/SVG/SVGFEFuncBElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFuncBElement);

SVGFEFuncBElement::SVGFEFuncBElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGComponentTransferFunctionElement(document, qualified_name)
{
}

void SVGFEFuncBElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEFuncBElement);
    Base::initialize(realm);
}

}
