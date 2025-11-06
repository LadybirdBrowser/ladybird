/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLTableRowBox.h>
#include <LibWeb/MathML/MathMLTableRowElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLTableRowElement);

MathMLTableRowElement::MathMLTableRowElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLTableRowElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLTableRowBox>(document(), *this, move(style));
}

}
