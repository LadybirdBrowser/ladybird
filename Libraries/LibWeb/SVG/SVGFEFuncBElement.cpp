/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEFuncBElement.h>
#include <LibWeb/SVG/SVGFEFuncBElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFuncBElement);

SVGFEFuncBElement::SVGFEFuncBElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGComponentTransferFunctionElement(document, qualified_name)
{
}

}
