/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEFuncAElement.h>
#include <LibWeb/SVG/SVGFEFuncAElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFuncAElement);

SVGFEFuncAElement::SVGFEFuncAElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGComponentTransferFunctionElement(document, qualified_name)
{
}

}
