/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEFuncRElementPrototype.h>
#include <LibWeb/SVG/SVGFEFuncRElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFuncRElement);

SVGFEFuncRElement::SVGFEFuncRElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGComponentTransferFunctionElement(document, qualified_name)
{
}

void SVGFEFuncRElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEFuncRElement);
    Base::initialize(realm);
}

}
