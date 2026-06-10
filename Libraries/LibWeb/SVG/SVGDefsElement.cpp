/*
 * Copyright (c) 2022, Simon Danner <danner.simon@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGBox.h>
#include <LibWeb/SVG/SVGDefsElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGDefsElement);

SVGDefsElement::SVGDefsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGDefsElement::~SVGDefsElement()
{
}

}
