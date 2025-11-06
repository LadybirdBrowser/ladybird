/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLTableCellBox.h>
#include <LibWeb/MathML/MathMLTableCellElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLTableCellElement);

MathMLTableCellElement::MathMLTableCellElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLTableCellElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLTableCellBox>(document(), *this, move(style));
}

}
