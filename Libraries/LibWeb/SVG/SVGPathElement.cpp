/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPathElement);

SVGPathElement::SVGPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, move(qualified_name))
{
}

}
