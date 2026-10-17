/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGSetElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGSetElement);

SVGSetElement::SVGSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGAnimationElement(document, move(qualified_name))
{
}

}
