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

SVGTSpanElement::SVGTSpanElement(DOM::RustMadePrefix prefix)
    : SVGTextPositioningElement(prefix)
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
