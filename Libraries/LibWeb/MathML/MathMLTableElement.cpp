/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLTableBox.h>
#include <LibWeb/MathML/MathMLTableElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLTableElement);

MathMLTableElement::MathMLTableElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLTableElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLTableBox>(document(), *this, move(style));
}

}
