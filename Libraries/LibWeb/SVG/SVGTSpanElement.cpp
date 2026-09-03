/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/SVGTSpanElement.h>
#include <LibWeb/SVG/SVGTextElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTSpanElement);

SVGTSpanElement::SVGTSpanElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextPositioningElement(document, move(qualified_name))
{
}

Layout::Node* SVGTSpanElement::create_layout_node(CSS::LayoutStyle style)
{
    // Text must be within an SVG <text> element.
    if (first_flat_tree_ancestor_of_type<SVGTextElement>())
        return &Layout::allocate_layout_node<Layout::Box>(document(), *this, style, Layout::RustFFI::NodeKind::SVGTextBox);
    return nullptr;
}

}
