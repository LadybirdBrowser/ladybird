/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEFuncGElementPrototype.h>
#include <LibWeb/SVG/SVGFEFuncGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFuncGElement);

SVGFEFuncGElement::SVGFEFuncGElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGComponentTransferFunctionElement(document, qualified_name)
{
}

void SVGFEFuncGElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEFuncGElement);
    Base::initialize(realm);
}

}
