/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/MathMLSemanticsBox.h>
#include <LibWeb/MathML/MathMLSemanticsElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLSemanticsElement);

MathMLSemanticsElement::MathMLSemanticsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLSemanticsElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::MathMLSemanticsBox>(document(), *this, move(style));
}

}
