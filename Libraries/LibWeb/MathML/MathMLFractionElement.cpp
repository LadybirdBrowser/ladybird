/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLFractionBox.h>
#include <LibWeb/MathML/MathMLFractionElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLFractionElement);

MathMLFractionElement::MathMLFractionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLFractionElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLFractionBox>(document(), *this, move(style));
}

}
