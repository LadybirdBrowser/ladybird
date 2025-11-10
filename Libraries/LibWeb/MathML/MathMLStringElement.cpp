/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLStringBox.h>
#include <LibWeb/MathML/MathMLStringElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLStringElement);

MathMLStringElement::MathMLStringElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLStringElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLStringBox>(document(), *this, move(style));
}

}
