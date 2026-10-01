/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/SVG/SVGTSpanElement.h>
#include <LibWeb/SVG/SVGTextElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTSpanElement);

SVGTSpanElement::SVGTSpanElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextPositioningElement(document, move(qualified_name))
{
}

CSS::ElementBoxKind SVGTSpanElement::box_kind() const
{
    // Text must be within an SVG <text> element.
    if (first_flat_tree_ancestor_of_type<SVGTextElement>())
        return CSS::ElementBoxKind::SvgText;
    return CSS::ElementBoxKind::NoBox;
}

}
