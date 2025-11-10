/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLPaddedBox.h>
#include <LibWeb/MathML/MathMLPaddedElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLPaddedElement);

MathMLPaddedElement::MathMLPaddedElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLPaddedElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLPaddedBox>(document(), *this, move(style));
}

}
