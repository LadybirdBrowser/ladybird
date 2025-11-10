/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLStyleBox.h>
#include <LibWeb/MathML/MathMLStyleElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLStyleElement);

MathMLStyleElement::MathMLStyleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLStyleElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLStyleBox>(document(), *this, move(style));
}

}
