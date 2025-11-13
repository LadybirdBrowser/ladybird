/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLErrorBox.h>
#include <LibWeb/MathML/MathMLErrorElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLErrorElement);

MathMLErrorElement::MathMLErrorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLErrorElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLErrorBox>(document(), *this, move(style));
}

}
