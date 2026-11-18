/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/SVG/SVGTextElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTextElement);

SVGTextElement::SVGTextElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextPositioningElement(document, move(qualified_name))
{
}

CSS::ElementBoxKind SVGTextElement::box_kind() const
{
    // https://svgwg.org/svg2-draft/text.html#TextLayoutAlgorithm
    // The used value of the 'display' property on an element that is a descendant of a text content block element and
    // is not a correctly parented text content child element or 'a' element must be none.
    if (first_flat_tree_ancestor_of_type<SVGTextElement>())
        return CSS::ElementBoxKind::NoBox;
    return CSS::ElementBoxKind::SvgText;
}

}
