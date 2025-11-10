/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLPhantomBox.h>
#include <LibWeb/MathML/MathMLPhantomElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLPhantomElement);

MathMLPhantomElement::MathMLPhantomElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLPhantomElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLPhantomBox>(document(), *this, move(style));
}

}
